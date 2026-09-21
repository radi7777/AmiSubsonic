/* AmiSubsonic - Netzarbeit in einem eigenen Prozess. */

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>

#include "netjob.h"
#include "ring.h"
#include "local.h"
#include "amisub.h"

/* Beide Prozesse sind derselbe Programmabzug und teilen sich damit auch
 * die globalen Daten - das ist der einfachste Weg, dem frisch
 * gestarteten Arbeitsprozess mitzuteilen, wo er sich melden soll. */
static struct MsgPort *g_reply    = NULL;   /* gehoert der Oberflaeche */
static struct MsgPort *g_workport = NULL;   /* gehoert dem Arbeiter */
static struct Task    *g_starter  = NULL;
static struct Prefs   *g_prefs    = NULL;
static BOOL            g_busy     = FALSE;
static BOOL            g_running  = FALSE;

/* ------------------------------------------------------------------ */
/* Der Strom fuer die Wiedergabe                                       */
/* ------------------------------------------------------------------ */

/* Warum hier und nicht in einem eigenen Prozess: AmiSSL gehoert genau
 * einem Prozess. Die Basiszeiger liegen global in amisub.c, zwei
 * Prozesse koennten also nicht jeder seine eigene Verbindung
 * unterhalten. Der Strom wird deshalb in Scheiben geholt, und zwischen
 * zwei Scheiben sieht der Arbeiter nach, ob ein Auftrag anliegt.
 *
 * Bei 1 MB Ring und 200 kbps sind das gut 40 s Vorrat - ein Coverabruf
 * dauert rund 1 s. Die Wiedergabe merkt davon nichts. */
#define ST_CHUNK       8192
#define ST_PATIENCE    (180 * 50)   /* 180 s Geduld, in Ticks */
#define ST_PAUSE       100          /* 2 s zwischen zwei Anlaeufen */

static struct SubStream g_st;
static struct Ring *g_st_ring = NULL;
static char  g_st_id[SUB_ID_LEN];
static BOOL  g_st_on = FALSE;       /* Strom laeuft */
static BOOL  g_st_open = FALSE;     /* Verbindung steht */
static long  g_st_off = 0;          /* naechste Stelle in der Datei */
static long  g_st_waited = 0;       /* Ticks ohne Verbindung */
static LONG  g_st_waited_s = 0;
static UBYTE g_st_buf[ST_CHUNK];
static char  g_st_msg[96];          /* was zuletzt geschah */

/* Zweite Quelle: eine Datei auf der Platte. Der Abspieler merkt davon
 * nichts - er liest aus demselben Ring. Nur gefuellt wird er hier
 * entweder aus einer TLS-Verbindung oder aus einer Datei. */
static BPTR  g_st_fh = 0;
static char  g_st_file[192];

/* Dritte Quelle: ein Radiosender. Kein Range, keine Laenge, kein Ende -
 * reisst die Verbindung, wird neu verbunden und live weitergehoert. */
static char  g_st_url[512];
static BOOL  g_st_notmp3 = FALSE;
static char  g_st_ctype[40];

/* ICY: der laufende Titel eines Senders. Mit "Icy-MetaData: 1" steckt
 * nach je metaint Musikbytes ein Block im Strom: ein Laengenbyte (mal
 * 16), dann Text wie "StreamTitle='Interpret - Titel';". Gemessen am
 * 21.9.2026 an vier Sendern: metaint 8192, fast jeder Block hat Laenge
 * 0, der erste traegt den Sendernamen, danach kommt je Titelwechsel
 * einer.
 *
 * Der Block MUSS aus dem Strom heraus, bevor etwas in den Ring geht -
 * mpega.library haelt ihn sonst fuer kaputte Rahmen, und man hoert es.
 * Er kann ueber zwei Lesevorgaenge verteilt ankommen, deshalb ein
 * Zustandsautomat statt einer Rechnung je Scheibe. */
#define ICY_MUSIC 0
#define ICY_LEN   1
#define ICY_META  2
static int   g_icy_state = ICY_MUSIC;
static long  g_icy_left  = 0;       /* Musikbytes bis zum naechsten Block */
static long  g_icy_metaint = 0;     /* 0 = der Sender schickt keine */
static int   g_icy_len = 0, g_icy_got = 0;
static char  g_icy_buf[16 * 255 + 1];

/* Fuer die Oberflaeche. Geschrieben unter Forbid(), weil sie aus einem
 * anderen Prozess liest; g_icy_seq zaehlt jede neue Meldung. */
static char  g_icy_title[160];
static volatile ULONG g_icy_seq = 0;

/* Ein MP3 vom Server faengt mit einem ID3v2-Etikett an, in dem das
 * Albumcover steckt - hier 38 bis 50 KB ohne ein einziges MPEG-
 * Synchronwort. mpega.library sucht den Anfang nur in den ersten
 * Kilobytes und gibt dann auf (gemessen: sie las 48 KB und meldete
 * "kein MPEG"). Also wird das Etikett uebersprungen. */
static void stream_skip_id3(void)
{
    UBYTE hdr[10];
    long got = 0;

    while (got < 10) {
        long n = sub_stream_read(&g_st, hdr + got, 10 - got);

        if (n <= 0) {
            return;
        }
        got += n;
    }
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        long skip = ((long)(hdr[6] & 0x7f) << 21)
                  | ((long)(hdr[7] & 0x7f) << 14)
                  | ((long)(hdr[8] & 0x7f) <<  7)
                  |  (long)(hdr[9] & 0x7f);

        while (skip > 0) {
            long want = (skip > ST_CHUNK) ? ST_CHUNK : skip;
            long n = sub_stream_read(&g_st, g_st_buf, want);

            if (n <= 0) {
                break;
            }
            skip -= n;
        }
    } else if (got > 0) {
        ring_write(g_st_ring, hdr, (ULONG)got);
    }
    g_st_off = g_st.pos;
}

static void stream_close(void)
{
    if (g_st_fh) {
        Close(g_st_fh);
        g_st_fh = 0;
    }
    if (g_st_open) {
        sub_stream_close(&g_st);
        g_st_open = FALSE;
    }
}

/* Das ID3v2-Etikett einer eigenen Datei ueberspringen - derselbe Grund
 * wie beim Server: mpega.library findet sonst keinen MPEG-Anfang. Bei
 * einer Datei ist es billiger, denn hier wird nur gesprungen statt
 * gelesen. */
static void file_skip_id3(void)
{
    UBYTE hdr[10];

    if (Read(g_st_fh, hdr, 10) != 10) {
        return;
    }
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        long skip = ((long)(hdr[6] & 0x7f) << 21)
                  | ((long)(hdr[7] & 0x7f) << 14)
                  | ((long)(hdr[8] & 0x7f) <<  7)
                  |  (long)(hdr[9] & 0x7f);

        Seek(g_st_fh, skip, OFFSET_CURRENT);
        g_st_off = 10 + skip;
    } else {
        Seek(g_st_fh, 0, OFFSET_BEGINNING);
        g_st_off = 0;
    }
}

static void stream_begin(struct NetJob *j)
{
    stream_close();
    g_st_file[0] = '\0';
    if (j->path[0]) {
        strncpy(g_st_file, j->path, sizeof(g_st_file) - 1);
        g_st_file[sizeof(g_st_file) - 1] = '\0';
    }
    strncpy(g_st_url, j->url, sizeof(g_st_url) - 1);
    g_st_url[sizeof(g_st_url) - 1] = '\0';
    g_st_notmp3 = FALSE;
    g_st_ctype[0] = '\0';
    g_icy_metaint = 0;
    Forbid();
    g_icy_title[0] = '\0';
    g_icy_seq++;
    Permit();
    g_st_ring = j->out_ring;
    g_st_off  = j->offset;          /* Byte, nicht Titelnummer */
    g_st_on   = (g_st_ring != NULL);
    g_st_waited = 0;
    g_st_waited_s = 0;
    strncpy(g_st_id, j->id, sizeof(g_st_id) - 1);
    g_st_id[sizeof(g_st_id) - 1] = '\0';
}

/* UTF-8 nach Latin-1, an Ort und Stelle. Nur wenn der Text GUELTIGES
 * UTF-8 ist - aeltere Shoutcast-Server schicken Latin-1, und das darf
 * nicht verstuemmelt werden. Zeichen ausserhalb von Latin-1 werden zu
 * '?', der Amiga-Font hat sie ohnehin nicht. */
static void icy_utf8_to_latin1(char *s)
{
    UBYTE *p = (UBYTE *)s, *q;
    BOOL multi = FALSE;

    /* Erst pruefen. */
    while (*p) {
        int n = 0, k;

        if (*p < 0x80)                { p++; continue; }
        else if ((*p & 0xE0) == 0xC0) { n = 1; }
        else if ((*p & 0xF0) == 0xE0) { n = 2; }
        else if ((*p & 0xF8) == 0xF0) { n = 3; }
        else { return; }                /* kein UTF-8 - so lassen */
        for (k = 1; k <= n; k++) {
            if ((p[k] & 0xC0) != 0x80) {
                return;
            }
        }
        multi = TRUE;
        p += n + 1;
    }
    if (!multi) {
        return;
    }

    /* Dann umsetzen. */
    p = q = (UBYTE *)s;
    while (*p) {
        if (*p < 0x80) {
            *q++ = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            UBYTE c = (UBYTE)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
            *q++ = (p[0] <= 0xC3) ? c : '?';
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            *q++ = '?';
            p += 3;
        } else {
            *q++ = '?';
            p += 4;
        }
    }
    *q = '\0';
}

/* Einen vollstaendigen Block auswerten: StreamTitle='...' heraus-
 * schneiden. Das Ende ist "';" - ein einzelnes ' kann im Titel selbst
 * stehen ("Guns N' Roses"). */
static void icy_parse(void)
{
    char *a, *e;
    char t[sizeof(g_icy_title)];
    int n;

    g_icy_buf[g_icy_len] = '\0';
    a = strstr(g_icy_buf, "StreamTitle='");
    if (!a) {
        return;
    }
    a += 13;
    e = strstr(a, "';");
    if (!e) {
        e = strrchr(a, '\'');
    }
    n = e ? (int)(e - a) : (int)strlen(a);
    if (n >= (int)sizeof(t)) {
        n = sizeof(t) - 1;
    }
    memcpy(t, a, n);
    t[n] = '\0';
    icy_utf8_to_latin1(t);

    Forbid();
    strcpy(g_icy_title, t);
    g_icy_seq++;
    Permit();
}

/* Musik in den Ring, wartet bei vollem Ring (dasselbe wie in den
 * anderen Zweigen). */
static void ring_put_all(const UBYTE *b, long n)
{
    ULONG put = 0;

    while (put < (ULONG)n && !g_st_ring->stop) {
        ULONG w = ring_write(g_st_ring, b + put, (ULONG)n - put);

        if (w == 0) {
            Delay(1);
            continue;
        }
        put += w;
    }
}

/* Eine gelesene Scheibe aufteilen: Musik in den Ring, Metadaten in den
 * Puffer. */
static void icy_feed(const UBYTE *b, long n)
{
    long i = 0;

    if (g_icy_metaint <= 0) {
        ring_put_all(b, n);
        return;
    }
    while (i < n) {
        if (g_icy_state == ICY_MUSIC) {
            long take = n - i;

            if (take > g_icy_left) {
                take = g_icy_left;
            }
            ring_put_all(b + i, take);
            i += take;
            g_icy_left -= take;
            if (g_icy_left == 0) {
                g_icy_state = ICY_LEN;
            }
        } else if (g_icy_state == ICY_LEN) {
            g_icy_len = b[i++] * 16;
            g_icy_got = 0;
            if (g_icy_len == 0) {
                g_icy_state = ICY_MUSIC;
                g_icy_left = g_icy_metaint;
            } else {
                g_icy_state = ICY_META;
            }
        } else {
            long take = n - i;

            if (take > g_icy_len - g_icy_got) {
                take = g_icy_len - g_icy_got;
            }
            memcpy(g_icy_buf + g_icy_got, b + i, take);
            g_icy_got += take;
            i += take;
            if (g_icy_got == g_icy_len) {
                icy_parse();
                g_icy_state = ICY_MUSIC;
                g_icy_left = g_icy_metaint;
            }
        }
    }
}

/* Eine Scheibe vom Radiosender. Aufgebaut wie der Serverweg in
 * stream_slice(), mit drei Unterschieden:
 *
 *   - kein Offset: nach einem Abriss geht es live weiter, was
 *     dazwischen gesendet wurde, ist verloren;
 *   - kein Ende: auch ein sauber geschlossener Strom heisst "neu
 *     verbinden", denn ein Sender hoert nicht auf;
 *   - der Content-Type wird geprueft. Liefert der Sender AAC, ist
 *     sofort Schluss - mpega.library kann das nicht, und ohne die
 *     Pruefung liefe der Ring mit Daten voll, die niemand dekodiert. */
static BOOL radio_slice(void)
{
    long n;

    if (!g_st_open) {
        if (sub_radio_open(g_st_url, TRUE, &g_st) != SUB_OK) {
            sprintf(g_st_msg, "Sender: %.60s", sub_last_error());
            if (g_st_waited >= ST_PATIENCE) {
                g_st_ring->eof = TRUE;
                g_st_on = FALSE;
                return FALSE;
            }
            Delay(ST_PAUSE);
            g_st_waited += ST_PAUSE;
            g_st_waited_s = g_st_waited / 50;
            return TRUE;
        }
        g_st_open = TRUE;
        g_st_waited = 0;
        g_st_waited_s = 0;
        /* Jede Verbindung faengt mit Musik an - auch eine neue nach
         * einem Abriss, mitten in einem alten Block war man dann ja
         * nicht mehr. */
        g_icy_metaint = g_st.metaint;
        g_icy_left    = g_st.metaint;
        g_icy_state   = ICY_MUSIC;
        strncpy(g_st_ctype, g_st.ctype, sizeof(g_st_ctype) - 1);
        g_st_ctype[sizeof(g_st_ctype) - 1] = '\0';
        sprintf(g_st_msg, "Sender offen, %.30s, %d Weiterleitung(en)",
                g_st.ctype, g_st.hops);

        if (!sub_radio_is_mp3(g_st.ctype)) {
            /* Erst die Markierung, DANN das Ende: die Oberflaeche sieht
             * das Ende und fragt gleich danach, warum. */
            g_st_notmp3 = TRUE;
            stream_close();
            g_st_ring->eof = TRUE;
            g_st_on = FALSE;
            return FALSE;
        }
    }

    if (ring_space(g_st_ring) < ST_CHUNK) {
        return FALSE;                   /* Ring voll - schlafen legen */
    }

    n = sub_stream_read(&g_st, g_st_buf, ST_CHUNK);
    if (n > 0) {
        icy_feed(g_st_buf, n);
        g_st_off += n;
        return TRUE;
    }

    /* Weg - gleich welcher Grund. Neu verbinden; scheitert das, greift
     * oben die Geduld in Sekunden. */
    sprintf(g_st_msg, "Sender abgerissen nach %ld Bytes", g_st_off);
    stream_close();
    return TRUE;
}

/* Eine Scheibe holen. Kehrt schnell zurueck, damit der Arbeiter
 * zwischendurch Auftraege annehmen kann.
 *
 * Rueckgabe: TRUE, wenn wirklich etwas getan wurde. FALSE heisst "nichts
 * zu tun" - dann MUSS der Aufrufer schlafen. Ohne das dreht dieser
 * Prozess bei vollem Ring in einer Schleife auf voller Drehzahl, und
 * weil er auf Priotitaet 0 laeuft, bleibt fuer die Workbench nichts
 * uebrig: ein Kopiervorgang fing waehrend der Wiedergabe gar nicht erst
 * an (gemessen vom Anwender am 20.9.2026). */
static BOOL stream_slice(void)
{
    long n;

    if (!g_st_on || !g_st_ring) {
        return FALSE;
    }
    if (g_st_ring->stop) {              /* der Abspieler will nicht mehr */
        stream_close();
        g_st_on = FALSE;
        return FALSE;
    }

    /* Eigene Datei: oeffnen, Etikett ueberspringen, lesen. Kein
     * Wiederaufsetzen noetig - eine Platte faellt nicht fuer drei
     * Minuten aus. */
    if (g_st_file[0]) {
        long n;

        if (!g_st_fh) {
            g_st_fh = Open((STRPTR)g_st_file, MODE_OLDFILE);
            if (!g_st_fh) {
                sprintf(g_st_msg, "Datei nicht lesbar: %.60s", g_st_file);
                g_st_ring->eof = TRUE;
                g_st_on = FALSE;
                return FALSE;
            }
            if (g_st_off > 0) {
                Seek(g_st_fh, g_st_off, OFFSET_BEGINNING);
            } else {
                file_skip_id3();
            }
            sprintf(g_st_msg, "Datei offen ab %ld", g_st_off);
        }

        if (ring_space(g_st_ring) < ST_CHUNK) {
            return FALSE;               /* Ring voll - schlafen legen */
        }
        n = Read(g_st_fh, g_st_buf, ST_CHUNK);
        if (n > 0) {
            ULONG put = 0;

            while (put < (ULONG)n && !g_st_ring->stop) {
                ULONG w = ring_write(g_st_ring, g_st_buf + put,
                                     (ULONG)n - put);
                if (w == 0) {
                    Delay(1);
                    continue;
                }
                put += w;
            }
            g_st_off += n;
            return TRUE;
        }
        sprintf(g_st_msg, "Datei zu Ende bei %ld", g_st_off);
        stream_close();
        g_st_ring->eof = TRUE;
        g_st_on = FALSE;
        return FALSE;
    }

    if (g_st_url[0]) {
        return radio_slice();
    }

    if (!g_st_open) {
        if (sub_stream_open(g_prefs, g_st_id, g_st_off, &g_st) != SUB_OK) {
            sprintf(g_st_msg, "oeffnen ab %ld fehlt: %.40s",
                    g_st_off, sub_last_error());
            /* In ZEIT rechnen, nicht in Anlaeufen: diese Funkstrecke
             * faellt ein bis drei Minuten aus (AGENTS.md 5.8). */
            if (g_st_waited >= ST_PATIENCE) {
                g_st_ring->eof = TRUE;
                g_st_on = FALSE;
                return FALSE;
            }
            Delay(ST_PAUSE);
            g_st_waited += ST_PAUSE;
            g_st_waited_s = g_st_waited / 50;
            return TRUE;
        }
        g_st_open = TRUE;
        g_st_waited = 0;
        sprintf(g_st_msg, "offen ab %ld von %ld", g_st_off, g_st.total);
        if (g_st_off == 0) {
            stream_skip_id3();
        }
    }

    if (ring_space(g_st_ring) < ST_CHUNK) {
        return FALSE;                   /* Ring voll - schlafen legen */
    }

    n = sub_stream_read(&g_st, g_st_buf, ST_CHUNK);
    if (n > 0) {
        ULONG put = 0;

        while (put < (ULONG)n && !g_st_ring->stop) {
            ULONG w = ring_write(g_st_ring, g_st_buf + put, (ULONG)n - put);

            if (w == 0) {
                Delay(1);
                continue;
            }
            put += w;
        }
        g_st_off = g_st.pos;
        return TRUE;
    }
    if (n == 0) {
        stream_close();
        if (g_st.total > 0 && g_st_off < g_st.total) {
            return TRUE;                /* abgeschnitten - neu ansetzen */
        }
        sprintf(g_st_msg, "Ende bei %ld von %ld", g_st_off, g_st.total);
        g_st_ring->eof = TRUE;          /* sauber zu Ende */
        g_st_on = FALSE;
        return FALSE;
    }
    /* Fehler: Verbindung weg. An derselben Stelle neu ansetzen. */
    stream_close();
    return TRUE;
}

/* Eigener Antwortport fuer die Stromauftraege. Er MUSS getrennt sein:
 * net_poll() der Oberflaeche wuerde einen Stromauftrag sonst fuer die
 * Antwort auf ihre Abfrage halten. */
static struct MsgPort *g_sreply = NULL;
static int g_soutstanding = 0;

BOOL net_submit_stream(struct NetJob *job)
{
    if (!g_running || !g_workport) {
        return FALSE;
    }
    if (!g_sreply) {
        g_sreply = CreateMsgPort();
        if (!g_sreply) {
            return FALSE;
        }
    }
    job->msg.mn_Node.ln_Type = NT_MESSAGE;
    job->msg.mn_Length       = sizeof(*job);
    job->msg.mn_ReplyPort    = g_sreply;
    PutMsg(g_workport, (struct Message *)job);
    g_soutstanding++;
    return TRUE;
}

void net_stream_reap(void)
{
    struct Message *m;

    if (!g_sreply) {
        return;
    }
    while ((m = GetMsg(g_sreply)) != NULL) {
        g_soutstanding--;
    }
}

void net_stream_end(void)
{
    /* Kein eigener Auftrag noetig: der Arbeiter sieht das Stoppzeichen
     * im Ring, sobald er die naechste Scheibe holen will. */
    if (g_st_ring) {
        g_st_ring->stop = TRUE;
    }
}

BOOL net_stream_active(void)
{
    return g_st_on;
}

long net_stream_pos(void)
{
    return g_st_off;
}

long net_stream_total(void)
{
    return g_st.total;
}

LONG net_stream_waited(void)
{
    return g_st_waited_s;
}

const char *net_stream_msg(void)
{
    return g_st_msg;
}

BOOL net_stream_notmp3(void)
{
    return g_st_notmp3;
}

const char *net_stream_ctype(void)
{
    return g_st_ctype;
}

ULONG net_icy_seq(void)
{
    return g_icy_seq;
}

void net_icy_title(char *out, int size)
{
    Forbid();
    strncpy(out, g_icy_title, size - 1);
    out[size - 1] = '\0';
    Permit();
}

static void worker(void);

/* Wartet auf eine Antwort, aber nicht ewig.
 *
 * WaitPort() allein war hier ein Fehler mit Ansage: als der
 * Arbeitsprozess einmal abstuerzte und von SmartCrash entfernt wurde,
 * blieb die Oberflaeche beim Beenden in WaitPort stehen - auf eine
 * Antwort, die nie kommen konnte. Ein Programm, das sich nicht mehr
 * beenden laesst, ist das schlechtestmoegliche Verhalten: es haelt sein
 * Fenster, und ueber den amiagent blockiert es den Kanal.
 *
 * Ein Ctrl-C haette auch nicht geholfen - WaitPort() horcht nur auf das
 * Signal seines Ports und sieht SIGBREAKF_CTRL_C gar nicht.
 *
 * TRUE, wenn eine Antwort kam; FALSE bei Zeitablauf. */
static BOOL wait_reply(long secs)
{
    struct MsgPort     *tport;
    struct timerequest *treq;
    ULONG portsig, timersig, got;
    BOOL  ok = FALSE;

    portsig = 1UL << g_reply->mp_SigBit;

    tport = CreateMsgPort();
    if (!tport) {
        /* Ohne Zeitgeber bleibt nur das unbegrenzte Warten - immer noch
         * besser, als die Antwort zu verwerfen und den Speicher unter
         * dem Arbeiter wegzuziehen. */
        WaitPort(g_reply);
        return TRUE;
    }
    treq = (struct timerequest *)CreateIORequest(tport, sizeof(*treq));
    if (!treq || OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
                            (struct IORequest *)treq, 0) != 0) {
        if (treq) {
            DeleteIORequest((struct IORequest *)treq);
        }
        DeleteMsgPort(tport);
        WaitPort(g_reply);
        return TRUE;
    }

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs    = secs;
    treq->tr_time.tv_micro   = 0;
    SendIO((struct IORequest *)treq);
    timersig = 1UL << tport->mp_SigBit;

    got = Wait(portsig | timersig);
    if (got & portsig) {
        ok = TRUE;
    }

    AbortIO((struct IORequest *)treq);
    WaitIO((struct IORequest *)treq);
    CloseDevice((struct IORequest *)treq);
    DeleteIORequest((struct IORequest *)treq);
    DeleteMsgPort(tport);

    return ok;
}

BOOL net_start(struct Prefs *p)
{
    struct Process *proc;

    if (g_running) {
        return TRUE;
    }

    g_prefs = p;
    g_reply = CreateMsgPort();
    if (!g_reply) {
        return FALSE;
    }

    g_starter  = FindTask(NULL);
    g_workport = NULL;

    /* Ein eigener Prozess, kein Task: die Netzfunktionen und die
     * datatypes rufen dos.library, und das setzt eine Process-Struktur
     * mit gueltigem pr_CLI/pr_MsgPort voraus.
     *
     * 64 KB Stapel: der tiefste Weg fuehrt durch AmiSSL und die
     * OpenSSL-Zertifikatspruefung, und die ist nicht sparsam. Der
     * Vorgabewert von 4 KB reicht dafuer nicht.
     *
     * 16 KB reichten auf dem A500 (68020), aber nicht in Amiberry
     * (68040 mit FPU, 19.9.2026): dort stuerzte die Oberflaeche beim
     * ersten Netzabruf ab, waehrend das CLI - TLS im Shell-Prozess,
     * also mit dem Shell-Stapel - einwandfrei lief. AmiSSL laedt je
     * CPU eine eigene Variante. Der Stapel hier haengt NICHT vom
     * "Stack"-Befehl der Shell ab; der wirkt nur auf den Hauptprozess. */
    proc = CreateNewProcTags(NP_Entry,     (ULONG)worker,
                             NP_Name,      (ULONG)"AmiSubsonic.net",
                             NP_StackSize, 65536,
                             NP_Priority,  0,
                             TAG_DONE);
    if (!proc) {
        DeleteMsgPort(g_reply);
        g_reply = NULL;
        return FALSE;
    }

    /* Der Arbeiter legt seinen Port selbst an und meldet sich dann mit
     * SIGF_SINGLE zurueck. Erst danach darf ihm etwas geschickt
     * werden. */
    Wait(SIGF_SINGLE);

    if (!g_workport) {
        DeleteMsgPort(g_reply);
        g_reply = NULL;
        return FALSE;
    }

    g_running = TRUE;
    return TRUE;
}

void net_stop(void)
{
    struct NetJob quit;

    if (!g_running) {
        return;
    }

    /* Erst einen etwaigen laufenden Auftrag abwarten - sonst schreibt
     * der Arbeiter noch in Strukturen, die es gleich nicht mehr gibt.
     *
     * Die Frist ist grosszuegig: eine Anfrage laeuft im schlimmsten Fall
     * in die Zeitgrenzen von 5 s Verbindung plus 20 s Uebertragung. Wer
     * darueber hinaus nicht antwortet, ist tot. */
    if (g_busy) {
        if (!wait_reply(30)) {
            /* Der Arbeiter antwortet nicht mehr. Aufgeben, aber NICHTS
             * freigeben und den Port nicht abbauen - falls er doch noch
             * lebt, schreibt er sonst in Speicher, den es nicht mehr
             * gibt. Lieber ein Port und ein Prozess zu viel als ein
             * Absturz beim Beenden. */
            g_running = FALSE;
            g_workport = NULL;
            g_reply = NULL;
            return;
        }
        while (GetMsg(g_reply)) {
            ;
        }
        g_busy = FALSE;
    }

    memset(&quit, 0, sizeof(quit));
    quit.msg.mn_Node.ln_Type = NT_MESSAGE;
    quit.msg.mn_Length       = sizeof(quit);
    quit.msg.mn_ReplyPort    = g_reply;
    quit.op = NJ_QUIT;

    PutMsg(g_workport, (struct Message *)&quit);
    if (!wait_reply(10)) {
        g_running = FALSE;
        g_workport = NULL;
        g_reply = NULL;
        return;
    }
    while (GetMsg(g_reply)) {
        ;
    }

    /* Der Arbeiter raeumt seinen Port ab und meldet sich ein letztes
     * Mal, bevor er endet. Ohne dieses Warten koennte das Programm
     * enden, waehrend er noch laeuft - und sein Code liegt in unserem
     * Speicher. */
    Wait(SIGF_SINGLE);

    DeleteMsgPort(g_reply);
    g_reply = NULL;
    g_workport = NULL;
    g_running = FALSE;
}

ULONG net_signal(void)
{
    return g_reply ? (1UL << g_reply->mp_SigBit) : 0;
}

BOOL net_busy(void)
{
    return g_busy;
}

BOOL net_submit(struct NetJob *job)
{
    if (!g_running || g_busy) {
        return FALSE;
    }

    job->msg.mn_Node.ln_Type = NT_MESSAGE;
    job->msg.mn_Length       = sizeof(*job);
    job->msg.mn_ReplyPort    = g_reply;
    job->rc = SUB_OK;
    job->error[0] = '\0';

    g_busy = TRUE;
    PutMsg(g_workport, (struct Message *)job);
    return TRUE;
}

struct NetJob *net_poll(void)
{
    struct NetJob *job;

    if (!g_reply) {
        return NULL;
    }
    job = (struct NetJob *)GetMsg(g_reply);
    if (job) {
        g_busy = FALSE;
    }
    return job;
}

/* ------------------------------------------------------------------ */
/* Der Arbeitsprozess                                                  */
/* ------------------------------------------------------------------ */

static void do_job(struct NetJob *j)
{
    switch (j->op) {
    case NJ_RESOLVE:
        j->rc = sub_resolve(g_prefs);
        break;

    case NJ_PING:
        j->rc = sub_ping(g_prefs);
        break;

    case NJ_ALBUMS:
        j->rc = sub_get_albums(g_prefs,
                               j->listtype[0] ? j->listtype
                                              : "alphabeticalByName",
                               j->size > 0 ? j->size : 100, 0, j->out_list);
        break;

    case NJ_TRACKS:
        /* Leere Suchanfrage: Navidrome liefert dann alle Titel, und
         * songOffset blaettert darin. */
        j->rc = sub_search_songs(g_prefs, "", j->size > 0 ? j->size : 100,
                                 j->offset, j->out_list);
        break;

    case NJ_FAVORITES:
        j->rc = sub_get_starred_songs(g_prefs, j->out_list);
        break;

    case NJ_RADIOS:
        j->rc = sub_get_radios(g_prefs, j->out_list);
        break;

    case NJ_ALBUM_SONGS:
        j->rc = sub_get_album_songs(g_prefs, j->id, j->out_album,
                                    j->out_list);
        break;

    case NJ_SCAN: {
        /* Der Durchgang laeuft im Arbeitsprozess und nicht in der
         * Oberflaeche - ein Verzeichnisbaum auf einer 68k-Maschine
         * kostet Zeit, und das Fenster soll bedienbar bleiben.
         *
         * Waehrend er laeuft, ruht der Nachschub fuer einen laufenden
         * Titel. Bei 40 s Vorrat im Ring faellt ein kurzer Durchgang
         * nicht auf; wird er laenger, muss er in Scheiben zerlegt
         * werden wie der Strom. Genau dafuer wird die Zeit gemessen. */
        struct ScanStat st;

        j->rc = local_scan(j->path, j->out_list, &st);
        j->scan_dirs  = st.dirs;
        j->scan_files = st.files;
        j->scan_cs    = st.cs;
        j->scan_tagcs = st.tag_cs;
        j->scan_full  = st.full;
        if (j->rc != SUB_OK) {
            strncpy(j->error, local_last_error(), sizeof(j->error) - 1);
        }
        break;
    }

    case NJ_COVER:
        /* Ueber den Cache, nicht unmittelbar: sub_get_cover_cached()
         * schreibt den Zielpfad nach j->path zurueck, laesst eine schon
         * vorhandene Datei in Ruhe und raeumt eine abgebrochene weg.
         * Damit gibt es im ganzen Programm genau EINEN Ort, der
         * entscheidet, wo ein Cover liegt. */
        j->rc = sub_get_cover_cached(g_prefs, j->id, j->size,
                                     j->path, (int)sizeof(j->path));
        break;

    case NJ_LYRICS:
        /* Erst der eigene Server - was in der Datei steckt, passt
         * garantiert zur Aufnahme und kostet keine fremde Anfrage. */
        j->rc = sub_get_lyrics(g_prefs, j->id, j->out_text, j->out_textsize);
        if (j->rc == SUB_OK && j->out_text[0]) {
            break;
        }
        j->rc = sub_get_lyrics_net(j->artist, j->title, j->album,
                                   j->duration,
                                   j->out_text, j->out_textsize,
                                   NULL, 0);
        break;

    default:
        j->rc = SUB_EAPI;
        break;
    }

    if (j->rc != SUB_OK) {
        strncpy(j->error, sub_last_error(), sizeof(j->error) - 1);
        j->error[sizeof(j->error) - 1] = '\0';
    }
}

static void worker(void)
{
    struct MsgPort *port;
    struct NetJob  *job;
    BOOL run = TRUE;

    port = CreateMsgPort();
    g_workport = port;

    /* Egal ob der Port zustande kam - der Starter wartet auf dieses
     * Signal und prueft danach selbst, ob g_workport gesetzt ist. */
    Signal(g_starter, SIGF_SINGLE);

    if (!port) {
        return;
    }

    while (run) {
        /* Laeuft ein Strom, darf hier NICHT gewartet werden - sonst
         * bekaeme der Abspieler keinen Nachschub. Dann wird eine
         * Scheibe geholt und danach nachgesehen, ob ein Auftrag
         * anliegt. */
        if (!g_st_on) {
            WaitPort(port);
        } else if (!stream_slice()) {
            /* Nichts zu tun - eine Fuenfzigstelsekunde schlafen. Der
             * Ring fasst 40 s, auf 20 ms kommt es nicht an, und die
             * Maschine gehoert in dieser Zeit wieder allen anderen. */
            Delay(1);
        }

        while ((job = (struct NetJob *)GetMsg(port)) != NULL) {
            switch (job->op) {
            case NJ_QUIT:
                run = FALSE;
                break;

            case NJ_STREAM:
                /* Setzt nur den Zustand und ist sofort beantwortet -
                 * gefuellt wird nebenher. */
                stream_begin(job);
                job->rc = SUB_OK;
                break;

            case NJ_STREAM_STOP:
                stream_close();
                g_st_on = FALSE;
                job->rc = SUB_OK;
                break;

            default:
                do_job(job);
                break;
            }
            ReplyMsg((struct Message *)job);
        }
    }

    stream_close();

    /* bsdsocket und AmiSSL gehoeren diesem Prozess - AmiSSLs Doku
     * verlangt ausdruecklich, dass jeder benutzende Prozess selbst
     * aufraeumt. */
    sub_cleanup();

    DeleteMsgPort(port);
    g_workport = NULL;

    /* Fertig - der Starter darf jetzt enden. */
    Signal(g_starter, SIGF_SINGLE);
}
