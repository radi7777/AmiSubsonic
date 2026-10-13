/* AmiSubsonic - Shell-Werkzeug ueber demselben Kern wie die Oberflaeche.
 *
 * Zweck ist nicht nur Bequemlichkeit: Version 0.1 von AmiHomeassist war
 * reine Konsole und gegen die echte Gegenstelle verifiziert, bevor das
 * erste MUI-Objekt existierte. Genauso hier - was dieses Programm nicht
 * kann, braucht in der Oberflaeche gar nicht erst gesucht zu werden.
 */

#include <exec/types.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amisub.h"
#include "cover.h"
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dostags.h>

#include "audio.h"
#include "ring.h"
#include "abs.h"
#include "pod.h"

/* aacdec.h kennt keinen 68k und bricht sonst mit #error ab. ARDUINO ist
 * dort nur ein leerer Plattformzweig - die Rechenroutinen stehen in
 * assembly.h, das hier gar nicht eingebunden wird. */
#define ARDUINO
#include "vendor/helix-aac/aacdec.h"
#undef ARDUINO

/* ------------------------------------------------------------------ */
/* Strom aus dem Netz in den Ring (Stufe 2)                            */
/* ------------------------------------------------------------------ */

/* Der Fueller laeuft als EIGENER Prozess und ist der einzige, der das
 * Netz anfasst - AmiSSL gehoert damit ihm allein, genau wie in der
 * Oberflaeche dem Netzprozess. Der Hauptprozess dekodiert und spielt
 * und fasst weder bsdsocket noch AmiSSL an.
 *
 * Gemeinsam sind nur der Ring (AllocVec-Speicher) und diese paar
 * Variablen; der Ring hat genau einen Schreiber und einen Leser. */
static struct Ring    g_ring;
static struct Prefs   g_fprefs;
static char           g_fid[SUB_ID_LEN];
static volatile LONG  g_fill_done = 0;
static volatile LONG  g_fill_bytes = 0;
static volatile LONG  g_fill_reconnects = 0;
static volatile LONG  g_fill_id3 = 0;
static volatile LONG  g_fill_waited = 0;   /* Sekunden Warten auf das Netz */

/* MESSUNG des Vorlaufs, in Ticks zu 1/50 s: wie lange brauchen
 * Verbindung, ID3-Etikett und Vorratspuffer, bis Ton kommt? */
static volatile LONG  g_t_conn = 0, g_t_id3 = 0;

/* Zaehler fuer "dl": laeuft auf Prioritaet -127 und zaehlt, solange
 * g_spin_run gilt. Was er schafft, ist die freie CPU. */
static volatile BOOL  g_spin_run = FALSE;
static volatile ULONG g_spin_count = 0;
static struct Task   *g_spin_task = NULL;
static struct Task   *g_spin_parent = NULL;

static void spinner(void)
{
    while (g_spin_run) {
        g_spin_count++;
    }
    /* Melden, dass der Code hier nicht mehr laeuft. Forbid, damit das
     * Signal erst ankommt, wenn dieser Prozess wirklich weg ist. */
    Forbid();
    Signal(g_spin_parent, SIGF_SINGLE);
}

/* Den Zaehler anhalten und WARTEN, bis er weg ist. Nur eine feste Zeit
 * zu warten war falsch: unter Volllast kam er auf Prioritaet -127 nicht
 * mehr dran, das Programm endete vorher, und der Zaehler lief danach in
 * freigegebenen Code - Guru 80000004 auf der TF536 (13.10.2026). Deshalb
 * vorher die Prioritaet anheben, damit er sofort aufhoert. */
static void spin_stop(void)
{
    if (!g_spin_task) {
        return;
    }
    g_spin_run = FALSE;
    SetTaskPri(g_spin_task, 1);
    Wait(SIGF_SINGLE);
    g_spin_task = NULL;
}

static LONG ticks_now(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (LONG)ds.ds_Minute * 3000L + (LONG)ds.ds_Tick;
}
static char           g_fill_err[160];

#define FILL_CHUNK   8192

/* Nicht in Anlaeufen rechnen, sondern in ZEIT. Gemessen am 20.9.2026:
 * fuenf Anlaeufe waren nach keinen zehn Sekunden aufgebraucht, das WLAN
 * dieser Maschine faellt aber regelmaessig ein bis drei Minuten aus
 * (AGENTS.md, Abschnitt 5.8). Der Titel lief aus dem Ring noch zu Ende,
 * die folgenden fielen dann aus.
 *
 * 180 s Geduld, zwischen den Anlaeufen 2 s Ruhe - das ueberbrueckt einen
 * typischen Aussetzer, ohne das Netz zu bombardieren. */
#define FILL_PATIENCE  (180 * 50)   /* in Ticks zu 1/50 s */
#define FILL_PAUSE     100          /* 2 s zwischen zwei Anlaeufen */

/* Woher der Fueller liest. Vorgabe ist ein Titel vom Navidrome-Server;
 * fuer Audiobookshelf wird umgestellt (cmd_abs). So bleibt es EIN
 * Fueller mit derselben Wiederaufnahme nach Netzausfall und demselben
 * ID3-Sprung - nur das Oeffnen ist verschieden. */
static int nav_open(long off, struct SubStream *st)
{
    return sub_stream_open(&g_fprefs, g_fid, off, st);
}
static int  (*g_fopen)(long off, struct SubStream *st) = nav_open;
static const char *(*g_ferrstr)(void) = sub_last_error;

/* Bytes, die nach dem ID3-Etikett noch zu ueberspringen sind - der
 * Einstieg mitten in eine Datei. Erst NACH dem Etikett, weil dessen
 * Laenge vorher keiner kennt. */
static long g_fskip = 0;

/* Dasselbe als ZEIT (Podcasts): die Bitrate ist vorher unbekannt und
 * kommt aus dem ersten Rahmenkopf hinter dem Etikett. */
static long g_fskip_ms = 0;

/* Ab dieser Groesse wird ein ID3-Etikett nicht mehr durchgelesen,
 * sondern per Range uebersprungen - Podcasts betten das Cover ein,
 * gemessen bis 531 KB. */
#define ID3_JUMP  65536

/* Wird gerufen, solange der Fueller wartet (Ring voll) und nachdem die
 * Datei ganz im Ring ist, bis der Hauptprozess Schluss sagt - mit
 * final = TRUE ein letztes Mal. Dort schreibt Audiobookshelf die
 * Hoerposition zurueck: das Netz gehoert diesem Prozess. */
static void (*g_fidle)(BOOL final) = NULL;

static void filler(void)
{
    struct SubStream st;
    LONG t0 = ticks_now();
    UBYTE *chunk;
    long off = 0;
    long waited = 0;            /* Ticks, die schon vergeblich vergingen */
    int  need_sync = 0;         /* nach einem Sprung: erst Rahmen suchen */
    int  phase = 0;             /* 0 Etikett pruefen, 1 Einstieg, 2 Strom */

    chunk = AllocVec(FILL_CHUNK, MEMF_PUBLIC);
    if (!chunk) {
        strcpy(g_fill_err, "zu wenig Speicher");
        g_ring.eof = TRUE;
        g_fill_done = 1;
        return;
    }

    for (;;) {
        long n;

        if (g_ring.stop) {
            break;
        }
        if (g_fopen(off, &st) != SUB_OK) {
            if (waited >= FILL_PATIENCE) {
                strncpy(g_fill_err, g_ferrstr(),
                        sizeof(g_fill_err) - 1);
                break;
            }
            Delay(FILL_PAUSE);
            waited += FILL_PAUSE;
            g_fill_waited = waited / 50;
            continue;
        }
        waited = 0;                     /* Verbindung steht wieder */
        if (off > 0) {
            g_fill_reconnects++;
        }
        if (g_t_conn == 0) {
            g_t_conn = ticks_now() - t0;
        }

        /* Ein MP3 vom Server faengt so gut wie nie mit Musik an,
         * sondern mit einem ID3v2-Etikett - und weil darin das
         * Albumcover steckt, ist es gern 50 KB und mehr gross.
         * mpega.library sucht den MPEG-Anfang nur in den ersten
         * Kilobytes und gibt dann auf: gemessen las sie 48 KB und
         * meldete "kein MPEG". Also wird das Etikett hier uebersprungen,
         * bevor etwas in den Ring geht.
         *
         * Die Laenge steht in vier "synchsafe" Bytes, also je sieben
         * nutzbaren Bits - das achte ist immer 0, damit im Etikett nie
         * ein MPEG-Synchronwort entstehen kann. */
        if (phase == 0 && off == 0) {
            UBYTE hdr[10];
            long got = 0;

            while (got < 10) {
                long n = sub_stream_read(&st, hdr + got, 10 - got);

                if (n <= 0) {
                    break;
                }
                got += n;
            }
            if (got == 10 && hdr[0] == 'I' && hdr[1] == 'D' &&
                hdr[2] == '3') {
                long skip = ((long)(hdr[6] & 0x7f) << 21)
                          | ((long)(hdr[7] & 0x7f) << 14)
                          | ((long)(hdr[8] & 0x7f) <<  7)
                          |  (long)(hdr[9] & 0x7f);

                g_fill_id3 = skip + 10;
                if (skip > ID3_JUMP) {
                    /* Gross: nicht mitlesen, dahinter neu ansetzen. */
                    sub_stream_close(&st);
                    off = skip + 10;
                    phase = 1;
                    continue;
                }
                while (skip > 0) {
                    long want = (skip > FILL_CHUNK) ? FILL_CHUNK : skip;
                    long n = sub_stream_read(&st, chunk, want);

                    if (n <= 0) {
                        break;
                    }
                    skip -= n;
                }
            } else if (got > 0 && g_fskip == 0 && g_fskip_ms == 0) {
                ring_write(&g_ring, hdr, (ULONG)got);
            }
            off = st.pos;
            g_fill_bytes = off;
            g_t_id3 = ticks_now() - t0 - g_t_conn;
            phase = 1;
        }

        /* Einstieg mitten in die Datei: Etikett ist vorbei, jetzt neu
         * ansetzen. Eine zweite Verbindung, aber nur eine kurze -
         * billiger, als bis zur Stelle mitzulesen. */
        if (phase == 1) {
            phase = 2;
            if (g_fskip > 0) {
                sub_stream_close(&st);
                off += g_fskip;
                g_fskip = 0;
                need_sync = 4;      /* hoechstens vier Scheiben suchen */
                continue;
            }
            if (g_fskip_ms > 0) {
                /* Eine Scheibe lesen, darin den ersten echten Rahmen
                 * suchen, seine Bitrate nehmen. Feste Bitrate
                 * angenommen - bei allen gemessenen Podcasts so. */
                long at = st.pos;
                long n2 = sub_stream_read(&st, chunk, FILL_CHUNK);
                LONG k = (n2 > 0) ? mp3_find_frame(chunk, n2) : -1;
                LONG kbps = (k >= 0) ? mp3_frame_kbps(chunk + k) : 0;

                if (kbps > 0) {
                    sub_stream_close(&st);
                    off = at + k + g_fskip_ms * kbps / 8;
                    g_fskip_ms = 0;
                    need_sync = 4;
                    continue;
                }
                /* Kein Rahmen gefunden: von vorn spielen statt gar nicht. */
                g_fskip_ms = 0;
                if (n2 > 0) {
                    ring_write(&g_ring, chunk, (ULONG)n2);
                }
                off = st.pos;
            }
        }

        for (;;) {
            if (g_ring.stop) {
                goto done;
            }
            /* Ist der Ring voll, wird gewartet statt gelesen - sonst
             * liefe der Titel schneller herein, als er gehoert wird,
             * und der Speicher waere umsonst gross. */
            if (ring_space(&g_ring) < FILL_CHUNK) {
                if (g_fidle) {
                    g_fidle(FALSE);
                }
                Delay(2);
                continue;
            }
            n = sub_stream_read(&st, chunk, FILL_CHUNK);
            if (n > 0) {
                ULONG put = 0;

                /* Mitten in der Datei steht vor dem ersten Rahmen
                 * Rauschen mit falschen Synchronwoertern - das darf
                 * mpega.library nicht als Erstes sehen. Was davor
                 * liegt, wird verworfen (unter einer halben Sekunde). */
                if (need_sync > 0) {
                    LONG k = mp3_find_frame(chunk, n);

                    if (k < 0 && --need_sync > 0) {
                        off = st.pos;
                        continue;
                    }
                    put = (k > 0) ? (ULONG)k : 0;
                    need_sync = 0;
                }

                while (put < (ULONG)n && !g_ring.stop) {
                    ULONG w = ring_write(&g_ring, chunk + put,
                                         (ULONG)n - put);
                    if (w == 0) {
                        Delay(2);
                        continue;
                    }
                    put += w;
                }
                off = st.pos;
                g_fill_bytes = off;
                continue;
            }
            if (n == 0) {
                sub_stream_close(&st);
                if (st.total > 0 && off < st.total) {
                    break;              /* abgeschnitten - neu ansetzen */
                }
                goto done;              /* sauber zu Ende */
            }
            /* Fehler: Verbindung weg. Neu ansetzen an derselben Stelle -
             * genau dafuer war die Range-Messung (siehe Projekt.md). */
            sub_stream_close(&st);
            break;
        }

        if (waited >= FILL_PATIENCE) {
            strcpy(g_fill_err, "Verbindung bricht immer wieder ab");
            break;
        }
        Delay(FILL_PAUSE);
        waited += FILL_PAUSE;
        g_fill_waited = waited / 50;
    }

done:
    sub_stream_close(&st);
    FreeVec(chunk);
    g_ring.eof = TRUE;

    /* Die Datei ist im Ring, gespielt wird sie noch eine Weile - bei
     * 1 MB Ring und 128 kbps gut eine Minute. Wer in dieser Zeit
     * Positionen abgleichen muss, bleibt solange hier. */
    if (g_fidle) {
        while (!g_ring.stop) {
            g_fidle(FALSE);
            Delay(10);
        }
        g_fidle(TRUE);
    }
    g_fill_done = 1;
    sub_cleanup();                      /* AmiSSL gehoert diesem Prozess */
}

/* Fueller fuer einen Radiosender. Er holt die Senderliste SELBST, statt
 * die Adresse vom Hauptprozess zu bekommen: sonst haette der Haupt-
 * prozess AmiSSL geoeffnet und dieser hier wuerde es mitbenutzen - und
 * AmiSSL gehoert genau einem Prozess (AGENTS.md 7).
 *
 * Anders als beim Titel gibt es kein Range und kein Ende. Bricht die
 * Verbindung ab, wird neu verbunden und live weitergehoert; was
 * dazwischen gesendet wurde, ist verloren - bei einem Sender geht es
 * nicht anders. */
static int           g_fradio = 0;          /* Nummer ab 1 */
static char          g_fr_name[SUB_NAME_LEN];
static char          g_fr_ctype[40];
static volatile LONG g_fr_hops = 0, g_fr_br = 0, g_fr_ok = 0;

static void radio_filler(void)
{
    struct SubStream st;
    struct SubList l;
    struct Radio *r;
    UBYTE *chunk;
    char url[512];
    long waited = 0;
    long n;

    st.sock = -1;
    chunk = AllocVec(FILL_CHUNK, MEMF_PUBLIC);
    if (!chunk) {
        strcpy(g_fill_err, "zu wenig Speicher");
        goto out;
    }

    list_init(&l, sizeof(struct Radio));
    if (sub_get_radios(&g_fprefs, &l) != SUB_OK) {
        strncpy(g_fill_err, sub_last_error(), sizeof(g_fill_err) - 1);
        list_free(&l);
        goto out;
    }
    r = (struct Radio *)list_get(&l, g_fradio - 1);
    if (!r) {
        sprintf(g_fill_err, "keinen Sender Nr. %d (es gibt %d)",
                g_fradio, l.count);
        list_free(&l);
        goto out;
    }
    strncpy(url, r->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    strncpy(g_fr_name, r->name, sizeof(g_fr_name) - 1);
    list_free(&l);

    while (!g_ring.stop) {
        if (sub_radio_open(url, FALSE, &st) != SUB_OK) {
            if (waited >= FILL_PATIENCE) {
                strncpy(g_fill_err, sub_last_error(),
                        sizeof(g_fill_err) - 1);
                break;
            }
            Delay(FILL_PAUSE);
            waited += FILL_PAUSE;
            g_fill_waited = waited / 50;
            continue;
        }
        if (g_fr_ok) {
            g_fill_reconnects++;
        }
        waited = 0;
        strncpy(g_fr_ctype, st.ctype, sizeof(g_fr_ctype) - 1);
        g_fr_hops = st.hops;
        g_fr_br = st.bitrate;
        g_fr_ok = 1;

        if (!sub_radio_is_mp3(st.ctype)) {
            sprintf(g_fill_err, "kein MP3, sondern %s - mpega.library "
                    "kann das nicht", st.ctype);
            break;
        }

        while (!g_ring.stop) {
            if (ring_space(&g_ring) < FILL_CHUNK) {
                Delay(2);
                continue;
            }
            n = sub_stream_read(&st, chunk, FILL_CHUNK);
            if (n <= 0) {
                break;                  /* weg - neu verbinden */
            }
            {
                ULONG put = 0;

                while (put < (ULONG)n && !g_ring.stop) {
                    ULONG w = ring_write(&g_ring, chunk + put,
                                         (ULONG)n - put);
                    if (w == 0) {
                        Delay(2);
                        continue;
                    }
                    put += w;
                }
            }
            g_fill_bytes += n;
        }
        sub_stream_close(&st);
    }

out:
    sub_stream_close(&st);
    if (chunk) {
        FreeVec(chunk);
    }
    g_ring.eof = TRUE;
    g_fill_done = 1;
    sub_cleanup();                      /* AmiSSL gehoert diesem Prozess */
}


/* ------------------------------------------------------------------ */
/* AAC: Dekodiertempo messen (Helix, Stufe 0 wie "mp3")               */
/* ------------------------------------------------------------------ */

/* Zwei der acht Sender dieser Instanz senden HE-AAC. Bevor der Dekoder
 * in den Audioprozess kommt, muss feststehen, dass er auf der Maschine
 * in Echtzeit laeuft - SBR verdoppelt die Abtastrate und ist der teure
 * Teil. Gemessen wird deshalb NUR das Dekodieren: der Strom liegt vorher
 * vollstaendig im Speicher, Netz und AHI zaehlen nicht mit.
 *
 * Eine Sendernummer holt das Stueck im Hauptprozess. Das ist erlaubt,
 * weil hier kein Fueller laeuft - AmiSSL hat also nur einen Besitzer.
 * Das Stueck landet in T:AmiSubsonic.aac, damit man dieselbe Messung
 * ohne Netz wiederholen kann (die WLAN-Strecke faellt gern aus). */
#define AAC_SAVE "T:AmiSubsonic.aac"

static long aac_fetch_radio(struct Prefs *p, int nr, UBYTE *buf, long cap)
{
    struct SubList l;
    struct SubStream st;
    struct Radio *r;
    char url[512];
    long len = 0, n;
    BPTR fh;

    list_init(&l, sizeof(struct Radio));
    if (sub_get_radios(p, &l) != SUB_OK) {
        list_free(&l);
        return -1;
    }
    r = (struct Radio *)list_get(&l, nr - 1);
    if (!r) {
        printf("keinen Sender Nr. %d (es gibt %d)\n", nr, l.count);
        list_free(&l);
        return -2;
    }
    strncpy(url, r->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    printf("%s\n", r->name);
    list_free(&l);

    /* icy = FALSE: sonst stecken Titelbloecke mitten im AAC-Strom. */
    if (sub_radio_open(url, FALSE, &st) != SUB_OK) {
        return -1;
    }
    printf("%s, %ld kbps laut Sender, hole %ld KB ...\n",
           st.ctype[0] ? st.ctype : "(kein Typ)", (long)st.bitrate,
           cap / 1024L);
    while (len < cap) {
        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
            break;
        }
        n = sub_stream_read(&st, buf + len, (cap - len > 8192L) ? 8192L
                                                              : cap - len);
        if (n <= 0) {
            break;
        }
        len += n;
    }
    sub_stream_close(&st);

    fh = Open((STRPTR)AAC_SAVE, MODE_NEWFILE);
    if (fh) {
        Write(fh, buf, len);
        Close(fh);
        printf("gesichert in %s\n", AAC_SAVE);
    }
    return len;
}

/* dump != NULL schreibt das PCM roh in eine Datei (16 Bit, Big Endian,
 * verschraenkt) - zum Vergleich mit einem Lauf auf dem Mac, der das
 * Endian-Problem in assembly.h aufdecken wuerde. Die Zeitmessung ist
 * dann NICHT zu gebrauchen, das Schreiben zaehlt mit. */
static int cmd_aac(struct Prefs *p, const char *what, long kb,
                   const char *dump)
{
    BPTR out = 0;
    static short pcm[AAC_MAX_NCHANS * AAC_MAX_NSAMPS * 2];  /* *2: SBR */
    HAACDecoder h;
    AACFrameInfo fi, first;
    UBYTE *buf, *ptr;
    long cap = kb * 1024L, len;
    int left, off, err, flen, first_err = 0;
    ULONG frames = 0, samples = 0, errs = 0, cs_dec, cs_play;
    LONG t0;

    buf = AllocVec(cap, MEMF_ANY);
    if (!buf) {
        printf("zu wenig Speicher fuer %ld KB\n", kb);
        return SUB_OK;
    }

    if (what[0] >= '0' && what[0] <= '9') {
        len = aac_fetch_radio(p, atoi(what), buf, cap);
        if (len == -1) {
            FreeVec(buf);
            return SUB_ENET;
        }
    } else {
        BPTR fh = Open((STRPTR)what, MODE_OLDFILE);

        len = -2;
        if (fh) {
            len = Read(fh, buf, cap);
            Close(fh);
        } else {
            printf("kann %s nicht oeffnen\n", what);
        }
    }
    if (len <= 0) {
        FreeVec(buf);
        return SUB_OK;
    }

    h = AACInitDecoder();
    if (!h) {
        printf("AACInitDecoder: zu wenig Speicher\n");
        FreeVec(buf);
        return SUB_OK;
    }

    if (dump) {
        out = Open((STRPTR)dump, MODE_NEWFILE);
    }
    memset(&first, 0, sizeof(first));
    ptr = buf;
    left = (int)len;
    t0 = ticks_now();
    while (left > 0) {
        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
            break;
        }
        /* Nur ADTS - so senden es Radiosender. Die Suche steht auch
         * nach jedem Fehler, damit ein kaputter Block nicht den Rest
         * des Stroms mitnimmt. */
        off = AACFindSyncWord(ptr, left);
        if (off < 0) {
            break;
        }
        ptr += off;
        left -= off;
        /* Helix verlaesst sich darauf, dass der GANZE Block im Puffer
         * liegt, und liest sonst ueber das Ende hinaus (gemessen auf dem
         * Mac mit AddressSanitizer, 22.9.2026: der angeschnittene letzte
         * Block). Die Laenge steht im ADTS-Kopf, 13 Bit ab Bit 30. */
        if (left < 7) {
            break;
        }
        flen = ((ptr[3] & 3) << 11) | (ptr[4] << 3) | (ptr[5] >> 5);
        if (flen < 7) {
            ptr++;                      /* falsches Sync, weitersuchen */
            left--;
            continue;
        }
        if (flen > left) {
            break;                      /* letzter Block angeschnitten */
        }
        err = AACDecode(h, &ptr, &left, pcm);
        if (err == ERR_AAC_NONE) {
            AACGetLastFrameInfo(h, &fi);
            if (frames == 0) {
                first = fi;
            }
            frames++;
            if (fi.nChans > 0) {
                samples += (ULONG)(fi.outputSamps / fi.nChans);
            }
            if (out) {
                Write(out, pcm, (LONG)fi.outputSamps * 2L);
            }
        } else if (err == ERR_AAC_INDATA_UNDERFLOW) {
            break;                      /* letzter Block angeschnitten */
        } else {
            if (errs == 0) {
                first_err = err;
            }
            errs++;
            ptr++;                      /* weiter zum naechsten Sync */
            left--;
        }
    }
    cs_dec = (ULONG)(ticks_now() - t0) * 2UL;
    if (out) {
        Close(out);
    }
    AACFreeDecoder(h);
    FreeVec(buf);

    printf("%ld Bytes, %lu Bloecke, %lu Fehler", len, frames, errs);
    if (errs) {
        printf(" (erster: %d)", first_err);
    }
    printf("\n");
    if (frames == 0 || first.sampRateOut <= 0) {
        printf("kein einziger AAC-Block dekodiert\n");
        return SUB_OK;
    }
    /* In Hundertsteln rechnen: samples * 1000 liefe bei langen
     * Stuecken ueber 32 Bit. */
    cs_play = (samples * 100UL) / (ULONG)first.sampRateOut;

    /* Die Bitrate aus Bytes und Spieldauer - Helix fuellt bitRate bei
     * ADTS nicht (gemessen: immer 0). */
    printf("%s, Profil %d, %d Kanaele, %d Hz (Kern %d Hz), %lu kbps\n",
           first.sampRateOut != first.sampRateCore ? "HE-AAC (SBR)"
                                                    : "AAC-LC",
           first.profile, first.nChans, first.sampRateOut,
           first.sampRateCore,
           cs_play ? ((ULONG)len * 8UL) / (cs_play * 10UL) : 0UL);
    printf("Spieldauer   %lu.%02lu s\n", cs_play / 100UL, cs_play % 100UL);
    printf("Dekodierzeit %lu.%02lu s\n", cs_dec / 100UL, cs_dec % 100UL);
    if (cs_dec > 0) {
        printf("Faktor       %lu.%02lu-fache Echtzeit, %lu %% der Maschine\n",
               cs_play / cs_dec, (cs_play * 100UL / cs_dec) % 100UL,
               cs_play ? cs_dec * 100UL / cs_play : 0UL);
    } else {
        printf("Faktor       zu schnell zum Messen\n");
    }
    return SUB_OK;
}

static void usage(void)
{
    printf(
"AmiSubsonic CLI - Navidrome/Subsonic von der Shell aus\n"
"\n"
"  setup <host[:port]> <benutzer> <passwort>   Zugang nach ENVARC: sichern\n"
"  ping                                        Anmeldung pruefen\n"
"  artists                                     alle Interpreten\n"
"  albums [art] [anzahl]                       Alben, art z.B. newest\n"
"  artist <id>                                 Alben eines Interpreten\n"
"  album <id>                                  Titel eines Albums\n"
"  search <text>                               Titel suchen\n"
"  similar <titel-id>                          aehnliche Titel (RELATED)\n"
"  lyrics <titel-id>                           Liedtext (LYRICS)\n"
"  url <titel-id>                              Stream-URL fuer AmigaAMP\n"
"  raw <methode> [extra] [bytes]               rohe Serverantwort ansehen\n"
"  cover <coverart-id> [datei]                 Cover holen, Vorgabe T:cover\n"
"  cache [coverart-id]                         Cache-Pfad, bzw. Cover cachen\n"
"  farbe <datei>                               Hauptfarbe eines Bildes\n"
"  mp3 <datei>                                 Dekodiertempo messen\n"
"  spiel <datei> [ahi-unit]                    Datei ueber AHI abspielen\n"
"  stream <titel-id> [weitere ...]             Titel aus dem Netz spielen\n"
"  radio [nr]                                  Sender auflisten / spielen\n"
"  aac <datei|sender-nr> [kbytes] [pcm-datei]  AAC-Dekodiertempo messen\n"
"  ptest <titel-id> [ahi-unit]                 Audioprozess: Pause, Position\n"
"  range <titel-id> [offset] [format] [kbps]   Einstieg mitten im Titel\n"
"  abs ...                                     Audiobookshelf (Hoerbuecher)\n"
"  pod ...                                     Podcasts\n"
"\n"
"\n"
"Der Zugang steht in ENVARC:AmiSubsonic/AmiSubsonic.prefs.\n");
}

static int need_prefs(struct Prefs *p)
{
    int rc = prefs_load(p);

    if (rc != SUB_OK) {
        printf("%s\n", sub_last_error());
        printf("Zuerst einrichten:  AmiSubsonicCLI setup <host> <user> <pass>\n");
    }
    return rc;
}

static void report(int rc)
{
    if (rc != SUB_OK) {
        printf("Fehler: %s\n", sub_last_error());
    }
}

static int cmd_artists(struct Prefs *p)
{
    struct SubList l;
    int rc, i;

    list_init(&l, sizeof(struct Artist));
    rc = sub_get_artists(p, &l);
    if (rc == SUB_OK) {
        for (i = 0; i < l.count; i++) {
            struct Artist *a = (struct Artist *)list_get(&l, i);
            printf("%-24s %3d  %s\n", a->id, a->albumcount, a->name);
        }
        printf("%d Interpreten\n", l.count);
    }
    list_free(&l);
    return rc;
}

static void print_albums(struct SubList *l)
{
    int i;

    for (i = 0; i < l->count; i++) {
        struct Album *a = (struct Album *)list_get(l, i);
        printf("%-24s %4d %2d  %-28s %s\n",
               a->id, a->year, a->songcount, a->artist, a->name);
    }
    printf("%d Alben\n", l->count);
}

static int cmd_albums(struct Prefs *p, const char *type, int size)
{
    struct SubList l;
    int rc;

    list_init(&l, sizeof(struct Album));
    rc = sub_get_albums(p, type, size, 0, &l);
    if (rc == SUB_OK) {
        print_albums(&l);
    }
    list_free(&l);
    return rc;
}

static int cmd_artist(struct Prefs *p, const char *id)
{
    struct SubList l;
    int rc;

    list_init(&l, sizeof(struct Album));
    rc = sub_get_artist_albums(p, id, &l);
    if (rc == SUB_OK) {
        print_albums(&l);
    }
    list_free(&l);
    return rc;
}

static void print_songs(struct SubList *l)
{
    int i;

    for (i = 0; i < l->count; i++) {
        struct Song *s = (struct Song *)list_get(l, i);
        char dur[16];

        duration_text(s->duration, dur, sizeof(dur));
        printf("%-24s %2d %6s %-4s %-24s %s\n",
               s->id, s->track, dur, s->suffix, s->artist, s->title);
    }
    printf("%d Titel\n", l->count);
}

static int cmd_album(struct Prefs *p, const char *id)
{
    struct SubList l;
    struct Album info;
    int rc;

    list_init(&l, sizeof(struct Song));
    rc = sub_get_album_songs(p, id, &info, &l);
    if (rc == SUB_OK) {
        printf("%s - %s (%d), Cover-ID %s\n",
               info.artist, info.name, info.year,
               info.coverart[0] ? info.coverart : "(keine)");
        print_songs(&l);
    }
    list_free(&l);
    return rc;
}

static int cmd_search(struct Prefs *p, const char *q)
{
    struct SubList l;
    int rc;

    list_init(&l, sizeof(struct Song));
    rc = sub_search_songs(p, q, 30, 0, &l);
    if (rc == SUB_OK) {
        print_songs(&l);
    }
    list_free(&l);
    return rc;
}

static int cmd_similar(struct Prefs *p, const char *id)
{
    struct SubList l;
    int rc;

    list_init(&l, sizeof(struct Song));
    rc = sub_get_similar(p, id, 20, &l);
    if (rc == SUB_OK) {
        print_songs(&l);
    }
    list_free(&l);
    return rc;
}

/* Der eigentliche Ablauf. main() drumherum sorgt dafuer, dass
 * sub_cleanup() auf JEDEM Weg nach draussen laeuft - der Kern haelt
 * bsdsocket und AmiSSL ab dem ersten Zugriff offen, und ein Programm,
 * das AmiSSL nicht aufraeumt, kann laut dessen Doku die naechste
 * Anwendung mitreissen. Bei einem Dutzend return-Stellen ist eine
 * Klammer sicherer als ein Aufruf vor jedem einzelnen. */
/* ------------------------------------------------------------------ */
/* Audiobookshelf                                                      */
/* ------------------------------------------------------------------ */

/* Was der Fueller fuer Audiobookshelf braucht. Die Liste der Dateien
 * holt der Hauptprozess VOR dem ersten Fueller; danach fasst er das
 * Netz nicht mehr an (sub_cleanup), denn ab dann gehoert es dem
 * jeweiligen Fueller. */
static char           g_abs_item[SUB_ID_LEN];
static struct AbsTrack g_abs_cur;
static long           g_abs_dur = 0;

/* Die Stelle im BUCH, die gerade zu hoeren ist. Schreibt der
 * Hauptprozess, liest der Fueller. Ein LONG ist auf dem 68k unteilbar. */
static volatile LONG  g_abs_pos = -1;
static LONG           g_abs_synced = -1;
static LONG           g_abs_tsync = 0;
static volatile LONG  g_abs_syncs = 0;
static char           g_abs_syncerr[120];

/* Alle 30 s zurueckschreiben. Faellt der Amiga aus, fehlen auf dem
 * naechsten Geraet hoechstens diese 30 s. Oefter braucht es nicht -
 * jeder Abgleich ist eine eigene Verbindung. */
#define ABS_SYNC_TICKS  (30 * 50)

static int abs_fopen(long off, struct SubStream *st)
{
    return abs_open_file(&g_fprefs, g_abs_item, g_abs_cur.ino, off, st);
}

static void abs_idle(BOOL final)
{
    LONG now = ticks_now();
    LONG pos = g_abs_pos;

    if (pos < 0 || pos == g_abs_synced) {
        return;
    }
    /* now < g_abs_tsync: Mitternacht, ticks_now() faengt neu an. */
    if (!final && now >= g_abs_tsync && now - g_abs_tsync < ABS_SYNC_TICKS) {
        return;
    }
    g_abs_tsync = now;
    if (abs_set_pos(&g_fprefs, g_abs_item, pos, g_abs_dur) == SUB_OK) {
        g_abs_synced = pos;
        g_abs_syncs++;
        g_abs_syncerr[0] = '\0';
    } else {
        strncpy(g_abs_syncerr, abs_last_error(), sizeof(g_abs_syncerr) - 1);
    }
}

static void hms(long ms, char *out)
{
    duration_text((int)(ms / 1000), out, 16);
}

/* "3" ist die dritte Zeile aus "abs books", alles Laengere eine ID. */
static int abs_pick(struct Prefs *p, const char *arg, struct AbsBook *b)
{
    struct SubList l;
    int rc, nr;

    memset(b, 0, sizeof(*b));
    if (strlen(arg) > 4) {
        strncpy(b->id, arg, sizeof(b->id) - 1);
        return abs_get_pos(p, b->id, &b->pos_ms, &b->finished);
    }
    nr = atoi(arg);
    list_init(&l, sizeof(struct AbsBook));
    rc = abs_books(p, &l);
    if (rc == SUB_OK) {
        if (nr < 1 || nr > l.count) {
            printf("kein Buch Nr. %d (es gibt %d)\n", nr, l.count);
            rc = SUB_EAPI;
        } else {
            *b = *(struct AbsBook *)list_get(&l, nr - 1);
        }
    }
    list_free(&l);
    return rc;
}

static int abs_play(struct Prefs *p, struct AbsBook *b, long start_ms)
{
    struct SubList tr;
    LONG t0;
    long in_ms = 0;
    int rc, k, first;
    BOOL quit = FALSE;
    char t1[16], t2[16];

    list_init(&tr, sizeof(struct AbsTrack));
    t0 = ticks_now();
    rc = abs_tracks(p, b->id, &tr, &g_abs_dur);
    if (rc != SUB_OK) {
        list_free(&tr);
        return rc;
    }
    printf("%d Dateien, Liste geholt in %ld.%02ld s\n", tr.count,
           (long)((ticks_now() - t0) / 50),
           (long)(((ticks_now() - t0) % 50) * 2));
        fflush(stdout);

    /* Ab hier gehoert das Netz den Fuellern. */
    sub_cleanup();

    if (start_ms < 0) {
        start_ms = b->pos_ms;
    }
    first = abs_find_track(&tr, start_ms, &in_ms);
    hms(start_ms, t1);
    hms(g_abs_dur, t2);
    printf("Start bei %s von %s - Datei %d, %ld.%03ld s hinein\n",
           t1, t2, first + 1, in_ms / 1000, in_ms % 1000);
        fflush(stdout);

    if (!ring_init(&g_ring, 1024UL * 1024UL)) {
        printf("zu wenig Speicher fuer den Ringpuffer\n");
        fflush(stdout);
        list_free(&tr);
        return SUB_EMEM;
    }
    if (!audio_start(p->ahiunit)) {
        printf("%s\n", audio_last_error());
        fflush(stdout);
        ring_free(&g_ring);
        list_free(&tr);
        return SUB_EMEM;
    }
    audio_set_volume(64);

    g_fprefs = *p;
    strncpy(g_abs_item, b->id, sizeof(g_abs_item) - 1);
    g_fopen = abs_fopen;
    g_ferrstr = abs_last_error;
    g_fidle = abs_idle;
    g_abs_pos = start_ms;
    g_abs_synced = start_ms;    /* steht schon so auf dem Server */
    g_abs_tsync = ticks_now();
    g_abs_syncs = 0;
    g_abs_syncerr[0] = '\0';

    for (k = first; k < tr.count && !quit; k++) {
        struct Process *proc;
        LONG last_print = 0;

        g_abs_cur = *(struct AbsTrack *)list_get(&tr, k);
        g_fskip = (in_ms > 0) ? abs_byte_for_ms(&g_abs_cur, in_ms) : 0;

        ring_reset(&g_ring);
        g_fill_done = 0;
        g_fill_err[0] = '\0';
        g_fill_reconnects = 0;
        g_fill_id3 = 0;
        g_fill_bytes = 0;

        proc = CreateNewProcTags(NP_Entry,     (ULONG)filler,
                                 NP_Name,      (ULONG)"AmiSubsonic.fill",
                                 NP_StackSize, 65536,
                                 NP_Priority,  0,
                                 TAG_DONE);
        if (!proc) {
            printf("Fuellprozess laesst sich nicht starten\n");
        fflush(stdout);
            break;
        }

        /* Vorrat sammeln. Strg-C muss auch hier greifen, siehe
         * "stream" - bei totem Netz haengt man sonst fest. */
        while (ring_used(&g_ring) < 64UL * 1024UL && !g_ring.eof) {
            if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                quit = TRUE;
                break;
            }
            Delay(5);
        }
        if (g_fill_err[0]) {
            printf("Fueller: %s\n", g_fill_err);
        fflush(stdout);
            quit = TRUE;
        }
        if (!quit) {
            hms(g_abs_cur.start_ms + in_ms, t1);
            printf("[%d/%d] %s  (ab %s)\n", k + 1, tr.count,
                   g_abs_cur.title, t1);
        fflush(stdout);
            audio_play(&g_ring, in_ms);

            for (;;) {
                Delay(25);
                if (audio_state() == AU_PLAYING) {
                    g_abs_pos = g_abs_cur.start_ms + audio_pos_ms();
                }
                if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                    audio_halt();
                    quit = TRUE;
                    break;
                }
                if (audio_track_done()) {
                    hms(g_abs_pos, t1);
                    printf("    Datei zu Ende bei %s (%ld ms in der Datei, "
                           "Datei hat %ld ms, %ld Bytes geholt von %ld)\n",
                           t1, (long)audio_pos_ms(), g_abs_cur.dur_ms,
                           (long)g_fill_bytes, g_abs_cur.size);
                    fflush(stdout);
                    break;
                }
                if (audio_error()) {
                    printf("Datei nicht lesbar (%s; ID3 %ld B, Stelle %ld, "
                           "Neuanlaeufe %ld, Ring %ld B%s%s) - weiter mit "
                           "der naechsten\n", audio_last_error(),
                           (long)g_fill_id3, (long)g_fill_bytes,
                           (long)g_fill_reconnects,
                           (long)ring_used(&g_ring),
                           g_fill_err[0] ? ", Fueller: " : "", g_fill_err);
        fflush(stdout);
                    break;
                }
                if (ticks_now() - last_print >= 15 * 50 ||
                    ticks_now() < last_print) {
                    last_print = ticks_now();
                    hms(g_abs_pos, t1);
                    printf("    %s  abgeglichen %ld mal%s%s\n", t1,
                           (long)g_abs_syncs,
                           g_abs_syncerr[0] ? " - FEHLER: " : "",
                           g_abs_syncerr);
        fflush(stdout);
                }
            }
        }

        /* Schluss fuer diese Datei. Der Fueller gleicht dabei ein
         * letztes Mal ab - beim Abbruch ist das die wichtigste Stelle. */
        g_ring.stop = TRUE;
        while (!g_fill_done) {
            Delay(2);
        }
        in_ms = 0;
    }

    audio_shutdown();
    ring_free(&g_ring);
    hms(g_abs_synced, t1);
    printf("Stand auf dem Server: %s (%ld Abgleiche)%s%s\n", t1,
           (long)g_abs_syncs, g_abs_syncerr[0] ? " - letzter Fehler: " : "",
           g_abs_syncerr);
        fflush(stdout);
    list_free(&tr);
    return SUB_OK;
}

/* abs setup <url> <benutzer> <passwort>
 * abs books | tracks <nr> | pos <nr> [sek] | play <nr> [sek] */
static int cmd_abs(struct Prefs *p, int argc, char **argv)
{
    struct AbsBook b;
    const char *sub = argc > 0 ? argv[0] : "";
    char t1[16], t2[16];
    int rc = SUB_OK;

    if (stricmp(sub, "setup") == 0 && argc >= 4) {
        int n;

        strncpy(p->absurl, argv[1], sizeof(p->absurl) - 1);
        n = (int)strlen(p->absurl);
        while (n > 0 && p->absurl[n - 1] == '/') {
            p->absurl[--n] = '\0';
        }
        strncpy(p->absuser, argv[2], sizeof(p->absuser) - 1);
        strncpy(p->abspass, argv[3], sizeof(p->abspass) - 1);
        rc = prefs_save(p);
        if (rc == SUB_OK) {
            printf("gesichert: %s  Benutzer %s\n", p->absurl, p->absuser);
            rc = abs_login(p);
            if (rc == SUB_OK) {
                printf("Anmeldung in Ordnung.\n");
            }
        }
    } else if (stricmp(sub, "login") == 0) {
        rc = abs_login(p);
        if (rc == SUB_OK) {
            printf("%s - Anmeldung in Ordnung.\n", p->absurl);
        }
    } else if (stricmp(sub, "books") == 0) {
        struct SubList l;
        LONG t0 = ticks_now();
        int i;

        list_init(&l, sizeof(struct AbsBook));
        rc = abs_books(p, &l);
        if (rc == SUB_OK) {
            for (i = 0; i < l.count; i++) {
                struct AbsBook *x = (struct AbsBook *)list_get(&l, i);
                long pct = x->dur_ms > 0 ? x->pos_ms / (x->dur_ms / 100 + 1) : 0;

                hms(x->pos_ms, t1);
                hms(x->dur_ms, t2);
                printf("%2d %-36.36s %-20.20s %s / %s (%ld%%)%s\n", i + 1,
                       x->title, x->author, t1, t2, pct,
                       x->finished ? " fertig" : "");
            }
            printf("%d Buecher in %ld.%02ld s\n", l.count,
                   (long)((ticks_now() - t0) / 50),
                   (long)(((ticks_now() - t0) % 50) * 2));
        }
        list_free(&l);
    } else if (stricmp(sub, "tracks") == 0 && argc >= 2) {
        struct SubList l;
        LONG t0;
        long dur = 0;
        int i;

        rc = abs_pick(p, argv[1], &b);
        if (rc != SUB_OK) {
            goto out;
        }
        list_init(&l, sizeof(struct AbsTrack));
        t0 = ticks_now();
        rc = abs_tracks(p, b.id, &l, &dur);
        if (rc == SUB_OK) {
            for (i = 0; i < l.count; i++) {
                struct AbsTrack *x = (struct AbsTrack *)list_get(&l, i);

                hms(x->start_ms, t1);
                hms(x->dur_ms, t2);
                printf("%3d %8s %6s %4ld kbps %s\n", i + 1, t1, t2,
                       x->bitrate / 1000, x->title);
            }
            hms(dur, t1);
            printf("%d Dateien, %s, geholt und gelesen in %ld.%02ld s\n",
                   l.count, t1, (long)((ticks_now() - t0) / 50),
                   (long)(((ticks_now() - t0) % 50) * 2));
        }
        list_free(&l);
    } else if (stricmp(sub, "pos") == 0 && argc >= 2) {
        rc = abs_pick(p, argv[1], &b);
        if (rc != SUB_OK) {
            goto out;
        }
        if (argc >= 3) {
            /* Nur zum Pruefen des Abgleichs: Stelle von Hand setzen.
             * Die Dauer kommt aus der Buecherliste - bei Angabe einer
             * ID fehlt sie, dann aus den Dateien. */
            long ms = atol(argv[2]) * 1000L;

            if (b.dur_ms == 0) {
                struct SubList l;

                list_init(&l, sizeof(struct AbsTrack));
                rc = abs_tracks(p, b.id, &l, &b.dur_ms);
                list_free(&l);
                if (rc != SUB_OK) {
                    goto out;
                }
            }
            rc = abs_set_pos(p, b.id, ms, b.dur_ms);
            if (rc != SUB_OK) {
                goto out;
            }
            rc = abs_get_pos(p, b.id, &b.pos_ms, &b.finished);
            if (rc != SUB_OK) {
                goto out;
            }
            printf("gesetzt, ");
        }
        hms(b.pos_ms, t1);
        printf("Position %s (%ld.%03ld s)%s\n", t1, b.pos_ms / 1000,
               b.pos_ms % 1000, b.finished ? ", fertig gehoert" : "");
    } else if (stricmp(sub, "raw") == 0 && argc >= 2) {
        /* Nie raten, was der Server schickt (AGENTS.md 1): die rohe
         * Antwort landet in einer Datei, die Laenge auf dem Schirm. */
        char *resp = NULL;
        long len = 0;
        const char *dest = argc >= 3 ? argv[2] : "T:abs.json";

        rc = abs_raw(p, argv[1], &resp, &len);
        if (rc == SUB_OK) {
            BPTR fh = Open((STRPTR)dest, MODE_NEWFILE);

            if (fh) {
                Write(fh, resp, len);
                Close(fh);
            }
            printf("HTTP %d, %ld Bytes -> %s\n", sub_last_http_status(),
                   len, dest);
            free(resp);
        }
    } else if (stricmp(sub, "play") == 0 && argc >= 2) {
        rc = abs_pick(p, argv[1], &b);
        if (rc != SUB_OK) {
            goto out;
        }
        if (b.title[0]) {
            printf("%s - %s\n", b.author, b.title);
        }
        rc = abs_play(p, &b, argc >= 3 ? atol(argv[2]) * 1000L : -1L);
    } else {
        printf("abs setup <url> <benutzer> <passwort>   z.B. http://rechner:13378\n"
               "abs login                       Anmeldung pruefen\n"
               "abs books                       Buecher mit Hoerposition\n"
               "abs tracks <nr|id>              Dateien eines Buchs\n"
               "abs pos <nr|id> [sekunden]      Position lesen / setzen\n"
               "abs raw <pfad> [datei]          rohe Antwort, Vorgabe T:abs.json\n"
               "abs play <nr|id> [sekunden]     abspielen, Position abgleichen\n");
        return 5;
    }

out:
    if (rc != SUB_OK) {
        printf("Fehler: %s\n", abs_last_error());
        return 10;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Podcasts                                                            */
/* ------------------------------------------------------------------ */

static char g_pod_url[POD_URL_LEN];

static int pod_fopen(long off, struct SubStream *st)
{
    return sub_url_open(g_pod_url, NULL, off, st);
}

/* Gross genug fuer den Stapel ist eine PodFeed (rund 70 KB) nicht -
 * also statisch. Im CLI gibt es nur den einen Hauptprozess, der sie
 * anfasst. */
static struct PodFeed g_pf;

/* "3" ist die dritte Zeile aus "pod list". */
static int pod_pick(const char *arg)
{
    struct SubList l;
    int nr = atoi(arg), rc;

    list_init(&l, sizeof(struct PodShow));
    pod_list(&l);
    if (nr < 1 || nr > l.count) {
        printf("kein Podcast Nr. %d (es gibt %d)\n", nr, l.count);
        list_free(&l);
        return SUB_EAPI;
    }
    rc = pod_load(((struct PodShow *)list_get(&l, nr - 1))->id, &g_pf);
    list_free(&l);
    return rc;
}

static void ep_state(const struct PodEpisode *e, char *out)
{
    char t[16];

    if (e->done) {
        strcpy(out, "gehoert");
    } else if (e->pos_ms > 0) {
        duration_text((int)(e->pos_ms / 1000), t, sizeof(t));
        sprintf(out, "bei %s", t);
    } else {
        strcpy(out, "neu");
    }
}

static int pod_play(int epnr, long start_ms, LONG unit)
{
    struct PodEpisode *e;
    struct Process *proc;
    LONG last_save, last_print = 0;
    BOOL quit = FALSE, natural = FALSE;
    char t1[16], t2[16];

    if (epnr < 1 || epnr > g_pf.count) {
        printf("keine Folge Nr. %d\n", epnr);
        return SUB_EAPI;
    }
    e = &g_pf.ep[epnr - 1];
    if (start_ms < 0) {
        start_ms = e->done ? 0 : e->pos_ms;
    }
    strncpy(g_pod_url, e->url, sizeof(g_pod_url) - 1);

    /* Ab hier gehoert das Netz dem Fueller. */
    sub_cleanup();

    if (!ring_init(&g_ring, 1024UL * 1024UL)) {
        printf("zu wenig Speicher fuer den Ringpuffer\n");
        return SUB_EMEM;
    }
    if (!audio_start(unit)) {
        printf("%s\n", audio_last_error());
        ring_free(&g_ring);
        return SUB_EMEM;
    }
    audio_set_volume(64);

    g_fopen = pod_fopen;
    g_ferrstr = sub_last_error;
    g_fidle = NULL;
    g_fskip = 0;
    g_fskip_ms = start_ms;
    ring_reset(&g_ring);
    g_fill_done = 0;
    g_fill_err[0] = '\0';
    g_fill_reconnects = 0;
    g_fill_id3 = 0;
    g_fill_bytes = 0;

    proc = CreateNewProcTags(NP_Entry,     (ULONG)filler,
                             NP_Name,      (ULONG)"AmiSubsonic.fill",
                             NP_StackSize, 65536,
                             NP_Priority,  0,
                             TAG_DONE);
    if (!proc) {
        printf("Fuellprozess laesst sich nicht starten\n");
        audio_shutdown();
        ring_free(&g_ring);
        return SUB_EMEM;
    }
    while (ring_used(&g_ring) < 64UL * 1024UL && !g_ring.eof) {
        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
            quit = TRUE;
            break;
        }
        Delay(5);
    }
    if (g_fill_err[0]) {
        printf("Fueller: %s\n", g_fill_err);
        quit = TRUE;
    }
    if (!quit) {
        hms(start_ms, t1);
        printf("%s\n  ab %s, ID3 %ld B\n", e->title, t1, (long)g_fill_id3);
        fflush(stdout);
        audio_play(&g_ring, start_ms);
        last_save = ticks_now();

        for (;;) {
            Delay(25);
            if (audio_state() == AU_PLAYING) {
                e->pos_ms = audio_pos_ms();
            }
            if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                audio_halt();
                break;
            }
            if (audio_track_done()) {
                natural = TRUE;
                break;
            }
            if (audio_error()) {
                printf("nicht abspielbar: %s\n", audio_last_error());
                break;
            }
            /* Stand alle 30 s sichern - faellt der Amiga aus, fehlen
             * hoechstens diese 30 s. Nur eine kleine Datei. */
            if (ticks_now() - last_save >= 30 * 50 || ticks_now() < last_save) {
                last_save = ticks_now();
                pod_save_state(&g_pf);
            }
            if (ticks_now() - last_print >= 15 * 50 || ticks_now() < last_print) {
                last_print = ticks_now();
                hms(e->pos_ms, t1);
                hms(e->dur_s * 1000L, t2);
                printf("    %s / %s\n", t1, t2);
                fflush(stdout);
            }
        }
    }

    g_ring.stop = TRUE;
    while (!g_fill_done) {
        Delay(2);
    }
    audio_shutdown();
    ring_free(&g_ring);
    g_fopen = nav_open;
    g_ferrstr = sub_last_error;

    /* Zu Ende gelaufen heisst gehoert; Wunsch des Anwenders: danach
     * anhalten, nicht weiterschalten. */
    if (natural) {
        e->done = TRUE;
        e->pos_ms = 0;
    }
    pod_save_state(&g_pf);
    hms(e->pos_ms, t1);
    printf("gesichert: %s%s\n", e->done ? "gehoert" : t1, "");
    return SUB_OK;
}

/* pod search <text> | add <text> <nr> | list | eps <nr> |
 * refresh [nr] | play <nr> <folge> [sek] | remove <nr> */
static int cmd_pod(struct Prefs *p, int argc, char **argv)
{
    const char *sub = argc > 0 ? argv[0] : "";
    char t1[16], st[32];
    int rc = SUB_OK, i;

    if (stricmp(sub, "search") == 0 && argc >= 2) {
        struct SubList l;
        LONG t0 = ticks_now();

        list_init(&l, sizeof(struct PodShow));
        rc = pod_search(argv[1], &l);
        if (rc == SUB_OK) {
            for (i = 0; i < l.count; i++) {
                struct PodShow *p = (struct PodShow *)list_get(&l, i);

                printf("%2d %-40.40s %.30s\n", i + 1, p->title, p->author);
            }
            printf("%d Treffer in %ld.%02ld s\n", l.count,
                   (long)((ticks_now() - t0) / 50),
                   (long)(((ticks_now() - t0) % 50) * 2));
        }
        list_free(&l);
    } else if (stricmp(sub, "add") == 0 && argc >= 3) {
        struct SubList l;
        int nr = atoi(argv[2]);
        BOOL changed;
        LONG t0;

        list_init(&l, sizeof(struct PodShow));
        rc = pod_search(argv[1], &l);
        if (rc == SUB_OK && (nr < 1 || nr > l.count)) {
            printf("kein Treffer Nr. %d\n", nr);
            rc = SUB_EAPI;
        }
        if (rc == SUB_OK) {
            memset(&g_pf, 0, sizeof(g_pf));
            g_pf.show = *(struct PodShow *)list_get(&l, nr - 1);
            pod_key(g_pf.show.feed, g_pf.show.id);
            printf("abonniere %s\n  %s\n", g_pf.show.title, g_pf.show.feed);
            t0 = ticks_now();
            rc = pod_refresh(&g_pf, &changed);
            if (rc == SUB_OK) {
                printf("%d Folgen, Feed gelesen in %ld.%02ld s\n", g_pf.count,
                       (long)((ticks_now() - t0) / 50),
                       (long)(((ticks_now() - t0) % 50) * 2));
            }
        }
        list_free(&l);
    } else if (stricmp(sub, "list") == 0) {
        struct SubList l;

        list_init(&l, sizeof(struct PodShow));
        rc = pod_list(&l);
        for (i = 0; i < l.count; i++) {
            struct PodShow *p = (struct PodShow *)list_get(&l, i);

            printf("%2d %-40.40s %2d neu von %d  [%s]\n", i + 1, p->title,
                   p->nnew, p->count, p->id);
        }
        list_free(&l);
    } else if (stricmp(sub, "eps") == 0 && argc >= 2) {
        rc = pod_pick(argv[1]);
        if (rc == SUB_OK) {
            printf("%s - %s\n", g_pf.show.title, g_pf.show.author);
            for (i = 0; i < g_pf.count; i++) {
                struct PodEpisode *e = &g_pf.ep[i];

                duration_text((int)e->dur_s, t1, sizeof(t1));
                ep_state(e, st);
                printf("%2d %-11s %7s %-12s %.48s\n", i + 1, e->date, t1, st,
                       e->title);
            }
        }
    } else if (stricmp(sub, "refresh") == 0) {
        struct SubList l;
        BOOL changed;

        list_init(&l, sizeof(struct PodShow));
        pod_list(&l);
        for (i = 0; i < l.count; i++) {
            struct PodShow *p = (struct PodShow *)list_get(&l, i);
            LONG t0 = ticks_now();
            int before;

            if (argc >= 2 && atoi(argv[1]) != i + 1) {
                continue;
            }
            pod_load(p->id, &g_pf);
            before = g_pf.show.nnew;
            rc = pod_refresh(&g_pf, &changed);
            printf("%-40.40s %s, %ld.%02ld s, %d neu (vorher %d)\n",
                   p->title,
                   rc != SUB_OK ? pod_last_error()
                                : (changed ? "aktualisiert" : "unveraendert (304)"),
                   (long)((ticks_now() - t0) / 50),
                   (long)(((ticks_now() - t0) % 50) * 2),
                   g_pf.show.nnew, before);
        }
        list_free(&l);
        rc = SUB_OK;
    } else if (stricmp(sub, "play") == 0 && argc >= 3) {
        rc = pod_pick(argv[1]);
        if (rc == SUB_OK) {
            rc = pod_play(atoi(argv[2]),
                          argc >= 4 ? atol(argv[3]) * 1000L : -1L,
                          (LONG)p->ahiunit);
        }
    } else if (stricmp(sub, "remove") == 0 && argc >= 2) {
        rc = pod_pick(argv[1]);
        if (rc == SUB_OK) {
            pod_remove(g_pf.show.id);
            printf("entfernt: %s\n", g_pf.show.title);
        }
    } else {
        printf("pod search <text>              bei Apple suchen\n"
               "pod add <text> <nr>            Treffer nr abonnieren\n"
               "pod list                       Abos\n"
               "pod eps <nr>                   Folgen eines Abos\n"
               "pod refresh [nr]               Feeds neu holen\n"
               "pod play <nr> <folge> [sek]    abspielen, Stand sichern\n"
               "pod remove <nr>                Abo loeschen\n");
        return 5;
    }

    if (rc != SUB_OK) {
        printf("Fehler: %s\n", pod_last_error());
        return 10;
    }
    return 0;
}

static int run(int argc, char **argv)
{
    struct Prefs p;
    const char *cmd;
    int rc = SUB_OK;

    if (argc < 2) {
        usage();
        return 5;
    }
    cmd = argv[1];

    if (stricmp(cmd, "setup") == 0) {
        if (argc < 5) {
            printf("setup <host[:port]> <benutzer> <passwort>\n");
            printf("z.B.  setup navidrome.local:4533 user secret\n");
            return 5;
        }
        /* Erst laden, dann ueberschreiben: sonst wirft ein erneutes
         * setup den eingetragenen Cache-Pfad weg. Schlaegt das Laden
         * fehl, ist p trotzdem sauber - prefs_load raeumt es selbst. */
        prefs_load(&p);
        prefs_set_host(&p, argv[2]);
        strncpy(p.user, argv[3], sizeof(p.user) - 1);
        strncpy(p.pass, argv[4], sizeof(p.pass) - 1);

        rc = prefs_save(&p);
        if (rc == SUB_OK) {
            printf("gesichert: %s://%s:%d  Benutzer %s\n",
                   p.https ? "https" : "http", p.host, p.port, p.user);
            printf("Cover-Cache: %s\n", p.cache);
            printf("Verbindung wird geprueft ...\n");
            rc = sub_ping(&p);
            if (rc == SUB_OK) {
                printf("Anmeldung in Ordnung.\n");
            }
        }
        report(rc);
        return rc == SUB_OK ? 0 : 10;
    }

    if (stricmp(cmd, "?") == 0 || stricmp(cmd, "help") == 0) {
        usage();
        return 0;
    }

    /* Eine Datei dekodieren braucht keinen Serverzugang - so laeuft
     * die Messung auch ohne Prefs, z.B. unter vamos auf dem Mac. */
    if (stricmp(cmd, "aac") == 0 && argc > 2 &&
        !(argv[2][0] >= '0' && argv[2][0] <= '9')) {
        memset(&p, 0, sizeof(p));
        cmd_aac(&p, argv[2], (argc > 3) ? atol(argv[3]) : 256L,
                (argc > 4) ? argv[4] : NULL);
        return 0;
    }

    if (need_prefs(&p) != SUB_OK) {
        return 10;
    }

    /* Eigener Server, eigene Fehlertexte - deshalb vor allen anderen
     * Befehlen und mit eigenem Rueckgabewert. */
    if (stricmp(cmd, "abs") == 0) {
        return cmd_abs(&p, argc - 2, argv + 2);
    }
    if (stricmp(cmd, "pod") == 0) {
        return cmd_pod(&p, argc - 2, argv + 2);
    }

    if (stricmp(cmd, "ping") == 0) {
        /* ping [n]: n Anmeldungen hintereinander, je mit Zeit - zeigt,
         * was die TLS-Wiederaufnahme ab der zweiten Verbindung spart. */
        int i, n = (argc > 2) ? atoi(argv[2]) : 1;

        for (i = 0; i < n; i++) {
            LONG t0 = ticks_now(), t;

            rc = sub_ping(&p);
            t = ticks_now() - t0;
            if (rc != SUB_OK) {
                break;
            }
            printf("%s:%d - Anmeldung in Ordnung (%ld.%02ld s).\n",
                   p.host, p.port, (long)(t / 50), (long)((t % 50) * 2));
        }
    } else if (stricmp(cmd, "artists") == 0) {
        rc = cmd_artists(&p);
    } else if (stricmp(cmd, "albums") == 0) {
        const char *type = (argc > 2) ? argv[2] : "alphabeticalByName";
        int size = (argc > 3) ? atoi(argv[3]) : 20;
        rc = cmd_albums(&p, type, size);
    } else if (stricmp(cmd, "artist") == 0 && argc > 2) {
        rc = cmd_artist(&p, argv[2]);
    } else if (stricmp(cmd, "album") == 0 && argc > 2) {
        rc = cmd_album(&p, argv[2]);
    } else if (stricmp(cmd, "search") == 0 && argc > 2) {
        rc = cmd_search(&p, argv[2]);
    } else if (stricmp(cmd, "similar") == 0 && argc > 2) {
        rc = cmd_similar(&p, argv[2]);
    } else if (stricmp(cmd, "lyrics") == 0 && argc > 2) {
        char *text = malloc(16384);
        char *synced = malloc(16384);
        struct Song s;

        if (!text || !synced) {
            printf("zu wenig Speicher\n");
            free(text);
            free(synced);
            return 10;
        }

        /* Erst der eigene Server - was in der Datei steckt, passt
         * garantiert zur Aufnahme und kostet keine fremde Anfrage. */
        rc = sub_get_lyrics(&p, argv[2], text, 16384);
        if (rc == SUB_OK && text[0]) {
            printf("%s", text);
        } else {
            /* Sonst lrclib.net. Dafuer braucht es Interpret, Titel,
             * Album und Dauer - die stehen nicht in der Titel-ID, also
             * einmal getSong. */
            rc = sub_get_song(&p, argv[2], &s);
            if (rc == SUB_OK) {
                printf("suche bei lrclib.net: %s - %s (%d s)\n",
                       s.artist, s.title, s.duration);
                rc = sub_get_lyrics_net(s.artist, s.title, s.album,
                                        s.duration,
                                        text, 16384, synced, 16384);
                if (rc == SUB_OK) {
                    if (synced[0]) {
                        printf("%s", synced);
                        printf("\n(mit Zeitmarken)\n");
                    } else {
                        printf("%s", text);
                    }
                }
            }
        }
        free(text);
        free(synced);
    } else if (stricmp(cmd, "url") == 0 && argc > 2) {
        char url[1200];
        rc = sub_stream_url(&p, argv[2], url, sizeof(url));
        if (rc == SUB_OK) {
            printf("%s\n", url);
        }
    } else if (stricmp(cmd, "raw") == 0 && argc > 2) {
        /* Die rohe Antwort des Servers, gekuerzt. Kein Luxus, sondern
         * das Werkzeug fuer die Frage "welche Angaben schickt der
         * Server ueberhaupt mit?" - die liess sich sonst nur raten. */
        char *body = NULL;
        long  len  = 0;

        rc = sub_api_raw(&p, argv[2], (argc > 3) ? argv[3] : "",
                         &body, &len);
        if (rc == SUB_OK && body) {
            long n = (argc > 4) ? atol(argv[4]) : 1200;

            if (n > len) {
                n = len;
            }
            body[n] = '\0';
            printf("%ld Bytes, davon die ersten %ld:\n%s\n", len, n, body);
        }
        if (body) {
            free(body);
        }
    } else if (stricmp(cmd, "mp3") == 0 && argc > 2) {
        /* Stufe 0 des eigenen Abspielers: wie schnell dekodiert
         * mpega.library auf DIESER Maschine? Ohne Ausgabe, damit die
         * Zahl am Dekoder haengt und nicht an der Soundkarte. */
        struct AudioProbe pr;

        if (!audio_probe_file(argv[2], &pr)) {
            printf("kein Dekodieren von %s: %s\n", argv[2],
                   audio_last_error());
            return 10;
        }
        printf("Layer %ld, %ld kbps, %ld Hz, %ld Kanaele\n",
               (long)pr.layer, (long)pr.bitrate, (long)pr.freq,
               (long)pr.channels);
        printf("%lu Bloecke, %lu Abtastwerte je Kanal\n",
               pr.frames, pr.samples);
        if (pr.freq > 0) {
            ULONG ms_play = (ULONG)((pr.samples * 1000UL) / (ULONG)pr.freq);

            printf("Spieldauer   %lu.%02lu s\n",
                   ms_play / 1000UL, (ms_play % 1000UL) / 10UL);
            printf("Dekodierzeit %lu.%02lu s\n",
                   pr.cs_decode / 100UL, pr.cs_decode % 100UL);
            if (pr.cs_decode > 0) {
                printf("Faktor       %lu.%02lu-fache Echtzeit\n",
                       (ms_play / 10UL) / pr.cs_decode,
                       ((ms_play / 10UL) * 100UL / pr.cs_decode) % 100UL);
            } else {
                printf("Faktor       zu schnell zum Messen\n");
            }
        }
        rc = SUB_OK;
    } else if (stricmp(cmd, "dl") == 0 && argc > 2) {
        /* Messwerkzeug: wie schnell kommt ein Strom an, und was kostet
         * das Holen die CPU? dl <titel-id|http-url> [kbytes]
         * Die CPU-Last misst ein Zaehler auf niedrigster Prioritaet: er
         * bekommt nur, was sonst niemand will. Erst 3 s ohne Download
         * (Grundwert), dann mit. */
        struct SubStream st;
        static UBYTE dlbuf[16384];
        long want = (argc > 3) ? atol(argv[3]) * 1024L : 2048L * 1024L;
        long got = 0, n;
        LONG t0, t1, b0, b1;
        ULONG c0, c1, base;

        g_spin_run = TRUE;
        g_spin_count = 0;
        g_spin_parent = FindTask(NULL);
        SetSignal(0, SIGF_SINGLE);
        g_spin_task = (struct Task *)CreateNewProcTags(
                               NP_Entry, (ULONG)spinner,
                               NP_Name, (ULONG)"AmiSubsonic.spin",
                               NP_Priority, -127, TAG_DONE);
        if (!g_spin_task) {
            printf("Zaehler laesst sich nicht starten\n");
            return 10;
        }
        Delay(25);
        b0 = ticks_now();
        c0 = g_spin_count;
        Delay(150);
        b1 = ticks_now();
        c1 = g_spin_count;
        base = (c1 - c0) / (ULONG)((b1 - b0) > 0 ? (b1 - b0) : 1);

        if (strnicmp(argv[2], "http", 4) == 0) {
            rc = sub_url_open(argv[2], NULL, 0, &st);
        } else {
            rc = sub_stream_open(&p, argv[2], 0, &st);
        }
        if (rc != SUB_OK) {
            spin_stop();
            printf("%s\n", sub_last_error());
            return 10;
        }
        t0 = ticks_now();
        c0 = g_spin_count;
        while (got < want) {
            n = sub_stream_read(&st, dlbuf, sizeof(dlbuf));
            if (n <= 0) {
                break;
            }
            got += n;
        }
        t1 = ticks_now();
        c1 = g_spin_count;
        sub_stream_close(&st);
        spin_stop();
        {
            LONG dt = (t1 - t0) > 0 ? (t1 - t0) : 1;
            ULONG rate = (c1 - c0) / (ULONG)dt;
            LONG busy = base ? (LONG)(100 - (rate * 100UL) / base) : -1;

            printf("%ld KB in %ld.%02ld s = %ld KB/s, CPU fuers Holen %ld %%\n",
                   got / 1024, (long)(dt / 50), (long)((dt % 50) * 2),
                   (long)((got / 1024) * 50 / dt), (long)busy);
        }
        rc = SUB_OK;
    } else if (stricmp(cmd, "stream") == 0 && argc > 2) {
        /* Stufe 2: Titel aus dem Netz spielen, ohne Datei dazwischen.
         * Mehrere Kennungen hintereinander sind erlaubt - damit laesst
         * sich der Dauerlauf messen UND der Uebergang von Titel zu
         * Titel, der in der Oberflaeche spaeter die Warteschlange ist. */
        extern LONG g_hk_waits, g_hk_wait_ticks, g_t_mpega, g_t_ahi;
        LONG unit = 0;
        int k;

        /* 1 MB statt 256 KB: bei 200 kbps sind das gut 40 s Vorrat
         * statt 10. Im Dauerlauf vom 20.9.2026 lief der Ring waehrend
         * eines Netzabrisses leer (7,42 s Warten) - mit 1 MB waere
         * nichts zu hoeren gewesen. Auf dieser Maschine (340 MB Fast)
         * kostet das nichts. */
        if (!ring_init(&g_ring, 1024UL * 1024UL)) {
            printf("zu wenig Speicher fuer den Ringpuffer\n");
            return 10;
        }
        g_fprefs = p;

        for (k = 2; k < argc; k++) {
            struct AudioProbe pr;
            struct Process *proc;
            LONG t_start;

            ring_reset(&g_ring);
            g_t_conn = 0;
            g_t_id3 = 0;
            g_fill_done = 0;
            g_fill_bytes = 0;
            g_fill_reconnects = 0;
            g_fill_id3 = 0;
            g_fill_waited = 0;
            g_fill_err[0] = '\0';
            strncpy(g_fid, argv[k], sizeof(g_fid) - 1);

            proc = CreateNewProcTags(NP_Entry,     (ULONG)filler,
                                     NP_Name,      (ULONG)"AmiSubsonic.fill",
                                     NP_StackSize, 65536,
                                     NP_Priority,  0,
                                     TAG_DONE);
            if (!proc) {
                printf("Fuellprozess laesst sich nicht starten\n");
                break;
            }

            /* Erst einen Vorrat sammeln, dann anfangen: sonst hoert man
             * die ersten Sekunden stockend, waehrend TLS-Handschlag und
             * erste Bloecke noch laufen.
             *
             * Strg-C MUSS hier greifen. Ohne die Abfrage haengt das
             * Programm unbeendbar fest, wenn das Netz weg ist - genau
             * das ist am 20.9.2026 passiert: Break zeigte keine Wirkung,
             * und es blieb nur, den Prozess stehen zu lassen. */
            t_start = ticks_now();
            while (ring_used(&g_ring) < 64UL * 1024UL && !g_ring.eof) {
                if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                    break;
                }
                Delay(5);
            }
            if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                printf("abgebrochen\n");
                g_ring.stop = TRUE;
                while (!g_fill_done) {
                    Delay(2);
                }
                break;
            }

            printf("%d/%d %s ... Vorlauf %ld.%02ld s "
                   "(Verbindung %ld.%02ld, ID3 %ld.%02ld) ",
                   k - 1, argc - 2, argv[k],
                   (long)((ticks_now() - t_start) / 50),
                   (long)(((ticks_now() - t_start) % 50) * 2),
                   (long)(g_t_conn / 50), (long)((g_t_conn % 50) * 2),
                   (long)(g_t_id3 / 50), (long)((g_t_id3 % 50) * 2));
            fflush(stdout);

            if (!audio_play_ring(&g_ring, unit, &pr)) {
                printf("kein Abspielen: %s\n", audio_last_error());
            } else {
                printf("%ld kbps, %lu.%02lu s, Warten %ld mal "
                       "(%ld.%02ld s), %ld Neuanlaeufe, ID3 %ld B\n",
                       (long)pr.bitrate,
                       pr.cs_decode / 100UL, pr.cs_decode % 100UL,
                       (long)g_hk_waits,
                       (long)(g_hk_wait_ticks / 50),
                       (long)((g_hk_wait_ticks % 50) * 2),
                       (long)g_fill_reconnects, (long)g_fill_id3);
                printf("    Vorlauf Dekoder: MPEGA_open %ld.%02ld s, "
                       "ahi.device %ld.%02ld s\n",
                       (long)(g_t_mpega / 100), (long)(g_t_mpega % 100),
                       (long)(g_t_ahi / 100), (long)(g_t_ahi % 100));
                if (g_fill_waited > 0) {
                    printf("    Netz war %ld s weg\n", (long)g_fill_waited);
                }
            }

            g_ring.stop = TRUE;
            while (!g_fill_done) {
                Delay(2);
            }
            if (g_fill_err[0]) {
                printf("    Fueller: %s\n", g_fill_err);
            }
            if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                printf("abgebrochen\n");
                break;
            }
        }

        ring_free(&g_ring);
        rc = SUB_OK;
    } else if (stricmp(cmd, "aac") == 0 && argc > 2) {
        rc = cmd_aac(&p, argv[2], (argc > 3) ? atol(argv[3]) : 256L,
                     (argc > 4) ? argv[4] : NULL);
    } else if (stricmp(cmd, "radio") == 0 && argc == 2) {
        /* Liste. Das Netz fasst hier der Hauptprozess an - es startet
         * danach kein Fueller mehr, also gibt es keinen zweiten
         * AmiSSL-Besitzer. */
        struct SubList l;
        int i;

        list_init(&l, sizeof(struct Radio));
        rc = sub_get_radios(&p, &l);
        if (rc == SUB_OK) {
            for (i = 0; i < l.count; i++) {
                struct Radio *r = (struct Radio *)list_get(&l, i);

                printf("%2d %-4s %-24s %s\n", i + 1,
                       sub_radio_url_aac(r->url) ? "AAC" : "", r->name,
                       r->url);
            }
            printf("%d Sender\n", l.count);
        }
        list_free(&l);
    } else if (stricmp(cmd, "radio") == 0 && argc > 2) {
        /* Einen Sender spielen, bis Strg-C. */
        struct AudioProbe pr;
        struct Process *proc;
        LONG t_start;

        if (!ring_init(&g_ring, 1024UL * 1024UL)) {
            printf("zu wenig Speicher fuer den Ringpuffer\n");
            return 10;
        }
        g_fprefs = p;
        g_fradio = atoi(argv[2]);
        ring_reset(&g_ring);
        g_fill_done = 0;
        g_fill_bytes = 0;
        g_fill_reconnects = 0;
        g_fill_waited = 0;
        g_fill_err[0] = '\0';

        proc = CreateNewProcTags(NP_Entry,     (ULONG)radio_filler,
                                 NP_Name,      (ULONG)"AmiSubsonic.fill",
                                 NP_StackSize, 65536,
                                 NP_Priority,  0,
                                 TAG_DONE);
        if (!proc) {
            printf("Fuellprozess laesst sich nicht starten\n");
            ring_free(&g_ring);
            return 10;
        }

        t_start = ticks_now();
        while (ring_used(&g_ring) < 64UL * 1024UL && !g_ring.eof) {
            if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
                break;
            }
            Delay(5);
        }
        if (g_fr_ok) {
            printf("%s: %s, %ld kbps, %ld Weiterleitung(en), "
                   "Vorlauf %ld.%02ld s\n",
                   g_fr_name, g_fr_ctype[0] ? g_fr_ctype : "(kein Typ)",
                   (long)g_fr_br, (long)g_fr_hops,
                   (long)((ticks_now() - t_start) / 50),
                   (long)(((ticks_now() - t_start) % 50) * 2));
        }
        if (!g_ring.eof && !(SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)) {
            printf("spielt, Strg-C beendet\n");
            if (!audio_play_ring(&g_ring, 0, &pr)) {
                printf("kein Abspielen: %s\n", audio_last_error());
            } else {
                printf("%ld kbps, %ld Hz, %lu.%02lu s gehoert, "
                       "%ld Neuverbindungen\n",
                       (long)pr.bitrate, (long)pr.freq,
                       pr.cs_decode / 100UL, pr.cs_decode % 100UL,
                       (long)g_fill_reconnects);
            }
        }
        g_ring.stop = TRUE;
        while (!g_fill_done) {
            Delay(2);
        }
        if (g_fill_err[0]) {
            printf("Fueller: %s\n", g_fill_err);
        }
        ring_free(&g_ring);
        rc = SUB_OK;
    } else if (stricmp(cmd, "ptest") == 0 && argc > 2) {
        /* Stufe 3: der Audioprozess, mit Pause und Positionsanzeige -
         * beides ging mit AmigaAMP am Netzstrom nicht. Ablauf:
         * 8 s spielen, 4 s Pause, 8 s weiter, dann abbrechen. */
        struct Process *proc;
        LONG unit = (argc > 3) ? atol(argv[3]) : 0;
        int sek;

        if (!ring_init(&g_ring, 1024UL * 1024UL)) {
            printf("zu wenig Speicher fuer den Ringpuffer\n");
            return 10;
        }
        if (!audio_start(unit)) {
            printf("%s\n", audio_last_error());
            ring_free(&g_ring);
            return 10;
        }

        g_fprefs = p;
        ring_reset(&g_ring);
        g_fill_done = 0;
        g_fill_err[0] = '\0';
        strncpy(g_fid, argv[2], sizeof(g_fid) - 1);

        proc = CreateNewProcTags(NP_Entry,     (ULONG)filler,
                                 NP_Name,      (ULONG)"AmiSubsonic.fill",
                                 NP_StackSize, 65536,
                                 NP_Priority,  0,
                                 TAG_DONE);
        if (!proc) {
            printf("Fuellprozess laesst sich nicht starten\n");
            audio_shutdown();
            ring_free(&g_ring);
            return 10;
        }
        while (ring_used(&g_ring) < 64UL * 1024UL && !g_ring.eof) {
            Delay(5);
        }

        audio_set_volume(64);
        audio_play(&g_ring, 0);

        for (sek = 0; sek < 20; sek++) {
            const char *z = "?";

            switch (audio_state()) {
            case AU_STOPPED: z = "steht"; break;
            case AU_PLAYING: z = "spielt"; break;
            case AU_PAUSED:  z = "Pause"; break;
            }
            printf("%2d s  %s  Position %ld.%02ld s  Ring %ld KB\n",
                   sek, z,
                   (long)(audio_pos_ms() / 1000),
                   (long)((audio_pos_ms() % 1000) / 10),
                   (long)(ring_used(&g_ring) / 1024));

            if (sek == 8)  { printf("--> PAUSE\n");  audio_pause(TRUE);  }
            if (sek == 12) { printf("--> WEITER\n"); audio_pause(FALSE); }
            if (audio_track_done()) {
                printf("Titel zu Ende\n");
                break;
            }
            Delay(50);
        }

        printf("--> STOP\n");
        audio_halt();
        Delay(25);
        audio_shutdown();
        g_ring.stop = TRUE;
        while (!g_fill_done) {
            Delay(2);
        }
        ring_free(&g_ring);
        rc = SUB_OK;
    } else if (stricmp(cmd, "spiel") == 0 && argc > 2) {
        /* Stufe 1: lokale Datei ueber AHI, noch ohne Netz. */
        struct AudioProbe pr;
        LONG unit = (argc > 3) ? atol(argv[3]) : 0;

        printf("spiele %s ueber ahi.device Unit %ld, Strg-C bricht ab\n",
               argv[2], (long)unit);
        if (!audio_play_file(argv[2], unit, &pr)) {
            printf("kein Abspielen: %s\n", audio_last_error());
            return 10;
        }
        printf("Layer %ld, %ld kbps, %ld Hz, %ld Kanaele\n",
               (long)pr.layer, (long)pr.bitrate, (long)pr.freq,
               (long)pr.channels);
        printf("%lu Bloecke gespielt, %lu.%02lu s\n", pr.frames,
               pr.cs_decode / 100UL, pr.cs_decode % 100UL);
        rc = SUB_OK;
    } else if (stricmp(cmd, "range") == 0 && argc > 2) {
        /* Stufe 0: laesst der Server mitten im Titel einsteigen?
         *   range <titel-id> [offset] [format]
         * Antwortet er mit 206 und Content-Range, gehen Spulen und das
         * Wiederaufsetzen nach einem Netzaussetzer. */
        char head[1200];
        long off = (argc > 3) ? atol(argv[3]) : 100000L;
        const char *fmt = (argc > 4) ? argv[4] : "";

        rc = sub_range_probe(&p, argv[2], off, fmt,
                             (argc > 5) ? atoi(argv[5]) : 320,
                             head, sizeof(head));
        if (rc != SUB_OK) {
            printf("%s\n", sub_last_error());
            return 10;
        }
        printf("Anfrage ab Byte %ld%s%s\n", off,
               fmt[0] ? ", format=" : "", fmt);
        printf("%s\n", head);
    } else if (stricmp(cmd, "farbe") == 0 && argc > 2) {
        ULONG rgb = 0;

        if (!cover_dominant(argv[2], &rgb)) {
            printf("keine Hauptfarbe aus %s: %s\n",
                   argv[2], cover_last_error());
            return 10;
        }
        printf("Hauptfarbe   0x%06lx   R %ld  G %ld  B %ld\n",
               (long)rgb, (long)((rgb >> 16) & 0xff),
               (long)((rgb >> 8) & 0xff), (long)(rgb & 0xff));
        printf("Verlaufskopf 0x%06lx  (auf Helligkeit %d normiert)\n",
               (long)cover_gradient_top(rgb), GRAD_TOP_LUM);
        /* Auch bei Erfolg kann cover.c einen Hinweis hinterlassen haben,
         * etwa dass es den Ausweg ueber die BitMap nehmen musste. */
        if (strcmp(cover_last_error(), "kein Fehler") != 0) {
            printf("Hinweis      %s\n", cover_last_error());
        }
        rc = SUB_OK;
    } else if (stricmp(cmd, "cover") == 0 && argc > 2) {
        const char *dest = (argc > 3) ? argv[3] : "T:cover";
        rc = sub_get_cover(&p, argv[2], 300, dest);
        if (rc == SUB_OK) {
            printf("Cover liegt in %s\n", dest);
        }
    } else if (stricmp(cmd, "cache") == 0) {
        if (argc > 2) {
            char path[256];

            rc = sub_get_cover_cached(&p, argv[2], 300, path, sizeof(path));
            if (rc == SUB_OK) {
                printf("%s\n", path);
            }
        } else {
            printf("Cover-Cache: %s\n", p.cache);
            rc = cache_ensure(&p);
            if (rc == SUB_OK) {
                printf("Verzeichnis ist da.\n");
            }
        }
    } else {
        usage();
        return 5;
    }

    report(rc);
    return rc == SUB_OK ? 0 : 10;
}


int main(int argc, char **argv)
{
    int rc = run(argc, argv);

    sub_cleanup();
    return rc;
}
