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

static void filler(void)
{
    struct SubStream st;
    LONG t0 = ticks_now();
    UBYTE *chunk;
    long off = 0;
    long waited = 0;            /* Ticks, die schon vergeblich vergingen */

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
        if (sub_stream_open(&g_fprefs, g_fid, off, &st) != SUB_OK) {
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
        if (off == 0) {
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
                while (skip > 0) {
                    long want = (skip > FILL_CHUNK) ? FILL_CHUNK : skip;
                    long n = sub_stream_read(&st, chunk, want);

                    if (n <= 0) {
                        break;
                    }
                    skip -= n;
                }
            } else if (got > 0) {
                ring_write(&g_ring, hdr, (ULONG)got);
            }
            off = st.pos;
            g_fill_bytes = off;
            g_t_id3 = ticks_now() - t0 - g_t_conn;
        }

        for (;;) {
            if (g_ring.stop) {
                goto done;
            }
            /* Ist der Ring voll, wird gewartet statt gelesen - sonst
             * liefe der Titel schneller herein, als er gehoert wird,
             * und der Speicher waere umsonst gross. */
            if (ring_space(&g_ring) < FILL_CHUNK) {
                Delay(2);
                continue;
            }
            n = sub_stream_read(&st, chunk, FILL_CHUNK);
            if (n > 0) {
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

    if (stricmp(cmd, "ping") == 0) {
        rc = sub_ping(&p);
        if (rc == SUB_OK) {
            printf("%s:%d - Anmeldung in Ordnung.\n", p.host, p.port);
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
