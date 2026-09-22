/* AmiSubsonic - eigener Abspieler, Stufe 0: messen.
 *
 * Hier steht vorerst NUR die Messung, wie schnell mpega.library auf
 * dieser Maschine dekodiert. Daran haengt alles Weitere: bleibt neben
 * dem Dekodieren genug Rechenzeit fuer TLS und die Oberflaeche?
 *
 * Gemessen wird ohne Ausgabe ueber AHI - sonst laege die Zeit an der
 * Soundkarte statt am Dekoder. Die Datei wird von mpega.library selbst
 * gelesen (bs_access = NULL); den Hook auf einen eigenen Ringpuffer
 * braucht erst die Netzfassung. */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <devices/ahi.h>
#include <devices/timer.h>
#include <proto/timer.h>

#include <stdio.h>
#include <string.h>

#include <libraries/mpega.h>
#include <inline/mpega.h>

#include <utility/hooks.h>

#include "audio.h"
#include "ring.h"

/* aacdec.h kennt keinen 68k und bricht sonst mit #error ab. ARDUINO ist
 * dort nur ein leerer Plattformzweig - die Rechenroutinen stehen in
 * assembly.h, das hier gar nicht eingebunden wird. */
#define ARDUINO
#include "vendor/helix-aac/aacdec.h"
#undef ARDUINO

extern long aac_state_size(void);       /* aacsize.c */

struct Library *MPEGABase = NULL;

/* Nur fuer ReadEClock - die gemeinsame Uhr von Audioprozess und
 * Visualizer. Geoeffnet in audio_start(), siehe dort. */
struct Device *TimerBase = NULL;
static struct timerequest g_eclock_req;

/* Der hoerbare Puffer fuer den Visualizer - erklaert bei vis_publish(). */
static WORD *volatile g_vis_buf = NULL;
static volatile LONG  g_vis_frames = 0;
static volatile LONG  g_vis_freq = 44100;
static volatile ULONG g_vis_t0 = 0;
static volatile ULONG g_vis_seq = 0;


static char g_why[160] = "";

const char *audio_last_error(void)
{
    return g_why[0] ? g_why : "no error";
}

/* Zeit in Hundertstelsekunden. DateStamp zaehlt in Ticks zu 1/50 s, das
 * reicht hier: gemessen werden Sekunden bis Minuten. */
static ULONG now_cs(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 6000UL
         + (ULONG)ds.ds_Tick * 2UL;
}

BOOL audio_probe_file(const char *path, struct AudioProbe *out)
{
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *mps;
    WORD *pcm[MPEGA_MAX_CHANNELS];
    WORD buf[MPEGA_MAX_CHANNELS][MPEGA_PCM_SIZE];
    ULONG t0, t1;
    LONG samples;

    memset(out, 0, sizeof(*out));
    g_why[0] = '\0';

    MPEGABase = OpenLibrary("mpega.library", 0);
    if (!MPEGABase) {
        strcpy(g_why, "mpega.library missing");
        return FALSE;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access = NULL;              /* Datei, nicht eigener Puffer */
    ctrl.check_mpeg = 1;
    ctrl.stream_buffer_size = 0;        /* Vorgabe */

    /* Volle Qualitaet: keine Halbierung der Rate, kein Zwangsmono. Das
     * Programm ist fuer schnelle Maschinen gedacht, und genau das soll
     * die Messung ja zeigen. */
    ctrl.layer_1_2.force_mono = 0;
    ctrl.layer_1_2.mono.freq_div = 1;
    ctrl.layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_1_2.stereo.freq_div = 1;
    ctrl.layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_3 = ctrl.layer_1_2;

    mps = MPEGA_open((char *)path, &ctrl);
    if (!mps) {
        strcpy(g_why, "MPEGA_open failed - not an MPEG audio file?");
        CloseLibrary(MPEGABase);
        MPEGABase = NULL;
        return FALSE;
    }

    out->freq     = mps->dec_frequency;
    out->channels = mps->dec_channels;
    out->bitrate  = mps->bitrate;
    out->layer    = mps->layer;
    out->ms_total = mps->ms_duration;

    pcm[0] = buf[0];
    pcm[1] = buf[1];

    t0 = now_cs();
    for (;;) {
        samples = MPEGA_decode_frame(mps, pcm);
        if (samples <= 0) {
            break;                      /* Dateiende oder Fehler */
        }
        out->frames++;
        out->samples += samples;
    }
    t1 = now_cs();

    out->cs_decode = t1 - t0;

    MPEGA_close(mps);
    CloseLibrary(MPEGABase);
    MPEGABase = NULL;
    return out->frames > 0;
}


/* ------------------------------------------------------------------ */
/* Stufe 1: Ausgabe ueber AHI                                          */
/* ------------------------------------------------------------------ */

/* Zwei Puffer im Wechsel. Waehrend AHI den einen spielt, wird der andere
 * gefuellt; verkettet werden sie ueber ahir_Link, damit zwischen beiden
 * keine Luecke entsteht.
 *
 * Groesse: 8192 Bilder je Puffer sind bei 44,1 kHz gut 0,19 s. Kleiner
 * hiesse mehr Wechsel, groesser traege Reaktion auf Stop und Pause.
 * Stereo, 16 Bit: 4 Bytes je Bild. */
#define AU_FRAMES  8192
#define AU_BYTES   (AU_FRAMES * 4)

struct AudioOut {
    struct MsgPort   *port;
    struct AHIRequest *req[2];
    BOOL   devopen;
    WORD  *buf[2];
    LONG   cur;
    BOOL   linked;      /* laeuft schon etwas? */
    BOOL   abort;       /* beim Aufraeumen abbrechen statt auslaufen */
};

/* Laufende Puffer zu Ende bringen (oder abbrechen, wenn o->abort). Das
 * schliesst NICHT das Geraet - der Audioprozess haelt es offen. */
static void out_drain(struct AudioOut *o)
{
    LONG i;

    if (o->linked) {
        /* Beide Anforderungen zu Ende bringen, sonst schreibt AHI noch
         * in Speicher, den wir gleich zurueckgeben. Abgebrochen wird nur,
         * wenn der Aufrufer es verlangt (Strg-C, spaeter STOP) - sonst
         * laeuft der letzte Puffer aus, sonst fehlt das Ende des Titels. */
        for (i = 0; o->abort && i < 2; i++) {
            if (o->req[i] && !CheckIO((struct IORequest *)o->req[i])) {
                AbortIO((struct IORequest *)o->req[i]);
            }
        }
        /* Abgebrochen heisst: ab sofort still. Der Visualizer zeigte
         * sonst noch den abgebrochenen Puffer weiter, bis dessen Zeit
         * rechnerisch um war - 4 bis 5 Bilder Nachlauf nach STOP (vom
         * Anwender gesehen, 22.9.2026). */
        if (o->abort) {
            g_vis_seq++;
            g_vis_buf = NULL;
            g_vis_seq++;
        }
        for (i = 0; i < 2; i++) {
            if (o->req[i]) {
                WaitIO((struct IORequest *)o->req[i]);
            }
        }
        o->linked = FALSE;
        o->abort = FALSE;
    }
}

static void out_free(struct AudioOut *o)
{
    LONG i;

    out_drain(o);

    /* Der Visualizer liest aus diesen Puffern - vor dem Freigeben
     * abmelden. */
    g_vis_seq++;
    g_vis_buf = NULL;
    g_vis_seq++;

    if (o->devopen && o->req[0]) {
        CloseDevice((struct IORequest *)o->req[0]);
        o->devopen = FALSE;
    }
    if (o->req[1]) {
        FreeVec(o->req[1]);
        o->req[1] = NULL;
    }
    if (o->req[0]) {
        DeleteIORequest((struct IORequest *)o->req[0]);
        o->req[0] = NULL;
    }
    if (o->port) {
        DeleteMsgPort(o->port);
        o->port = NULL;
    }
    for (i = 0; i < 2; i++) {
        if (o->buf[i]) {
            FreeVec(o->buf[i]);
            o->buf[i] = NULL;
        }
    }
}

/* AllocVec statt malloc: diese Puffer gehoeren spaeter dem Audioprozess,
 * und der darf den Speicherverwalter von libnix nicht anfassen, solange
 * ein Netzauftrag laeuft (siehe netjob.h). Exec-Speicher ist dagegen
 * zwischen Prozessen sicher. */
static BOOL out_open(struct AudioOut *o, LONG unit)
{
    memset(o, 0, sizeof(*o));

    o->port = CreateMsgPort();
    if (!o->port) {
        strcpy(g_why, "no message port");
        return FALSE;
    }
    o->req[0] = (struct AHIRequest *)
                CreateIORequest(o->port, sizeof(struct AHIRequest));
    if (!o->req[0]) {
        strcpy(g_why, "no IO request");
        return FALSE;
    }
    o->req[0]->ahir_Version = 4;

    if (OpenDevice("ahi.device", (ULONG)unit,
                   (struct IORequest *)o->req[0], 0) != 0) {
        sprintf(g_why, "cannot open ahi.device unit %ld",
                (long)unit);
        return FALSE;
    }
    o->devopen = TRUE;

    /* Die zweite Anforderung ist eine KOPIE der ersten - so erbt sie das
     * geoeffnete Geraet. Geschlossen wird nur ueber die erste. */
    o->req[1] = AllocVec(sizeof(struct AHIRequest), MEMF_PUBLIC | MEMF_CLEAR);
    if (!o->req[1]) {
        strcpy(g_why, "out of memory");
        return FALSE;
    }
    CopyMem(o->req[0], o->req[1], sizeof(struct AHIRequest));

    o->buf[0] = AllocVec(AU_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    o->buf[1] = AllocVec(AU_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    if (!o->buf[0] || !o->buf[1]) {
        strcpy(g_why, "out of memory for audio buffers");
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Was gerade zu hoeren ist - fuer den Visualizer                      */
/* ------------------------------------------------------------------ */

/* Ein Puffer spielt knapp 0,19 s, der Visualizer will 25 Bilder je
 * Sekunde. Ein Schnappschuss je Puffer waere also viel zu grob. Statt
 * dessen steht hier, WELCHER Puffer gerade laeuft und SEIT WANN (E-Clock);
 * die Oberflaeche rechnet daraus die Stelle im Puffer und schneidet dort
 * ihr Fenster aus. So ist die Anzeige genau so weit wie der Ton.
 *
 * g_vis_seq ist ungerade, solange geschrieben wird. Der Leser nimmt die
 * Werte nur, wenn die Zahl vorher und nachher gleich und gerade ist -
 * so gibt es keine halb alten, halb neuen Angaben, ohne Semaphore. */
static void vis_publish(WORD *buf, LONG frames, LONG freq)
{
    struct EClockVal ev;

    if (!TimerBase) {
        return;
    }
    ReadEClock(&ev);
    g_vis_seq++;
    g_vis_buf = buf;
    g_vis_frames = frames;
    g_vis_freq = freq;
    g_vis_t0 = ev.ev_lo;
    g_vis_seq++;
}

BOOL audio_vis_window(WORD *mono, LONG n)
{
    struct EClockVal ev;
    ULONG efreq, s1, dt;
    WORD *buf;
    LONG frames, freq, pos, start, i;
    ULONG t0;

    if (!TimerBase || n <= 0) {
        return FALSE;
    }
    s1 = g_vis_seq;
    buf = g_vis_buf;
    frames = g_vis_frames;
    freq = g_vis_freq;
    t0 = g_vis_t0;
    if ((s1 & 1) || s1 != g_vis_seq || !buf || frames < n || freq <= 0) {
        return FALSE;                   /* gerade im Wechsel, oder nichts */
    }

    efreq = ReadEClock(&ev);
    dt = ev.ev_lo - t0;                 /* vorzeichenlos: auch ueber den Ueberlauf */
    if (efreq < 100 || dt > efreq) {
        return FALSE;                   /* laenger als 1 s her: Pause, Ende */
    }
    /* In Hundertsteln rechnen, sonst laeuft dt * freq ueber 32 Bit. */
    pos = (LONG)((dt * (ULONG)(freq / 100)) / (efreq / 100));
    if (pos > frames + frames / 2) {
        return FALSE;                   /* der Puffer ist laengst durch */
    }

    /* Das Fenster endet an der hoerbaren Stelle. Am Anfang eines Puffers
     * reicht er nicht weit genug zurueck - dann eben ab Pufferanfang,
     * das sind hoechstens n Abtastwerte Versatz (23 ms bei 1024). */
    start = pos - n;
    if (start < 0) {
        start = 0;
    }
    if (start + n > frames) {
        start = frames - n;
    }
    buf += start * 2;
    for (i = 0; i < n; i++) {
        mono[i] = (WORD)(((LONG)buf[0] + (LONG)buf[1]) >> 1);
        buf += 2;
    }
    return TRUE;
}

/* Einen gefuellten Puffer losschicken und auf den vorigen warten. */
static void out_send(struct AudioOut *o, LONG frames, LONG freq, LONG vol)
{
    struct AHIRequest *r = o->req[o->cur];
    struct AHIRequest *prev = o->req[o->cur ^ 1];

    r->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
    r->ahir_Std.io_Command = CMD_WRITE;
    r->ahir_Std.io_Data    = o->buf[o->cur];
    r->ahir_Std.io_Length  = (ULONG)(frames * 4);
    r->ahir_Std.io_Offset  = 0;
    r->ahir_Type      = AHIST_S16S;      /* Stereo, 16 Bit, verzahnt */
    r->ahir_Frequency = (ULONG)freq;
    r->ahir_Volume    = (Fixed)vol;
    r->ahir_Position  = 0x8000;          /* Mitte */
    /* Verkettung an den gerade laufenden Puffer - das ist die Stelle,
     * an der die Luecke entstuende, wenn man sie vergisst. */
    r->ahir_Link = o->linked ? prev : NULL;

    SendIO((struct IORequest *)r);

    if (o->linked) {
        WaitIO((struct IORequest *)prev);
    }
    /* Ab JETZT ist dieser Puffer zu hoeren: der vorige ist durch, und
     * AHI ist ueber ahir_Link nahtlos in diesen gewechselt. Er bleibt
     * unangetastet, bis er ausgespielt ist - gefuellt wird inzwischen
     * der andere. Der Visualizer darf ihn also lesen. */
    vis_publish(o->buf[o->cur], frames, freq);
    o->linked = TRUE;
    o->cur ^= 1;
}

BOOL audio_play_file(const char *path, LONG unit, struct AudioProbe *out)
{
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *mps = NULL;
    struct AudioOut o;
    WORD *pcm[MPEGA_MAX_CHANNELS];
    WORD chbuf[MPEGA_MAX_CHANNELS][MPEGA_PCM_SIZE];
    BOOL ok = FALSE;
    ULONG t0;
    LONG carry_n = 0, carry_pos = 0;
    LONG skips = 0, bad = 0;
    BOOL ended = FALSE;    /* Rest des letzten Blocks */

    memset(out, 0, sizeof(*out));
    g_why[0] = '\0';
    memset(&o, 0, sizeof(o));

    MPEGABase = OpenLibrary("mpega.library", 0);
    if (!MPEGABase) {
        strcpy(g_why, "mpega.library missing");
        return FALSE;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.check_mpeg = 1;
    ctrl.layer_1_2.mono.freq_div = 1;
    ctrl.layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_1_2.stereo.freq_div = 1;
    ctrl.layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_3 = ctrl.layer_1_2;

    mps = MPEGA_open((char *)path, &ctrl);
    if (!mps) {
        strcpy(g_why, "MPEGA_open failed - not an MPEG audio file?");
        goto out;
    }

    out->freq     = mps->dec_frequency;
    out->channels = mps->dec_channels;
    out->bitrate  = mps->bitrate;
    out->layer    = mps->layer;
    out->ms_total = mps->ms_duration;

    if (!out_open(&o, unit)) {
        goto out;
    }

    pcm[0] = chbuf[0];
    pcm[1] = chbuf[1];

    t0 = now_cs();
    for (;;) {
        WORD *dst = o.buf[o.cur];
        LONG have = 0;

        /* Puffer fuellen. mpega liefert die Kanaele GETRENNT, AHI will
         * sie verzahnt - deshalb wird hier umgeschichtet. Bei Mono
         * kommt derselbe Wert auf beide Seiten.
         *
         * Ein dekodierter Block hat 1152 Abtastwerte, der Puffer fasst
         * 8192 - der letzte Block passt also nie glatt hinein. Sein Rest
         * wird AUFGEHOBEN und eroeffnet den naechsten Puffer. Beim ersten
         * Anlauf fiel er weg: aus 60,05 s Musik wurden 53,36 s, also
         * 12 Prozent zu schnell und mit einem Knacken je Puffer. */
        while (have < AU_FRAMES) {
            LONG n, i;

            if (carry_n <= 0) {
                n = MPEGA_decode_frame(mps, pcm);

                /* DREI Faelle, und sie zu verwechseln kostet einen
                 * ganzen Titel (gemessen 20.9.2026):
                 *   n > 0  Abtastwerte
                 *   n == 0 der Dekoder ist NOCH NICHT eingerastet -
                 *          laut mpega.doc ausdruecklich KEIN Fehler.
                 *          Beim Sprung mitten in die Datei ist das der
                 *          Normalfall, denn dort faengt kein Rahmen an.
                 *   n < 0  Dateiende oder ein kaputter Rahmen.
                 *
                 * Vorher stand hier "n <= 0 heisst Ende". Ein Sprung
                 * meldete damit sofort "Titel zu Ende", und die
                 * Warteschlange schaltete weiter, statt zu springen. */
                if (n == 0) {
                    if (++skips > 2000) {
                        ended = TRUE;       /* findet keinen Rahmen */
                        break;
                    }
                    continue;
                }
                if (n < 0) {
                    if (n == MPEGA_ERR_BADFRAME && ++bad < 64) {
                        continue;           /* einen kaputten ueberspringen */
                    }
                    ended = TRUE;
                    break;
                }
                skips = 0;
                bad = 0;
                carry_n = n;
                carry_pos = 0;
                out->frames++;
                out->samples += n;
            }
            n = carry_n - carry_pos;
            if (n > AU_FRAMES - have) {
                n = AU_FRAMES - have;
            }
            if (mps->dec_channels > 1) {
                for (i = 0; i < n; i++) {
                    *dst++ = chbuf[0][carry_pos + i];
                    *dst++ = chbuf[1][carry_pos + i];
                }
            } else {
                for (i = 0; i < n; i++) {
                    *dst++ = chbuf[0][carry_pos + i];
                    *dst++ = chbuf[0][carry_pos + i];
                }
            }
            have += n;
            carry_pos += n;
            if (carry_pos >= carry_n) {
                carry_n = 0;
                carry_pos = 0;
            }
        }

        if (have <= 0) {
            if (ended) {
                break;                      /* Ende */
            }
            continue;                       /* nur noch nicht synchron */
        }
        out_send(&o, have, mps->dec_frequency, 0x10000);

        /* Strg-C bricht ab - ohne das laesst sich ein laufender Titel
         * aus der Shell nicht beenden. */
        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
            strcpy(g_why, "aborted");
            o.abort = TRUE;
            break;
        }
    }
    out->cs_decode = now_cs() - t0;
    ok = out->frames > 0;

out:
    out_free(&o);
    if (mps) {
        MPEGA_close(mps);
    }
    if (MPEGABase) {
        CloseLibrary(MPEGABase);
        MPEGABase = NULL;
    }
    return ok;
}


/* ------------------------------------------------------------------ */
/* Stufe 2: aus dem Ringpuffer statt aus einer Datei                   */
/* ------------------------------------------------------------------ */

/* mpega.library ruft den Hook mit den Argumenten in Registern auf, in
 * der SAS/C-Schreibweise a0 = Hook, a2 = Handle, a1 = Zugriff. gcc kann
 * das nicht unmittelbar, deshalb dieser kleine Zwischenschritt: er legt
 * die drei Register in der Reihenfolge der C-Aufrufkonvention auf den
 * Stapel und springt weiter.
 *
 * Dass Symbole hier einen Unterstrich tragen, ist nachgesehen und nicht
 * geraten (nm auf einer Uebersetzungseinheit). */
asm("    .text\n"
"    .globl _mpega_stub\n"
"_mpega_stub:\n"
"    move.l  a1,-(sp)\n"          /* MPEGA_ACCESS *access */
"    move.l  a2,-(sp)\n"          /* APTR handle          */
"    move.l  a0,-(sp)\n"          /* struct Hook *hook    */
"    jsr     _mpega_access\n"
"    lea     12(sp),sp\n"
"    rts\n");

extern void mpega_stub(void);

/* Der Ring, aus dem gerade dekodiert wird. Ein Zeiger im Hook selbst
 * waere sauberer, aber mpega gibt den Hook als a0 mit - und genau den
 * benutzen wir dafuer (h_Data). */
/* MESSUNG: wer ruft was im Hook? */
LONG g_hk_open = 0, g_hk_read = 0, g_hk_seek = 0, g_hk_close = 0;
LONG g_hk_bytes = 0, g_hk_other = 0, g_hk_seekpos = -1;

/* Wie oft musste der Dekoder auf Nachschub warten, und wie lange? Jede
 * Runde ist eine Fuenfzigstelsekunde. Das ist das Mass dafuer, ob der
 * Ring gross genug und das Netz schnell genug ist: > 0 heisst, es haette
 * stocken koennen. */
LONG g_hk_waits = 0, g_hk_wait_ticks = 0;

/* MESSUNG des Vorlaufs im Dekoder: wie lange brauchen MPEGA_open (das
 * liest schon Daten und sucht den ersten Rahmen) und das Oeffnen von
 * ahi.device, bis der erste Puffer unterwegs ist? */
LONG g_t_mpega = 0, g_t_ahi = 0;

ULONG mpega_access(struct Hook *hook, APTR handle, MPEGA_ACCESS *acc)
{
    struct Ring *r = (struct Ring *)hook->h_Data;

    (void)handle;

    switch (acc->func) {
    case MPEGA_BSFUNC_OPEN:
        g_hk_open++;
        /* stream_size ist AUSGABE: die Gesamtlaenge des Stroms. 0 heisst
         * "unbekannt" - mpega rechnet dann keine Spieldauer aus, was bei
         * einem laufenden Strom auch richtig ist. */
        acc->data.open.stream_size = 0;
        return 1;               /* irgendein Handle ungleich NULL */

    case MPEGA_BSFUNC_CLOSE:
        g_hk_close++;
        return 0;

    case MPEGA_BSFUNC_READ: {
        g_hk_read++;
        UBYTE *dst = (UBYTE *)acc->data.read.buffer;
        LONG want = acc->data.read.num_bytes;
        LONG got = 0;
        BOOL waited = FALSE;

        while (got < want) {
            ULONG n = ring_read(r, dst + got, (ULONG)(want - got));

            if (n > 0) {
                got += (LONG)n;
                continue;
            }
            if (r->eof || r->stop) {
                break;          /* nichts mehr zu erwarten */
            }
            /* Der Ring ist leer, der Fueller haengt noch am Netz. Kurz
             * warten statt drehen - eine Fuenfzigstelsekunde. */
            if (!waited) {
                g_hk_waits++;       /* einmal je Leseaufruf zaehlen */
                waited = TRUE;
            }
            g_hk_wait_ticks++;
            Delay(1);
        }
        g_hk_bytes += got;
        return (ULONG)got;
    }

    case MPEGA_BSFUNC_SEEK:
        g_hk_seek++;
        g_hk_seekpos = acc->data.seek.abs_byte_seek_pos;
        /* In einem laufenden Strom kann man nicht springen: was durch
         * ist, ist weg. Gesprungen wird stattdessen mit einer neuen
         * Anfrage an den Server (Range), siehe sub_stream_open(). */
        return (ULONG)-1;
    }
    g_hk_other++;
    return (ULONG)-1;
}

BOOL audio_play_ring(struct Ring *r, LONG unit, struct AudioProbe *out)
{
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *mps = NULL;
    struct Hook hook;
    struct AudioOut o;
    WORD *pcm[MPEGA_MAX_CHANNELS];
    WORD chbuf[MPEGA_MAX_CHANNELS][MPEGA_PCM_SIZE];
    BOOL ok = FALSE;
    ULONG t0;
    LONG carry_n = 0, carry_pos = 0;
    LONG skips = 0, bad = 0;
    BOOL ended = FALSE;

    memset(out, 0, sizeof(*out));
    g_why[0] = '\0';
    memset(&o, 0, sizeof(o));
    g_hk_waits = 0;
    g_hk_wait_ticks = 0;

    MPEGABase = OpenLibrary("mpega.library", 0);
    if (!MPEGABase) {
        strcpy(g_why, "mpega.library missing");
        return FALSE;
    }

    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (ULONG (*)())mpega_stub;
    hook.h_Data  = r;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access = &hook;             /* HIER kommt der Ring ins Spiel */
    ctrl.check_mpeg = 1;
    ctrl.layer_1_2.mono.freq_div = 1;
    ctrl.layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_1_2.stereo.freq_div = 1;
    ctrl.layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_3 = ctrl.layer_1_2;

    /* Der Dateiname wird mit dem eigenen Hook nicht benutzt, mpega
     * reicht ihn nur an MPEGA_BSFUNC_OPEN durch. */
    {
        ULONG t = now_cs();

        mps = MPEGA_open("<strom>", &ctrl);
        g_t_mpega = (LONG)(now_cs() - t);
    }
    if (!mps) {
        strcpy(g_why, "MPEGA_open failed - no MPEG in stream?");
        goto out;
    }

    out->freq     = mps->dec_frequency;
    out->channels = mps->dec_channels;
    out->bitrate  = mps->bitrate;
    out->layer    = mps->layer;

    {
        ULONG t = now_cs();

        if (!out_open(&o, unit)) {
            goto out;
        }
        g_t_ahi = (LONG)(now_cs() - t);
    }

    pcm[0] = chbuf[0];
    pcm[1] = chbuf[1];

    t0 = now_cs();
    for (;;) {
        WORD *dst = o.buf[o.cur];
        LONG have = 0;

        while (have < AU_FRAMES) {
            LONG n, i;

            if (carry_n <= 0) {
                n = MPEGA_decode_frame(mps, pcm);

                /* DREI Faelle, und sie zu verwechseln kostet einen
                 * ganzen Titel (gemessen 20.9.2026):
                 *   n > 0  Abtastwerte
                 *   n == 0 der Dekoder ist NOCH NICHT eingerastet -
                 *          laut mpega.doc ausdruecklich KEIN Fehler.
                 *          Beim Sprung mitten in die Datei ist das der
                 *          Normalfall, denn dort faengt kein Rahmen an.
                 *   n < 0  Dateiende oder ein kaputter Rahmen.
                 *
                 * Vorher stand hier "n <= 0 heisst Ende". Ein Sprung
                 * meldete damit sofort "Titel zu Ende", und die
                 * Warteschlange schaltete weiter, statt zu springen. */
                if (n == 0) {
                    if (++skips > 2000) {
                        ended = TRUE;       /* findet keinen Rahmen */
                        break;
                    }
                    continue;
                }
                if (n < 0) {
                    if (n == MPEGA_ERR_BADFRAME && ++bad < 64) {
                        continue;           /* einen kaputten ueberspringen */
                    }
                    ended = TRUE;
                    break;
                }
                skips = 0;
                bad = 0;
                carry_n = n;
                carry_pos = 0;
                out->frames++;
                out->samples += n;
            }
            n = carry_n - carry_pos;
            if (n > AU_FRAMES - have) {
                n = AU_FRAMES - have;
            }
            if (mps->dec_channels > 1) {
                for (i = 0; i < n; i++) {
                    *dst++ = chbuf[0][carry_pos + i];
                    *dst++ = chbuf[1][carry_pos + i];
                }
            } else {
                for (i = 0; i < n; i++) {
                    *dst++ = chbuf[0][carry_pos + i];
                    *dst++ = chbuf[0][carry_pos + i];
                }
            }
            have += n;
            carry_pos += n;
            if (carry_pos >= carry_n) {
                carry_n = 0;
                carry_pos = 0;
            }
        }

        if (have <= 0) {
            if (ended) {
                break;
            }
            continue;
        }
        out_send(&o, have, mps->dec_frequency, 0x10000);

        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
            strcpy(g_why, "aborted");
            o.abort = TRUE;
            r->stop = TRUE;
            break;
        }
    }
    out->cs_decode = now_cs() - t0;
    ok = out->frames > 0;

out:
    out_free(&o);
    if (mps) {
        MPEGA_close(mps);
    }
    if (MPEGABase) {
        CloseLibrary(MPEGABase);
        MPEGABase = NULL;
    }
    r->stop = TRUE;
    return ok;
}


/* ------------------------------------------------------------------ */
/* Stufe 3: der Audioprozess                                           */
/* ------------------------------------------------------------------ */

/* Warum ein eigener Prozess: Dekodieren und Ausgeben duerfen nicht davon
 * abhaengen, was die Oberflaeche gerade tut. MUI kann beim Rollen der
 * Bildwand eine Sekunde am Stueck zeichnen - der Ton darf davon nichts
 * merken. Umgekehrt darf die Oberflaeche nicht blockieren, waehrend ein
 * Titel laeuft.
 *
 * Die beiden reden ueber diese Felder und zwei Signale. Jedes Feld hat
 * genau einen Schreiber: was die Oberflaeche setzt (cmd, vol), liest der
 * Audioprozess nur, und umgekehrt (state, samples, done). Auf dem 68k
 * ist ein LONG unteilbar, damit braucht es keine Semaphore. */
#define AC_NONE    0
#define AC_PLAY    1
#define AC_PAUSE   2
#define AC_RESUME  3
#define AC_HALT    4
#define AC_QUIT    5

static struct Process *g_ap = NULL;
static struct Task    *g_ap_task = NULL;
static volatile LONG   g_ap_cmd = AC_NONE;
static volatile LONG   g_ap_state = AU_STOPPED;
static volatile LONG   g_ap_vol = 64;
static volatile LONG   g_ap_base_ms = 0;
static volatile ULONG  g_ap_samples = 0;
static volatile LONG   g_ap_freq = 44100;
static volatile LONG   g_ap_done = 0;
static volatile LONG   g_ap_error = 0;
static volatile LONG   g_ap_ready = 0;
static volatile LONG   g_ap_unit = 0;
static struct Ring    *volatile g_ap_ring = NULL;
static struct AudioOut g_ap_out;

/* Lautstaerke 0..64 in AHIs Festkommaform 0..0x10000. */
static LONG vol_fixed(LONG v)
{
    if (v < 0)  { v = 0; }
    if (v > 64) { v = 64; }
    return (v * 0x10000L) / 64L;
}

/* Pause: keine neuen Puffer mehr losschicken. Der Ton endet damit,
 * sobald die schon uebergebenen durch sind - das ist hoechstens eine
 * knappe halbe Sekunde. Anhalten mitten im Puffer waere nur mit AbortIO
 * zu haben, und das kostet den sauberen Uebergang.
 *
 * Rueckgabe TRUE: der Titel soll abbrechen (HALT, QUIT, neuer Titel).
 * Fuer beide Dekoder gleich, deshalb hier und nicht zweimal. */
static BOOL ap_hold(void)
{
    while (g_ap_cmd == AC_PAUSE || g_ap_state == AU_PAUSED) {
        g_ap_state = AU_PAUSED;
        if (g_ap_cmd == AC_RESUME) {
            g_ap_cmd = AC_NONE;
            g_ap_state = AU_PLAYING;
            break;
        }
        if (g_ap_cmd == AC_HALT || g_ap_cmd == AC_QUIT ||
            g_ap_cmd == AC_PLAY) {
            break;
        }
        g_ap_cmd = AC_NONE;
        Delay(2);
    }
    return g_ap_cmd == AC_HALT || g_ap_cmd == AC_QUIT ||
           g_ap_cmd == AC_PLAY;
}

/* Titelende, fuer beide Dekoder gleich.
 *
 * "Zu Ende" heisst NUR: der Dekoder hatte nichts mehr. Wurde der Titel
 * abgebrochen - fuer einen Sprung, einen Wechsel oder STOP - darf das
 * NICHT als Titelende gelten, sonst schaltet die Warteschlange weiter.
 *
 * Genau das ist am 20.9.2026 passiert: ein Klick in den
 * Fortschrittsbalken sprang nicht, sondern spielte den naechsten Titel -
 * der Sprung bricht den laufenden ab, und der meldete sich als "durch". */
static void ap_end(struct Ring *r)
{
    BOOL natural = (g_ap_cmd != AC_HALT && g_ap_cmd != AC_QUIT
                    && g_ap_cmd != AC_PLAY);

    r->stop = TRUE;                     /* der Fueller darf aufhoeren */

    /* Die letzten Puffer auslaufen lassen, sonst fehlt das Ende. Nur bei
     * HALT wird abgebrochen - dort soll es ja sofort still sein. */
    g_ap_out.abort = (g_ap_cmd == AC_HALT || g_ap_cmd == AC_QUIT);
    out_drain(&g_ap_out);

    g_ap_state = AU_STOPPED;
    if (natural) {
        g_ap_done = 1;
    }
}

/* ------------------------------------------------------------------ */
/* AAC (Helix) - bisher nur fuer Radiosender                           */
/* ------------------------------------------------------------------ */

/* Anders als mpega.library liest Helix nicht selbst, sondern bekommt
 * einen Puffer mit mindestens einem GANZEN ADTS-Block. Das ist Pflicht:
 * einen angeschnittenen Block liest Helix ueber das Pufferende hinaus
 * (AddressSanitizer auf dem Mac, 22.9.2026), und auf dem Amiga hing
 * das CLI danach fest, bis zum Neustart.
 *
 * Der Eingang fasst zwei der groessten Bloecke - die Laenge im ADTS-Kopf
 * hat 13 Bit, mehr als 8191 Bytes geht nicht.
 *
 * Alles hier ist statisch oder AllocVec: der Audioprozess darf malloc
 * nicht anfassen (siehe netjob.h). Deshalb auch AACInitDecoderPre mit
 * eigenem Speicher statt AACInitDecoder. */
#define AAC_IN  16384

static UBYTE g_aac_in[AAC_IN];
static LONG  g_aac_len = 0;             /* Bytes im Eingang */
static LONG  g_aac_off = 0;             /* davon schon verbraucht */
static WORD  g_aac_pcm[AAC_MAX_NCHANS * AAC_MAX_NSAMPS * 2];   /* *2: SBR */
static APTR  g_aac_mem = NULL;
static LONG  g_aac_memsize = 0;

/* Holt aus dem Ring nach, bis mindestens need Bytes im Eingang liegen.
 * FALSE: es kommt nichts mehr (Ende, Abbruch). Wartet wie der Hook von
 * mpega - schlafend, nie drehend. */
static BOOL aac_fill(struct Ring *r, LONG need)
{
    BOOL waited = FALSE;

    while (g_aac_len < need) {
        ULONG n = ring_read(r, g_aac_in + g_aac_len,
                            (ULONG)(AAC_IN - g_aac_len));

        if (n > 0) {
            g_aac_len += (LONG)n;
            continue;
        }
        if (r->eof || r->stop || g_ap_cmd == AC_HALT ||
            g_ap_cmd == AC_QUIT || g_ap_cmd == AC_PLAY) {
            return FALSE;
        }
        if (!waited) {
            g_hk_waits++;
            waited = TRUE;
        }
        g_hk_wait_ticks++;
        Delay(1);
    }
    return TRUE;
}

/* Den naechsten Block dekodieren. Rueckgabe: Abtastwerte je Kanal
 * (> 0), 0 = der Strom ist zu Ende oder abgebrochen, -1 = beim besten
 * Willen kein AAC darin. */
static LONG aac_next(struct Ring *r, HAACDecoder h, AACFrameInfo *fi)
{
    LONG junk = 0, bad = 0;

    for (;;) {
        UBYTE *p;
        int left, sync, err;
        LONG flen;

        /* Verbrauchtes wegschieben - der Block soll am Anfang liegen,
         * damit dahinter Platz fuer den groessten moeglichen ist. */
        if (g_aac_off > 0) {
            g_aac_len -= g_aac_off;
            memmove(g_aac_in, g_aac_in + g_aac_off, (size_t)g_aac_len);
            g_aac_off = 0;
        }
        if (g_aac_len < 7 && !aac_fill(r, 7)) {
            return 0;
        }

        sync = AACFindSyncWord(g_aac_in, (int)g_aac_len);
        if (sync < 0) {
            /* Kein Sync im ganzen Eingang. Das letzte Byte bleibt, es
             * koennte die erste Haelfte eines Syncworts sein. */
            g_aac_off = g_aac_len - 1;
            if (++junk > 64) {
                return -1;              /* 1 MB ohne einen Block */
            }
            continue;
        }
        if (sync > 0) {
            g_aac_off = sync;
            continue;                   /* erst nach vorn schieben */
        }

        /* Laenge aus dem ADTS-Kopf, 13 Bit ab Bit 30. Unter 7 ist es
         * kein Kopf, sondern zufaellig 0xFFF in den Daten. */
        flen = ((LONG)(g_aac_in[3] & 3) << 11) | ((LONG)g_aac_in[4] << 3)
             | ((LONG)g_aac_in[5] >> 5);
        if (flen < 7) {
            g_aac_off = 1;
            continue;
        }
        if (g_aac_len < flen && !aac_fill(r, flen)) {
            return 0;
        }

        p = g_aac_in;
        left = (int)g_aac_len;
        err = AACDecode(h, &p, &left, g_aac_pcm);
        g_aac_off = g_aac_len - left;
        if (err == ERR_AAC_NONE) {
            AACGetLastFrameInfo(h, fi);
            if (fi->nChans > 0 && fi->outputSamps > 0) {
                return fi->outputSamps / fi->nChans;
            }
            continue;
        }
        /* Kaputter Block (z.B. nach einem Abriss mitten hinein): ein
         * Byte weiter und neu suchen. */
        if (++bad > 64) {
            return -1;
        }
        g_aac_off = 1;
    }
}

static void ap_play_aac(struct Ring *r)
{
    HAACDecoder h;
    AACFrameInfo fi;
    LONG carry_n = 0, carry_pos = 0, chans = 2;
    ULONG frames = 0;
    BOOL ended = FALSE;

    /* Speicher fuer den Dekoder einmal holen und behalten, wie AHI. */
    if (!g_aac_mem) {
        g_aac_memsize = aac_state_size();
        g_aac_mem = AllocVec((ULONG)g_aac_memsize, MEMF_PUBLIC | MEMF_CLEAR);
    }
    /* AACInitDecoderPre loescht den Zustand selbst - jeder Sender
     * faengt also frisch an. */
    h = g_aac_mem ? AACInitDecoderPre(g_aac_mem, (int)g_aac_memsize) : NULL;
    if (!h) {
        strcpy(g_why, "out of memory for AAC decoder");
        r->stop = TRUE;
        g_ap_state = AU_STOPPED;
        g_ap_error = 1;
        return;
    }
    g_aac_len = 0;
    g_aac_off = 0;
    g_ap_samples = 0;
    g_ap_state = AU_PLAYING;

    for (;;) {
        WORD *dst;
        LONG have = 0;

        if (ap_hold()) {
            break;
        }

        dst = g_ap_out.buf[g_ap_out.cur];
        while (have < AU_FRAMES) {
            LONG n, i;
            WORD *src;

            if (carry_n <= 0) {
                n = aac_next(r, h, &fi);
                if (n <= 0) {
                    if (n < 0 && frames == 0) {
                        /* Wie "no MPEG in stream": ein Fehler, KEIN
                         * Titelende. */
                        strcpy(g_why, "no AAC in stream");
                        g_ap_error = 1;
                    }
                    ended = TRUE;
                    break;
                }
                if (frames == 0) {
                    g_ap_freq = fi.sampRateOut > 0 ? fi.sampRateOut
                                                   : 44100;
                }
                frames++;
                chans = fi.nChans;
                carry_n = n;
                carry_pos = 0;
            }
            n = carry_n - carry_pos;
            if (n > AU_FRAMES - have) {
                n = AU_FRAMES - have;
            }
            /* Helix liefert schon verzahnt (L R L R), anders als mpega.
             * Mono - auch HE-AAC v2, Parametric Stereo kann Helix nicht -
             * kommt auf beide Seiten. */
            if (chans > 1) {
                src = g_aac_pcm + carry_pos * chans;
                for (i = 0; i < n; i++) {
                    *dst++ = src[0];
                    *dst++ = src[1];
                    src += chans;
                }
            } else {
                src = g_aac_pcm + carry_pos;
                for (i = 0; i < n; i++) {
                    *dst++ = *src;
                    *dst++ = *src++;
                }
            }
            have += n;
            carry_pos += n;
            if (carry_pos >= carry_n) {
                carry_n = 0;
                carry_pos = 0;
            }
        }

        if (have > 0) {
            out_send(&g_ap_out, have, g_ap_freq, vol_fixed(g_ap_vol));
            g_ap_samples += (ULONG)have;
        }
        if (ended) {
            break;
        }
    }

    /* Kein AACFreeDecoder: der Speicher ist unserer (Pre), und das
     * Freigeben dort ginge an free() - also an libnix. */
    if (g_ap_error) {
        r->stop = TRUE;
        out_drain(&g_ap_out);
        g_ap_state = AU_STOPPED;
        return;
    }
    ap_end(r);
}

/* Einen Titel dekodieren und ausgeben. Laeuft IM Audioprozess. */
static void ap_play_track(void)
{
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *mps;
    struct Hook hook;
    struct Ring *r = g_ap_ring;
    WORD *pcm[MPEGA_MAX_CHANNELS];
    WORD chbuf[MPEGA_MAX_CHANNELS][MPEGA_PCM_SIZE];
    LONG carry_n = 0, carry_pos = 0;
    LONG skips = 0, bad = 0;
    BOOL ended = FALSE;

    if (!r) {
        return;
    }
    g_hk_waits = 0;
    g_hk_wait_ticks = 0;

    /* Erst warten, bis Daten da sind: das Format steht im Ring, und der
     * Netzprozess setzt es VOR dem ersten Byte (siehe ring.h). Vorher
     * hiesse RING_MPEG nur "noch nichts bekannt". Bricht jemand ab oder
     * kommt nie etwas, geht es wie bisher weiter - MPEGA_open scheitert
     * dann und meldet es. */
    while (ring_used(r) == 0 && !r->eof && !r->stop &&
           g_ap_cmd != AC_HALT && g_ap_cmd != AC_QUIT &&
           g_ap_cmd != AC_PLAY) {
        Delay(1);
    }
    if (r->fmt == RING_AAC && ring_used(r) > 0) {
        ap_play_aac(r);
        return;
    }

    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (ULONG (*)())mpega_stub;
    hook.h_Data  = r;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access = &hook;
    ctrl.check_mpeg = 1;
    ctrl.layer_1_2.mono.freq_div = 1;
    ctrl.layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_1_2.stereo.freq_div = 1;
    ctrl.layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
    ctrl.layer_3 = ctrl.layer_1_2;

    mps = MPEGA_open("<strom>", &ctrl);
    if (!mps) {
        /* KEIN g_ap_done: das ist ein Fehler, kein Titelende. Sonst
         * schaltet die Warteschlange weiter und verdeckt die Ursache -
         * genau so sah der missglueckte Sprung am 20.9.2026 aus. */
        strcpy(g_why, "no MPEG in stream");
        g_ap_state = AU_STOPPED;
        g_ap_error = 1;
        return;
    }

    g_ap_freq = mps->dec_frequency > 0 ? mps->dec_frequency : 44100;
    g_ap_samples = 0;
    g_ap_state = AU_PLAYING;

    pcm[0] = chbuf[0];
    pcm[1] = chbuf[1];

    for (;;) {
        WORD *dst;
        LONG have = 0;

        if (ap_hold()) {
            break;
        }

        dst = g_ap_out.buf[g_ap_out.cur];
        while (have < AU_FRAMES) {
            LONG n, i;

            if (carry_n <= 0) {
                n = MPEGA_decode_frame(mps, pcm);

                /* DREI Faelle, und sie zu verwechseln kostet einen
                 * ganzen Titel (gemessen 20.9.2026):
                 *   n > 0  Abtastwerte
                 *   n == 0 der Dekoder ist NOCH NICHT eingerastet -
                 *          laut mpega.doc ausdruecklich KEIN Fehler.
                 *          Beim Sprung mitten in die Datei ist das der
                 *          Normalfall, denn dort faengt kein Rahmen an.
                 *   n < 0  Dateiende oder ein kaputter Rahmen.
                 *
                 * Vorher stand hier "n <= 0 heisst Ende". Ein Sprung
                 * meldete damit sofort "Titel zu Ende", und die
                 * Warteschlange schaltete weiter, statt zu springen. */
                if (n == 0) {
                    if (++skips > 2000) {
                        ended = TRUE;       /* findet keinen Rahmen */
                        break;
                    }
                    continue;
                }
                if (n < 0) {
                    if (n == MPEGA_ERR_BADFRAME && ++bad < 64) {
                        continue;           /* einen kaputten ueberspringen */
                    }
                    ended = TRUE;
                    break;
                }
                skips = 0;
                bad = 0;
                carry_n = n;
                carry_pos = 0;
            }
            n = carry_n - carry_pos;
            if (n > AU_FRAMES - have) {
                n = AU_FRAMES - have;
            }
            if (mps->dec_channels > 1) {
                for (i = 0; i < n; i++) {
                    *dst++ = chbuf[0][carry_pos + i];
                    *dst++ = chbuf[1][carry_pos + i];
                }
            } else {
                for (i = 0; i < n; i++) {
                    *dst++ = chbuf[0][carry_pos + i];
                    *dst++ = chbuf[0][carry_pos + i];
                }
            }
            have += n;
            carry_pos += n;
            if (carry_pos >= carry_n) {
                carry_n = 0;
                carry_pos = 0;
            }
        }

        if (have <= 0) {
            if (ended) {
                break;                  /* Titel zu Ende */
            }
            continue;                   /* nur noch nicht synchron */
        }
        out_send(&g_ap_out, have, g_ap_freq, vol_fixed(g_ap_vol));
        g_ap_samples += (ULONG)have;
    }

    MPEGA_close(mps);
    ap_end(r);
}

static void audio_proc(void)
{
    if (!out_open(&g_ap_out, g_ap_unit)) {
        g_ap_ready = -1;
        Signal(g_ap_task ? g_ap_task : FindTask(NULL), SIGF_SINGLE);
        return;
    }
    g_ap_ready = 1;
    Signal(g_ap_task, SIGF_SINGLE);

    for (;;) {
        LONG cmd = g_ap_cmd;

        if (cmd == AC_QUIT) {
            break;
        }
        if (cmd == AC_PLAY) {
            g_ap_cmd = AC_NONE;
            g_ap_done = 0;
            ap_play_track();
            continue;
        }
        if (cmd != AC_NONE) {
            g_ap_cmd = AC_NONE;
        }
        Delay(2);                       /* nichts zu tun */
    }

    out_free(&g_ap_out);
    if (g_aac_mem) {
        FreeVec(g_aac_mem);
        g_aac_mem = NULL;
    }
    if (MPEGABase) {
        CloseLibrary(MPEGABase);
        MPEGABase = NULL;
    }
    g_ap_ready = 0;
    Signal(g_ap_task, SIGF_SINGLE);
}

BOOL audio_start(LONG unit)
{
    if (g_ap) {
        return TRUE;
    }
    g_why[0] = '\0';

    MPEGABase = OpenLibrary("mpega.library", 0);
    if (!MPEGABase) {
        strcpy(g_why, "mpega.library missing - see Readme");
        return FALSE;
    }

    /* timer.device nur fuer ReadEClock, eine Uhr, die Audioprozess und
     * Oberflaeche gemeinsam haben. Fehlt sie, gibt es eben keinen
     * Visualizer - abspielen geht trotzdem. */
    if (!TimerBase) {
        memset(&g_eclock_req, 0, sizeof(g_eclock_req));
        if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK,
                       (struct IORequest *)&g_eclock_req, 0) == 0) {
            TimerBase = g_eclock_req.tr_node.io_Device;
        }
    }

    g_ap_unit = unit;
    g_ap_cmd = AC_NONE;
    g_ap_ready = 0;
    g_ap_task = FindTask(NULL);

    /* 64 KB Stapel wie beim Netzprozess: mpega rechnet tief, und ein zu
     * kleiner Stapel faellt erst auf einer anderen Maschine auf (siehe
     * netjob.c). */
    g_ap = CreateNewProcTags(NP_Entry,     (ULONG)audio_proc,
                             NP_Name,      (ULONG)"AmiSubsonic.audio",
                             NP_StackSize, 65536,
                             NP_Priority,  5,   /* vor der Oberflaeche */
                             TAG_DONE);
    if (!g_ap) {
        strcpy(g_why, "cannot start audio process");
        CloseLibrary(MPEGABase);
        MPEGABase = NULL;
        return FALSE;
    }

    Wait(SIGF_SINGLE);                  /* er meldet sich, wenn AHI steht */
    if (g_ap_ready != 1) {
        g_ap = NULL;
        if (!g_why[0]) {
            strcpy(g_why, "cannot open ahi.device");
        }
        return FALSE;
    }
    return TRUE;
}

void audio_shutdown(void)
{
    if (!g_ap) {
        return;
    }
    g_ap_cmd = AC_QUIT;
    Wait(SIGF_SINGLE);
    g_ap = NULL;
    if (TimerBase) {
        CloseDevice((struct IORequest *)&g_eclock_req);
        TimerBase = NULL;
    }
}

void audio_play(struct Ring *r, LONG base_ms)
{
    g_ap_ring = r;
    g_ap_base_ms = base_ms;
    g_ap_samples = 0;
    g_ap_done = 0;
    g_ap_cmd = AC_PLAY;
}

void audio_pause(BOOL on)
{
    g_ap_cmd = on ? AC_PAUSE : AC_RESUME;
}

void audio_halt(void)
{
    if (g_ap_ring) {
        g_ap_ring->stop = TRUE;
    }
    g_ap_cmd = AC_HALT;
}

void audio_set_volume(LONG vol)
{
    g_ap_vol = vol;
}

LONG audio_state(void)
{
    return g_ap_state;
}

LONG audio_pos_ms(void)
{
    LONG freq = g_ap_freq > 0 ? g_ap_freq : 44100;

    /* Erst durch die Frequenz, dann mal 1000 - andersherum liefe ein
     * LONG bei gut 48 Sekunden ueber. */
    return g_ap_base_ms + (LONG)(g_ap_samples / (ULONG)freq) * 1000L
         + (LONG)((g_ap_samples % (ULONG)freq) * 1000UL / (ULONG)freq);
}

BOOL audio_track_done(void)
{
    return g_ap_done != 0;
}

BOOL audio_error(void)
{
    if (g_ap_error) {
        g_ap_error = 0;
        return TRUE;
    }
    return FALSE;
}

void audio_clear_done(void)
{
    g_ap_done = 0;
}
