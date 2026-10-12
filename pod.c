/* AmiSubsonic - Podcasts, siehe pod.h.
 *
 * Alles hier, was das Netz anfasst (pod_search, pod_refresh,
 * pod_cover_cached), gehoert in den Prozess, dem das Netz gehoert.
 * Grosse Puffer kommen von AllocVec, nicht vom Stapel und nicht aus
 * statischen Feldern: pod_list() laeuft in der Oberflaeche, pod_refresh()
 * gleichzeitig im Netzprozess - ein gemeinsamer statischer Puffer waere
 * ein stiller Fehler. */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pod.h"
#include "json.h"

/* Version des Dateiformats - eine hoehere Zahl liest alle gespeicherten
 * Feeds beim naechsten Scan neu. 3: doppelt maskierte Entitaeten werden
 * aufgeloest (vorher stand "&amp;" in Titeln, 12.10.2026). */
#define POD_FORMAT 3

static char g_pod_err[160] = "";

static int pod_fail(int code, const char *msg)
{
    strncpy(g_pod_err, msg, sizeof(g_pod_err) - 1);
    g_pod_err[sizeof(g_pod_err) - 1] = '\0';
    return code;
}

const char *pod_last_error(void)
{
    return g_pod_err[0] ? g_pod_err : sub_last_error();
}

static void cut(char *dst, int size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* FNV-1a, 32 Bit. Nur ein Name fuer Dateien und Zustaende - eine
 * Kollision kostet hoechstens einen falschen Stand, keine Daten. */
void pod_key(const char *s, char *out)
{
    ULONG h = 2166136261UL;

    while (*s) {
        h ^= (UBYTE)*s++;
        h *= 16777619UL;
        /* Auf dem Amiga ohnehin 32 Bit - auf dem Mac (Pruefhuelle) ist
         * ULONG breiter, und acht Ziffern muessen es ueberall sein. */
        h &= 0xffffffffUL;
    }
    sprintf(out, "%08lx", h);
}

/* ------------------------------------------------------------------ */
/* Suche bei Apple                                                     */
/* ------------------------------------------------------------------ */

static void url_enc(const char *s, char *out, int size)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;

    while (*s && o < size - 4) {
        UBYTE c = (UBYTE)*s++;

        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else if (c >= 0x80) {
            /* Latin-1 von der Tastatur -> UTF-8, das will Apple. */
            out[o++] = '%';
            out[o++] = hex[(0xc0 | (c >> 6)) >> 4];
            out[o++] = hex[(0xc0 | (c >> 6)) & 15];
            if (o < size - 4) {
                out[o++] = '%';
                out[o++] = hex[(0x80 | (c & 0x3f)) >> 4];
                out[o++] = hex[(0x80 | (c & 0x3f)) & 15];
            }
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

struct SearchCtx {
    struct SubList *out;
    struct PodShow *cur;
    char small[POD_URL_LEN];
};

static int search_cb(const struct JsonEv *ev, void *ud)
{
    struct SearchCtx *c = (struct SearchCtx *)ud;

    if (ev->type == JSON_OBJ_BEGIN && JSON_IS(ev, "results.#")) {
        c->cur = (struct PodShow *)list_add(c->out);
        c->small[0] = '\0';
        return c->cur == NULL;
    }
    if (!c->cur) {
        return 0;
    }
    if (JSON_IS(ev, "results.#.collectionName")) {
        json_text(ev, c->cur->title, sizeof(c->cur->title));
    } else if (JSON_IS(ev, "results.#.artistName")) {
        json_text(ev, c->cur->author, sizeof(c->cur->author));
    } else if (JSON_IS(ev, "results.#.feedUrl")) {
        json_text(ev, c->cur->feed, sizeof(c->cur->feed));
    } else if (JSON_IS(ev, "results.#.artworkUrl600")) {
        json_text(ev, c->cur->image, sizeof(c->cur->image));
    } else if (JSON_IS(ev, "results.#.artworkUrl100")) {
        json_text(ev, c->small, sizeof(c->small));
    } else if (ev->type == JSON_OBJ_END && JSON_IS(ev, "results.#")) {
        char *p;

        /* Apple rechnet das Bild auf jede Groesse um, die in der
         * Adresse steht. 300 Punkte reichen fuer Bildwand und Flaeche -
         * das Original ist oft 3000x3000 und mehrere MB gross. */
        if (!c->cur->image[0]) {
            strcpy(c->cur->image, c->small);
        }
        p = strstr(c->cur->image, "600x600");
        if (p) {
            memcpy(p, "300x300", 7);
        }
        /* Ohne Feed-Adresse (manche Eintraege) laesst sich nichts
         * abonnieren - wieder weg damit. */
        if (!c->cur->feed[0]) {
            c->out->count--;
        }
        c->cur = NULL;
    }
    return 0;
}

int pod_search(const char *term, struct SubList *out)
{
    struct SearchCtx c;
    char path[400];
    char enc[300];
    char *resp = NULL;
    long len = 0;
    int rc;

    g_pod_err[0] = '\0';
    url_enc(term, enc, sizeof(enc));
    sprintf(path, "/search?media=podcast&entity=podcast&country=DE"
                  "&limit=20&term=%s", enc);
    rc = sub_http_request("https://itunes.apple.com", "GET", path,
                          NULL, NULL, &resp, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    memset(&c, 0, sizeof(c));
    c.out = out;
    rc = json_walk(resp, len, search_cb, &c);
    free(resp);
    if (rc < 0) {
        return pod_fail(SUB_EAPI, "podcast search: unreadable answer");
    }
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Den Feed lesen                                                      */
/* ------------------------------------------------------------------ */

/* Ein Leser, der den Strom STUECKWEISE bekommt. Kein Baum, kein ganzer
 * Puffer: die Feeds sind bis 8 MB gross (gemessen). Aufgehoben wird nur
 * der Text der Elemente, die gebraucht werden - die Shownotes, oft das
 * Meiste am Feed, laufen ungesehen durch. */

#define M_TEXT    0
#define M_TAG     1
#define M_CDATA   2
#define M_COMMENT 3

#define C_NONE     0
#define C_TITLE    1
#define C_GUID     2
#define C_PUBDATE  3
#define C_DURATION 4
#define C_AUTHOR   5

#define ORD_UNKNOWN 0
#define ORD_DESC    1           /* neueste zuerst - der Normalfall */
#define ORD_ASC     2           /* aelteste zuerst: alles lesen */

struct RssCtx {
    int   mode;
    char  tag[2048];
    int   tlen;
    char  inq;                  /* offenes Anfuehrungszeichen im Tag */
    int   marks;                /* gezaehlte ']' bzw. '-' am Ende */
    char  text[1024];
    int   xlen;
    int   cap;
    BOOL  in_item, in_image;
    BOOL  have_title;
    struct PodEpisode cur;
    struct PodFeed   *f;
    struct PodEpisode eps[POD_EPISODES];
    int   n;
    long  total;
    long  videos;               /* uebersprungene Video-Folgen */
    int   order;
    ULONG first_when;
    BOOL  stop;
};

static void text_add(struct RssCtx *c, char ch)
{
    if (c->cap != C_NONE && c->xlen < (int)sizeof(c->text) - 8) {
        c->text[c->xlen++] = ch;
    }
}

static void put_utf8(char *out, int *o, int size, unsigned long cp)
{
    if (cp < 0x80) {
        if (*o < size - 1) out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*o < size - 2) {
            out[(*o)++] = (char)(0xc0 | (cp >> 6));
            out[(*o)++] = (char)(0x80 | (cp & 0x3f));
        }
    } else if (*o < size - 3) {
        out[(*o)++] = (char)(0xe0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*o)++] = (char)(0x80 | (cp & 0x3f));
    }
}

/* Ein Durchgang: Entitaeten aufloesen, Leerraum zusammenziehen. Das
 * Ergebnis ist noch UTF-8. */
static void decode_once(const char *src, int len, char *out, int size)
{
    int i = 0, o = 0;
    BOOL sp = TRUE;             /* fuehrenden Leerraum weglassen */

    while (i < len && o < size - 4) {
        char ch = src[i];

        if (ch == '&') {
            const char *e = src + i;
            int rest = len - i;

            if (rest >= 5 && strncmp(e, "&amp;", 5) == 0) {
                out[o++] = '&'; i += 5; sp = FALSE; continue;
            }
            if (rest >= 4 && strncmp(e, "&lt;", 4) == 0) {
                out[o++] = '<'; i += 4; sp = FALSE; continue;
            }
            if (rest >= 4 && strncmp(e, "&gt;", 4) == 0) {
                out[o++] = '>'; i += 4; sp = FALSE; continue;
            }
            if (rest >= 6 && strncmp(e, "&quot;", 6) == 0) {
                out[o++] = '"'; i += 6; sp = FALSE; continue;
            }
            if (rest >= 6 && strncmp(e, "&apos;", 6) == 0) {
                out[o++] = '\''; i += 6; sp = FALSE; continue;
            }
            if (rest >= 4 && e[1] == '#') {
                unsigned long cp = 0;
                int k = 2;

                if (e[2] == 'x' || e[2] == 'X') {
                    for (k = 3; k < rest && isxdigit((UBYTE)e[k]); k++) {
                        char h = e[k];
                        cp = cp * 16 + (unsigned long)
                             (h <= '9' ? h - '0' : (tolower(h) - 'a' + 10));
                    }
                } else {
                    for (; k < rest && isdigit((UBYTE)e[k]); k++) {
                        cp = cp * 10 + (unsigned long)(e[k] - '0');
                    }
                }
                if (k < rest && e[k] == ';') {
                    put_utf8(out, &o, size, cp);
                    i += k + 1;
                    sp = FALSE;
                    continue;
                }
            }
        }
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!sp) {
                out[o++] = ' ';
                sp = TRUE;
            }
            i++;
            continue;
        }
        out[o++] = ch;
        sp = FALSE;
        i++;
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
}

/* Entitaeten aufloesen und nach Latin-1. Steht danach noch eine Entitaet
 * im Text, ein zweiter Durchgang: manche Feeds maskieren doppelt ("Klima
 * &amp;amp; Recht" - Lage der Nation, gemessen 12.10.2026). Beide
 * Durchgaenge laufen VOR der Wandlung nach Latin-1, damit Umlaute nicht
 * zweimal gewandelt werden. */
static void text_decode(const char *src, int len, char *out, int size)
{
    char tmp[1024];

    decode_once(src, len, tmp, (int)sizeof(tmp));
    if (strchr(tmp, '&') && strchr(tmp, ';')) {
        decode_once(tmp, (int)strlen(tmp), out, size);
    } else {
        strncpy(out, tmp, size - 1);
        out[size - 1] = '\0';
    }
    utf8_to_latin1(out);
}

/* Ein Attribut aus dem Tag holen: name="wert" oder name='wert'. */
static BOOL tag_attr(const char *tag, const char *name, char *out, int size)
{
    int nl = (int)strlen(name);
    const char *p = tag;

    while ((p = strstr(p, name)) != NULL) {
        if ((p == tag || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n')
                && p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            char q = p[nl + 1];
            const char *v = p + nl + 2;
            const char *e = strchr(v, q);
            int n = e ? (int)(e - v) : (int)strlen(v);

            text_decode(v, n, out, size);
            return TRUE;
        }
        p += nl;
    }
    out[0] = '\0';
    return FALSE;
}

static const char *g_months = "JanFebMarAprMayJunJulAugSepOctNovDec";

/* "Mon, 12 Oct 2026 05:00:00 +0200" -> sortierbare Minuten und
 * "12 Oct 2026". Die Zeitzone faellt weg - auf ein paar Stunden kommt es
 * beim Sortieren der Folgen eines Podcasts nicht an. */
static void parse_date(const char *s, ULONG *when, char *disp)
{
    long day = 0, mon = 0, year = 0, hh = 0, mm = 0;
    char tok[16];
    int i;

    while (*s) {
        while (*s == ' ' || *s == ',') {
            s++;
        }
        for (i = 0; *s && *s != ' ' && *s != ',' && i < 15; i++) {
            tok[i] = *s++;
        }
        tok[i] = '\0';
        while (*s && *s != ' ' && *s != ',') {
            s++;
        }
        if (i == 0) {
            continue;
        }
        if (strchr(tok, ':')) {
            hh = atol(tok);
            mm = atol(strchr(tok, ':') + 1);
        } else if (isdigit((UBYTE)tok[0])) {
            long v = atol(tok);

            if (v > 31) {
                year = v;
            } else if (day == 0) {
                day = v;
            }
        } else if (i >= 3) {
            const char *m;

            tok[3] = '\0';
            tok[0] = (char)toupper((UBYTE)tok[0]);
            tok[1] = (char)tolower((UBYTE)tok[1]);
            tok[2] = (char)tolower((UBYTE)tok[2]);
            m = strstr(g_months, tok);
            if (m && mon == 0) {
                mon = (long)(m - g_months) / 3 + 1;
            }
        }
    }
    *when = (((ULONG)year * 12UL + (ULONG)mon) * 31UL + (ULONG)day) * 1440UL
          + (ULONG)hh * 60UL + (ULONG)mm;
    if (day && mon && year) {
        /* Begrenzt, damit es sicher in date[16] passt. */
        sprintf(disp, "%d %.3s %d", (int)(day % 100),
                g_months + (mon - 1) * 3, (int)(year % 10000));
    } else {
        disp[0] = '\0';
    }
}

/* "01:02:03", "62:03" oder "3723" (auch "3723.5") -> Sekunden. */
static long parse_duration(const char *s)
{
    long v = 0, part = 0;

    for (; *s; s++) {
        if (isdigit((UBYTE)*s)) {
            part = part * 10 + (*s - '0');
        } else if (*s == ':') {
            v = (v + part) * 60;
            part = 0;
        } else if (*s == '.') {
            break;
        }
    }
    return v + part;
}

static void item_done(struct RssCtx *c)
{
    struct PodEpisode *e = &c->cur;
    int i, oldest;

    if (!e->url[0]) {
        return;                 /* keine Audiodatei - kein Podcast */
    }
    /* Video spielt der Amiga nicht. Bei Apple stehen manche Sendungen
     * mehrfach, als Audio UND als Video (c't uplink, 12.10.2026) - wer
     * die Video-Fassung erwischt, soll es beim Abonnieren erfahren und
     * nicht erst beim Abspielen. */
    if (strncmp(e->type, "video/", 6) == 0) {
        c->videos++;
        /* 20 Videos und noch kein Ton: das ist ein Video-Feed. Nicht
         * noch Megabytes weiterlesen, um das zu bestaetigen. */
        if (c->videos >= 20 && c->n == 0) {
            c->stop = TRUE;
        }
        return;
    }
    if (!e->key[0]) {
        pod_key(e->url, e->key);
    }

    c->total++;
    if (c->total == 1) {
        c->first_when = e->when;
    } else if (c->total == 2) {
        c->order = (e->when > c->first_when) ? ORD_ASC : ORD_DESC;
    }

    if (c->n < POD_EPISODES) {
        c->eps[c->n++] = *e;
    } else {
        /* Voll: nur aufnehmen, wenn neuer als die aelteste. Wichtig nur
         * bei aufsteigenden Feeds - bei absteigenden wird vorher
         * aufgehoert. */
        oldest = 0;
        for (i = 1; i < c->n; i++) {
            if (c->eps[i].when < c->eps[oldest].when) {
                oldest = i;
            }
        }
        if (e->when > c->eps[oldest].when) {
            c->eps[oldest] = *e;
        }
    }
    if (c->order != ORD_ASC && c->n >= POD_EPISODES) {
        c->stop = TRUE;
    }
}

static void tag_done(struct RssCtx *c)
{
    char name[40];
    const char *t = c->tag;
    BOOL closing = FALSE;
    int i;

    c->tag[c->tlen] = '\0';
    if (t[0] == '?' || t[0] == '!') {
        return;
    }
    if (t[0] == '/') {
        closing = TRUE;
        t++;
    }
    for (i = 0; t[i] && t[i] != ' ' && t[i] != '/' && t[i] != '\t'
                && t[i] != '\n' && t[i] != '\r' && i < 39; i++) {
        name[i] = t[i];
    }
    name[i] = '\0';

    if (closing) {
        if (strcmp(name, "item") == 0) {
            item_done(c);
            c->in_item = FALSE;
        } else if (strcmp(name, "image") == 0) {
            c->in_image = FALSE;
        } else if (c->cap != C_NONE) {
            char buf[SUB_NAME_LEN + 64];

            text_decode(c->text, c->xlen, buf, sizeof(buf));
            switch (c->cap) {
            case C_TITLE:
                if (c->in_item) {
                    cut(c->cur.title, sizeof(c->cur.title), buf);
                } else if (!c->have_title) {
                    c->have_title = TRUE;
                    if (!c->f->show.title[0]) {
                        cut(c->f->show.title, sizeof(c->f->show.title), buf);
                    }
                }
                break;
            case C_GUID: {
                char raw[1024];

                /* Der Streuwert aus dem ROHEN Text: eine GUID ist eine
                 * Kennung, kein Anzeigetext. */
                memcpy(raw, c->text, c->xlen);
                raw[c->xlen] = '\0';
                pod_key(raw, c->cur.key);
                break;
            }
            case C_PUBDATE:
                parse_date(buf, &c->cur.when, c->cur.date);
                break;
            case C_DURATION:
                c->cur.dur_s = parse_duration(buf);
                break;
            case C_AUTHOR:
                if (!c->f->show.author[0]) {
                    cut(c->f->show.author, sizeof(c->f->show.author), buf);
                }
                break;
            }
            c->cap = C_NONE;
        }
        return;
    }

    /* Ein oeffnendes (oder leeres) Element. */
    if (strcmp(name, "item") == 0) {
        c->in_item = TRUE;
        memset(&c->cur, 0, sizeof(c->cur));
        return;
    }
    if (strcmp(name, "image") == 0 && !c->in_item) {
        c->in_image = TRUE;
        return;
    }
    if (strcmp(name, "enclosure") == 0 && c->in_item) {
        char v[32];

        tag_attr(c->tag, "url", c->cur.url, sizeof(c->cur.url));
        tag_attr(c->tag, "type", c->cur.type, sizeof(c->cur.type));
        tag_attr(c->tag, "length", v, sizeof(v));
        c->cur.size = atol(v);
        return;
    }
    if (strcmp(name, "itunes:image") == 0 && !c->in_item
            && !c->f->show.image[0]) {
        tag_attr(c->tag, "href", c->f->show.image, sizeof(c->f->show.image));
        return;
    }

    c->cap = C_NONE;
    if (strcmp(name, "title") == 0 && !c->in_image) {
        c->cap = C_TITLE;
    } else if (c->in_item && strcmp(name, "guid") == 0) {
        c->cap = C_GUID;
    } else if (c->in_item && strcmp(name, "pubDate") == 0) {
        c->cap = C_PUBDATE;
    } else if (c->in_item && strcmp(name, "itunes:duration") == 0) {
        c->cap = C_DURATION;
    } else if (!c->in_item && strcmp(name, "itunes:author") == 0) {
        c->cap = C_AUTHOR;
    }
    c->xlen = 0;
    /* <title/> - leeres Element, nichts zu sammeln. */
    if (c->tlen > 0 && c->tag[c->tlen - 1] == '/') {
        c->cap = C_NONE;
    }
}

static void rss_feed(struct RssCtx *c, const char *buf, long n)
{
    long i;

    for (i = 0; i < n && !c->stop; i++) {
        char ch = buf[i];

        switch (c->mode) {
        case M_TEXT:
            if (ch == '<') {
                c->mode = M_TAG;
                c->tlen = 0;
                c->inq = 0;
            } else {
                text_add(c, ch);
            }
            break;

        case M_TAG:
            if (c->inq) {
                if (ch == c->inq) {
                    c->inq = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                c->inq = ch;
            } else if (ch == '>') {
                tag_done(c);
                c->mode = M_TEXT;
                break;
            }
            if (c->tlen < (int)sizeof(c->tag) - 1) {
                c->tag[c->tlen++] = ch;
            }
            if (c->tlen == 8 && memcmp(c->tag, "![CDATA[", 8) == 0) {
                c->mode = M_CDATA;
                c->marks = 0;
            } else if (c->tlen == 3 && memcmp(c->tag, "!--", 3) == 0) {
                c->mode = M_COMMENT;
                c->marks = 0;
            }
            break;

        case M_CDATA:
            /* Ende ist "]]>". Gezaehlte ']' erst nachtragen, wenn klar
             * ist, dass sie Inhalt waren. */
            if (ch == ']') {
                c->marks++;
            } else if (ch == '>' && c->marks >= 2) {
                while (c->marks-- > 2) {
                    text_add(c, ']');
                }
                c->marks = 0;
                c->mode = M_TEXT;
            } else {
                while (c->marks > 0) {
                    text_add(c, ']');
                    c->marks--;
                }
                /* Entitaeten in CDATA werden mit aufgeloest. Streng
                 * genommen falsch, aber Feeds maskieren dort doppelt
                 * ("Klima &amp; Recht" in CDATA, Lage der Nation,
                 * gemessen), und Podcast-Apps zeigen es aufgeloest. Ein
                 * echtes '&' laesst text_decode() stehen. */
                text_add(c, ch);
            }
            break;

        case M_COMMENT:
            if (ch == '-') {
                c->marks++;
            } else if (ch == '>' && c->marks >= 2) {
                c->mode = M_TEXT;
                c->marks = 0;
            } else {
                c->marks = 0;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Ablage                                                              */
/* ------------------------------------------------------------------ */

static void file_name(const char *id, const char *ext, char *out)
{
    sprintf(out, POD_DIR "/%.8s.%s", id, ext);
}

/* Tabulator und Zeilenende kommen in den Feldern nicht vor - die Datei
 * ist zeilen- und tabulatorgetrennt. */
static void clean(char *s)
{
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

static int feed_write(struct PodFeed *f)
{
    char path[64];
    BPTR fh, lock;
    int i;

    lock = CreateDir((STRPTR)POD_DIR);
    if (lock) {
        UnLock(lock);
    }
    file_name(f->show.id, "feed", path);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        return pod_fail(SUB_EMEM, "cannot write podcast file");
    }
    clean(f->show.title);
    clean(f->show.author);
    FPrintf(fh, "AmiSubsonic podcast %ld\n", (LONG)POD_FORMAT);
    FPrintf(fh, "feed=%s\n", (LONG)(ULONG)f->show.feed);
    FPrintf(fh, "title=%s\n", (LONG)(ULONG)f->show.title);
    FPrintf(fh, "author=%s\n", (LONG)(ULONG)f->show.author);
    FPrintf(fh, "image=%s\n", (LONG)(ULONG)f->show.image);
    FPrintf(fh, "etag=%s\n", (LONG)(ULONG)f->etag);
    FPrintf(fh, "lastmod=%s\n", (LONG)(ULONG)f->lastmod);
    for (i = 0; i < f->count; i++) {
        struct PodEpisode *e = &f->ep[i];

        clean(e->title);
        clean(e->url);
        FPrintf(fh, "ep=%s\t%lu\t%ld\t%ld\t%s\t%s\t%s\t%s\n",
                (LONG)(ULONG)e->key, e->when, e->dur_s, e->size,
                (LONG)(ULONG)e->date, (LONG)(ULONG)e->type,
                (LONG)(ULONG)e->url, (LONG)(ULONG)e->title);
    }
    Close(fh);
    return SUB_OK;
}

/* Naechstes Feld bis Tabulator oder Zeilenende, mit Laengengrenze. */
static char *field(char *p, char *out, int size)
{
    int i = 0;

    while (*p && *p != '\t' && *p != '\n') {
        if (i < size - 1) {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (*p == '\t') ? p + 1 : p;
}

static void state_read(struct PodFeed *f)
{
    char path[64];
    char line[80];
    BPTR fh;
    int i;

    file_name(f->show.id, "state", path);
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        return;
    }
    while (FGets(fh, (STRPTR)line, sizeof(line) - 1)) {
        char key[12];
        long pos = 0, done = 0;

        if (sscanf(line, "%11s %ld %ld", key, &pos, &done) != 3) {
            continue;
        }
        for (i = 0; i < f->count; i++) {
            if (strcmp(f->ep[i].key, key) == 0) {
                f->ep[i].pos_ms = pos;
                f->ep[i].done = (done != 0);
                break;
            }
        }
    }
    Close(fh);
}

int pod_save_state(const struct PodFeed *f)
{
    char path[64];
    BPTR fh, lock;
    int i;

    lock = CreateDir((STRPTR)POD_DIR);
    if (lock) {
        UnLock(lock);
    }
    file_name(f->show.id, "state", path);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        return pod_fail(SUB_EMEM, "cannot write podcast state");
    }
    for (i = 0; i < f->count; i++) {
        const struct PodEpisode *e = &f->ep[i];

        if (e->pos_ms > 0 || e->done) {
            FPrintf(fh, "%s %ld %ld\n", (LONG)(ULONG)e->key, e->pos_ms,
                    (LONG)(e->done ? 1 : 0));
        }
    }
    Close(fh);
    return SUB_OK;
}

static void count_new(struct PodFeed *f)
{
    int i;

    f->show.nnew = 0;
    f->show.count = f->count;
    for (i = 0; i < f->count; i++) {
        if (!f->ep[i].done && f->ep[i].pos_ms == 0) {
            f->show.nnew++;
        }
    }
}

int pod_load(const char *id, struct PodFeed *f)
{
    char path[64];
    char *line;
    BPTR fh;
    BOOL oldfmt = TRUE;         /* ohne Kopfzeile: sicher alt */

    memset(f, 0, sizeof(*f));
    cut(f->show.id, sizeof(f->show.id), id);
    file_name(id, "feed", path);
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        return pod_fail(SUB_ENOPREFS, "podcast not found");
    }
    line = AllocVec(1400, MEMF_ANY);
    if (!line) {
        Close(fh);
        return pod_fail(SUB_EMEM, "out of memory");
    }
    while (FGets(fh, (STRPTR)line, 1399)) {
        char *v = strchr(line, '=');
        char *nl = strchr(line, '\n');

        if (nl) {
            *nl = '\0';
        }
        if (strncmp(line, "AmiSubsonic podcast ", 20) == 0) {
            oldfmt = (atol(line + 20) < POD_FORMAT);
            continue;
        }
        if (!v) {
            continue;
        }
        *v++ = '\0';
        if (strcmp(line, "feed") == 0) {
            cut(f->show.feed, sizeof(f->show.feed), v);
        } else if (strcmp(line, "title") == 0) {
            cut(f->show.title, sizeof(f->show.title), v);
        } else if (strcmp(line, "author") == 0) {
            cut(f->show.author, sizeof(f->show.author), v);
        } else if (strcmp(line, "image") == 0) {
            cut(f->show.image, sizeof(f->show.image), v);
        } else if (strcmp(line, "etag") == 0) {
            cut(f->etag, sizeof(f->etag), v);
        } else if (strcmp(line, "lastmod") == 0) {
            cut(f->lastmod, sizeof(f->lastmod), v);
        } else if (strcmp(line, "ep") == 0 && f->count < POD_EPISODES) {
            struct PodEpisode *e = &f->ep[f->count];
            char num[24];

            v = field(v, e->key, sizeof(e->key));
            v = field(v, num, sizeof(num));
            e->when = strtoul(num, NULL, 10);
            v = field(v, num, sizeof(num));
            e->dur_s = atol(num);
            v = field(v, num, sizeof(num));
            e->size = atol(num);
            v = field(v, e->date, sizeof(e->date));
            v = field(v, e->type, sizeof(e->type));
            v = field(v, e->url, sizeof(e->url));
            field(v, e->title, sizeof(e->title));
            if (e->key[0] && e->url[0]) {
                f->count++;
            }
        }
    }
    FreeVec(line);
    Close(fh);
    /* Mit einem aelteren Leser geschrieben: beim naechsten Scan den Feed
     * GANZ neu lesen, statt "nicht geaendert" (304) zu bekommen - sonst
     * blieben dessen Fehler in den gespeicherten Titeln stehen, bis der
     * Feed sich von selbst aendert. */
    if (oldfmt) {
        f->etag[0] = '\0';
        f->lastmod[0] = '\0';
    }
    state_read(f);
    count_new(f);
    return SUB_OK;
}

int pod_refresh(struct PodFeed *f, BOOL *changed)
{
    struct SubStream st;
    struct RssCtx *c;
    char hdrs[200];
    char *buf;
    int rc, i, j;

    *changed = FALSE;
    g_pod_err[0] = '\0';
    if (!f->show.id[0]) {
        pod_key(f->show.feed, f->show.id);
    }

    /* "Nur wenn geaendert": dann kommt 304 und gar kein Feed. Zwei von
     * drei gemessenen Servern koennen das. */
    hdrs[0] = '\0';
    if (f->etag[0] && strlen(f->etag) < 90) {
        sprintf(hdrs, "If-None-Match: %s\r\n", f->etag);
    }
    if (f->lastmod[0] && strlen(f->lastmod) < 60) {
        sprintf(hdrs + strlen(hdrs), "If-Modified-Since: %s\r\n", f->lastmod);
    }
    rc = sub_url_open(f->show.feed, hdrs[0] ? hdrs : NULL, 0, &st);
    if (rc != SUB_OK) {
        return rc;
    }
    if (st.status == 304) {
        return SUB_OK;
    }

    c = AllocVec(sizeof(*c), MEMF_ANY | MEMF_CLEAR);
    buf = AllocVec(4096, MEMF_ANY);
    if (!c || !buf) {
        sub_stream_close(&st);
        if (c) FreeVec(c);
        if (buf) FreeVec(buf);
        return pod_fail(SUB_EMEM, "out of memory");
    }
    c->f = f;

    for (;;) {
        long n = sub_stream_read(&st, buf, 4096);

        if (n <= 0) {
            break;
        }
        rss_feed(c, buf, n);
        if (c->stop) {
            break;              /* die neuesten 50 sind da - Schluss */
        }
    }
    sub_stream_close(&st);

    if (c->n == 0) {
        BOOL video = (c->videos > 0);

        FreeVec(c);
        FreeVec(buf);
        return pod_fail(SUB_EAPI, video
                        ? "video podcast - choose the audio version"
                        : "feed has no audio episodes");
    }

    /* Neueste zuerst. */
    for (i = 1; i < c->n; i++) {
        struct PodEpisode tmp = c->eps[i];

        for (j = i - 1; j >= 0 && c->eps[j].when < tmp.when; j--) {
            c->eps[j + 1] = c->eps[j];
        }
        c->eps[j + 1] = tmp;
    }

    for (i = 0; i < c->n; i++) {
        f->ep[i] = c->eps[i];
    }
    f->count = c->n;
    cut(f->etag, sizeof(f->etag), st.etag);
    cut(f->lastmod, sizeof(f->lastmod), st.lastmod);
    FreeVec(c);
    FreeVec(buf);

    rc = feed_write(f);
    if (rc != SUB_OK) {
        return rc;
    }
    /* Den Stand wieder dazu - er steht nur in der .state-Datei. */
    state_read(f);
    count_new(f);
    *changed = TRUE;
    return SUB_OK;
}

int pod_list(struct SubList *out)
{
    struct FileInfoBlock *fib;
    struct PodFeed *f;
    BPTR lock;

    lock = Lock((STRPTR)POD_DIR, ACCESS_READ);
    if (!lock) {
        return SUB_OK;          /* noch keine Abos */
    }
    fib = AllocDosObject(DOS_FIB, NULL);
    f = AllocVec(sizeof(*f), MEMF_ANY);
    if (!fib || !f) {
        if (fib) FreeDosObject(DOS_FIB, fib);
        if (f) FreeVec(f);
        UnLock(lock);
        return pod_fail(SUB_EMEM, "out of memory");
    }
    if (Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            char id[12];
            int n = (int)strlen(fib->fib_FileName);
            struct PodShow *s;

            if (n != 13 || strcmp(fib->fib_FileName + 8, ".feed") != 0) {
                continue;
            }
            cut(id, 9, fib->fib_FileName);
            if (pod_load(id, f) != SUB_OK) {
                continue;
            }
            s = (struct PodShow *)list_add(out);
            if (!s) {
                break;
            }
            *s = f->show;
        }
    }
    FreeVec(f);
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return SUB_OK;
}

int pod_remove(const char *id)
{
    char path[64];

    file_name(id, "feed", path);
    DeleteFile((STRPTR)path);
    file_name(id, "state", path);
    DeleteFile((STRPTR)path);
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Cover                                                               */
/* ------------------------------------------------------------------ */

void pod_cover_id(const char *id, char *out)
{
    sprintf(out, "pc-%.8s", id);
}

int pod_cover_cached(struct Prefs *p, const struct PodShow *s,
                     char *out, int outsize)
{
    struct SubStream st;
    char cid[16];
    char *buf;
    BPTR fh;
    long n;
    int rc;

    pod_cover_id(s->id, cid);
    cache_path(p, cid, out, outsize);
    if (cache_have(p, cid)) {
        return SUB_OK;
    }
    if (!s->image[0]) {
        out[0] = '\0';
        return pod_fail(SUB_EAPI, "podcast has no cover");
    }
    rc = cache_ensure(p);
    if (rc != SUB_OK) {
        return rc;
    }
    rc = sub_url_open(s->image, NULL, 0, &st);
    if (rc != SUB_OK) {
        out[0] = '\0';
        return rc;
    }
    fh = Open((STRPTR)out, MODE_NEWFILE);
    buf = AllocVec(8192, MEMF_ANY);
    if (!fh || !buf) {
        if (fh) Close(fh);
        if (buf) FreeVec(buf);
        sub_stream_close(&st);
        DeleteFile((STRPTR)out);
        out[0] = '\0';
        return pod_fail(SUB_EMEM, "cannot write cover into cache");
    }
    while ((n = sub_stream_read(&st, buf, 8192)) > 0) {
        Write(fh, buf, n);
    }
    Close(fh);
    FreeVec(buf);
    sub_stream_close(&st);
    if (n < 0 || (st.total > 0 && st.pos < st.total)) {
        /* Abgebrochen - keine halbe Datei im Cache lassen. */
        DeleteFile((STRPTR)out);
        out[0] = '\0';
        return pod_fail(SUB_ENET, "cover download broken off");
    }
    return SUB_OK;
}
