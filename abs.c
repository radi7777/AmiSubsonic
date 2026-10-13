/* AmiSubsonic - Anbindung an Audiobookshelf, siehe abs.h.
 *
 * Alle Pfade und Felder sind am echten Server nachgemessen
 * (Audiobookshelf 2.35.1, 12.10.2026), nicht aus der Doku geraten. */

#include <exec/types.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abs.h"
#include "json.h"

/* Der Token. Nur im Speicher: er gilt eine Stunde, danach meldet der
 * Server 401 und es wird neu angemeldet. Ihn in die Prefs zu schreiben
 * brachte nichts und legte nur einen weiteren Schluessel auf Platte. */
static char g_token[512] = "";

/* Fehlertext fuer Fehler, die nicht aus dem HTTP-Weg kommen (kaputte
 * Antwort, kein Token). Netzfehler stehen weiter in sub_last_error();
 * abs_last_error() liefert, was zuletzt gesetzt wurde, so muss der
 * Aufrufer nur an einer Stelle nachsehen. */
static char g_abs_err[160] = "";

static int abs_fail(int code, const char *msg)
{
    strncpy(g_abs_err, msg, sizeof(g_abs_err) - 1);
    g_abs_err[sizeof(g_abs_err) - 1] = '\0';
    return code;
}

const char *abs_last_error(void)
{
    return g_abs_err[0] ? g_abs_err : sub_last_error();
}

static void copy_cut(char *dst, int size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* Fuer den JSON-Rumpf der Anmeldung: " und \ maskieren. Ein Passwort
 * mit Anfuehrungszeichen wuerde sonst den Rumpf zerbrechen. */
static void json_quote(const char *s, char *out, int outsize)
{
    int o = 0;

    while (*s && o < outsize - 3) {
        if (*s == '"' || *s == '\\') {
            out[o++] = '\\';
        }
        out[o++] = *s++;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Anmeldung                                                           */
/* ------------------------------------------------------------------ */

struct LoginCtx {
    char token[512];
    char access[512];
};

static int login_cb(const struct JsonEv *ev, void *ud)
{
    struct LoginCtx *c = (struct LoginCtx *)ud;

    if (JSON_IS(ev, "user.accessToken")) {
        json_text(ev, c->access, sizeof(c->access));
    } else if (JSON_IS(ev, "user.token")) {
        json_text(ev, c->token, sizeof(c->token));
    }
    return 0;
}

int abs_login(struct Prefs *p)
{
    struct LoginCtx c;
    char user[150], pass[270];
    char body[sizeof(user) + sizeof(pass) + 40];
    char *resp = NULL;
    long len = 0;
    int rc;

    g_abs_err[0] = '\0';
    if (p->absurl[0] == '\0') {
        return abs_fail(SUB_ENOPREFS, "no Audiobookshelf server configured");
    }
    if (p->absuser[0] == '\0') {
        return abs_fail(SUB_ENOPREFS, "no Audiobookshelf user configured");
    }

    json_quote(p->absuser, user, sizeof(user));
    json_quote(p->abspass, pass, sizeof(pass));
    sprintf(body, "{\"username\":\"%s\",\"password\":\"%s\"}", user, pass);

    rc = sub_http_request(p->absurl, "POST", "/login", NULL, body,
                          &resp, &len);
    if (rc != SUB_OK) {
        if (sub_last_http_status() == 401) {
            return abs_fail(SUB_EAPI,
                            "Audiobookshelf: wrong user name or password");
        }
        return rc;
    }

    memset(&c, 0, sizeof(c));
    json_walk(resp, len, login_cb, &c);
    free(resp);

    /* Der neue accessToken (2.26 und spaeter) laeuft nach einer Stunde
     * ab, der alte token nie - Audiobookshelf will ihn aber loswerden.
     * Also den neuen nehmen, solange es ihn gibt; ein 401 meldet dann
     * einfach neu an. Aeltere Server kennen nur den alten. */
    if (c.access[0]) {
        copy_cut(g_token, sizeof(g_token), c.access);
    } else if (c.token[0]) {
        copy_cut(g_token, sizeof(g_token), c.token);
    } else {
        return abs_fail(SUB_EAPI, "Audiobookshelf: login without token");
    }
    return SUB_OK;
}

/* Eine Anfrage mit Anmeldung. Bei 401 einmal neu anmelden und wieder-
 * holen - so ueberlebt ein laufendes Programm den Ablauf des Tokens. */
static int abs_call(struct Prefs *p, const char *method, const char *path,
                    const char *body, char **resp, long *len)
{
    char hdr[600];
    int rc, attempt;

    g_abs_err[0] = '\0';
    for (attempt = 0; attempt < 2; attempt++) {
        if (g_token[0] == '\0') {
            rc = abs_login(p);
            if (rc != SUB_OK) {
                return rc;
            }
        }
        sprintf(hdr, "Authorization: Bearer %s\r\n", g_token);
        rc = sub_http_request(p->absurl, method, path, hdr, body, resp, len);
        if (rc == SUB_OK || sub_last_http_status() != 401) {
            return rc;
        }
        g_token[0] = '\0';
    }
    return rc;
}

int abs_raw(struct Prefs *p, const char *path, char **resp, long *len)
{
    return abs_call(p, "GET", path, NULL, resp, len);
}

/* ------------------------------------------------------------------ */
/* Buecher                                                             */
/* ------------------------------------------------------------------ */

#define ABS_MAX_LIBS  8
#define ABS_PAGE      50

struct LibCtx {
    char ids[ABS_MAX_LIBS][SUB_ID_LEN];
    char type[16];
    char id[SUB_ID_LEN];
    int  n;
};

static int lib_cb(const struct JsonEv *ev, void *ud)
{
    struct LibCtx *c = (struct LibCtx *)ud;

    if (ev->type == JSON_OBJ_BEGIN && JSON_IS(ev, "libraries.#")) {
        c->id[0] = '\0';
        c->type[0] = '\0';
    } else if (JSON_IS(ev, "libraries.#.id")) {
        json_text(ev, c->id, sizeof(c->id));
    } else if (JSON_IS(ev, "libraries.#.mediaType")) {
        json_text(ev, c->type, sizeof(c->type));
    } else if (ev->type == JSON_OBJ_END && JSON_IS(ev, "libraries.#")) {
        /* Podcasts sind keine Buecher - eigene Ansicht, spaeter. */
        if (c->id[0] && strcmp(c->type, "book") == 0 &&
            c->n < ABS_MAX_LIBS) {
            strcpy(c->ids[c->n++], c->id);
        }
    }
    return 0;
}

struct ItemsCtx {
    struct SubList *out;
    struct AbsBook *cur;
    long total;
};

static int items_cb(const struct JsonEv *ev, void *ud)
{
    struct ItemsCtx *c = (struct ItemsCtx *)ud;

    if (ev->type == JSON_OBJ_BEGIN && JSON_IS(ev, "results.#")) {
        c->cur = (struct AbsBook *)list_add(c->out);
        return c->cur == NULL;          /* kein Speicher: aufhoeren */
    }
    if (JSON_IS(ev, "total")) {
        c->total = json_long(ev);
        return 0;
    }
    if (!c->cur) {
        return 0;
    }
    if (JSON_IS(ev, "results.#.id")) {
        json_text(ev, c->cur->id, sizeof(c->cur->id));
    } else if (JSON_IS(ev, "results.#.media.metadata.title")) {
        json_text(ev, c->cur->title, sizeof(c->cur->title));
    } else if (JSON_IS(ev, "results.#.media.metadata.authorName")) {
        json_text(ev, c->cur->author, sizeof(c->cur->author));
    } else if (JSON_IS(ev, "results.#.media.duration")) {
        c->cur->dur_ms = json_ms(ev);
    } else if (JSON_IS(ev, "results.#.media.numTracks")) {
        c->cur->ntracks = (int)json_long(ev);
    } else if (ev->type == JSON_OBJ_END && JSON_IS(ev, "results.#")) {
        c->cur = NULL;
    }
    return 0;
}

/* /api/me traegt die Positionen ALLER Buecher (1,3 KB) - billiger als
 * je Buch eine Anfrage. */
struct ProgCtx {
    struct SubList *books;
    char item[SUB_ID_LEN];
    long pos;
    BOOL fin;
};

static int prog_cb(const struct JsonEv *ev, void *ud)
{
    struct ProgCtx *c = (struct ProgCtx *)ud;

    if (ev->type == JSON_OBJ_BEGIN && JSON_IS(ev, "mediaProgress.#")) {
        c->item[0] = '\0';
        c->pos = 0;
        c->fin = FALSE;
    } else if (JSON_IS(ev, "mediaProgress.#.libraryItemId")) {
        json_text(ev, c->item, sizeof(c->item));
    } else if (JSON_IS(ev, "mediaProgress.#.currentTime")) {
        c->pos = json_ms(ev);
    } else if (JSON_IS(ev, "mediaProgress.#.isFinished")) {
        c->fin = (ev->len == 4 && strncmp(ev->val, "true", 4) == 0);
    } else if (ev->type == JSON_OBJ_END && JSON_IS(ev, "mediaProgress.#")) {
        int i;

        for (i = 0; i < c->books->count; i++) {
            struct AbsBook *b = (struct AbsBook *)list_get(c->books, i);

            if (strcmp(b->id, c->item) == 0) {
                b->pos_ms = c->pos;
                b->finished = c->fin;
                break;
            }
        }
    }
    return 0;
}

int abs_books(struct Prefs *p, struct SubList *out)
{
    struct LibCtx lc;
    struct ItemsCtx ic;
    struct ProgCtx pc;
    char path[200];
    char *resp = NULL;
    long len = 0;
    int rc, l, page;

    memset(&lc, 0, sizeof(lc));
    rc = abs_call(p, "GET", "/api/libraries", NULL, &resp, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    json_walk(resp, len, lib_cb, &lc);
    free(resp);

    for (l = 0; l < lc.n; l++) {
        int first = out->count;

        /* Seitenweise wie bei Navidrome: eine Bibliothek kann gross
         * sein, und minified spart je Buch die Beschreibung. */
        for (page = 0; page < 200; page++) {
            int before = out->count;

            sprintf(path, "/api/libraries/%s/items?minified=1&limit=%d"
                          "&page=%d&sort=media.metadata.title",
                    lc.ids[l], ABS_PAGE, page);
            rc = abs_call(p, "GET", path, NULL, &resp, &len);
            if (rc != SUB_OK) {
                return rc;
            }
            ic.out = out;
            ic.cur = NULL;
            ic.total = 0;
            if (json_walk(resp, len, items_cb, &ic) < 0) {
                free(resp);
                return abs_fail(SUB_EAPI, "Audiobookshelf: unreadable book list");
            }
            free(resp);
            if (out->count == before || out->count - first >= ic.total) {
                break;
            }
        }
    }

    rc = abs_call(p, "GET", "/api/me", NULL, &resp, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    pc.books = out;
    json_walk(resp, len, prog_cb, &pc);
    free(resp);
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Dateien eines Buchs                                                 */
/* ------------------------------------------------------------------ */

/* Gelesen wird media.audioFiles. media.tracks mit fertigem startOffset
 * gibt es nur mit ?expanded=1, und dann ist die Antwort 340 statt 194 KB
 * gross (gemessen 12.10.2026 - der erste Anlauf hatte auf dem Mac gegen
 * die erweiterte Antwort getestet und fand auf dem Amiga nichts).
 *
 * Die Startzeiten rechnen wir selbst: Summe der Dauern davor, in der
 * Reihenfolge von "index", ausgeschlossene Dateien nicht mitgezaehlt.
 * Gegen die startOffsets des Servers nachgerechnet: alle 132 gleich. */
struct TrackCtx {
    struct SubList *out;
    struct AbsTrack *cur;
    long dur;
};

static int tracks_cb(const struct JsonEv *ev, void *ud)
{
    struct TrackCtx *c = (struct TrackCtx *)ud;

    if (ev->type == JSON_OBJ_BEGIN && JSON_IS(ev, "media.audioFiles.#")) {
        c->cur = (struct AbsTrack *)list_add(c->out);
        return c->cur == NULL;
    }
    if (JSON_IS(ev, "media.duration")) {
        c->dur = json_ms(ev);
        return 0;
    }
    if (!c->cur) {
        return 0;
    }
    if (JSON_IS(ev, "media.audioFiles.#.ino")) {
        json_text(ev, c->cur->ino, sizeof(c->cur->ino));
    } else if (JSON_IS(ev, "media.audioFiles.#.index")) {
        c->cur->index = (int)json_long(ev);
    } else if (JSON_IS(ev, "media.audioFiles.#.exclude")) {
        c->cur->exclude = (ev->len == 4 && strncmp(ev->val, "true", 4) == 0);
    } else if (JSON_IS(ev, "media.audioFiles.#.metadata.filename")) {
        json_text(ev, c->cur->title, sizeof(c->cur->title));
    } else if (JSON_IS(ev, "media.audioFiles.#.duration")) {
        c->cur->dur_ms = json_ms(ev);
    } else if (JSON_IS(ev, "media.audioFiles.#.bitRate")) {
        c->cur->bitrate = json_long(ev);
    } else if (JSON_IS(ev, "media.audioFiles.#.metadata.size")) {
        c->cur->size = json_long(ev);
    } else if (ev->type == JSON_OBJ_END && JSON_IS(ev, "media.audioFiles.#")) {
        c->cur = NULL;
    }
    return 0;
}

/* Ausgeschlossene heraus, nach index ordnen, Startzeiten aufsummieren.
 * Einfuegesortierung: der Server liefert sie ohnehin geordnet, dann ist
 * das ein einziger Durchgang. */
static long tracks_finish(struct SubList *l)
{
    struct AbsTrack tmp;
    int i, j, n = 0;
    long start = 0;

    for (i = 0; i < l->count; i++) {
        struct AbsTrack *t = (struct AbsTrack *)list_get(l, i);

        if (!t->exclude) {
            if (n != i) {
                *(struct AbsTrack *)list_get(l, n) = *t;
            }
            n++;
        }
    }
    l->count = n;

    for (i = 1; i < n; i++) {
        tmp = *(struct AbsTrack *)list_get(l, i);
        for (j = i - 1; j >= 0 &&
             ((struct AbsTrack *)list_get(l, j))->index > tmp.index; j--) {
            *(struct AbsTrack *)list_get(l, j + 1) =
                *(struct AbsTrack *)list_get(l, j);
        }
        *(struct AbsTrack *)list_get(l, j + 1) = tmp;
    }

    for (i = 0; i < n; i++) {
        struct AbsTrack *t = (struct AbsTrack *)list_get(l, i);

        t->start_ms = start;
        start += t->dur_ms;
    }
    return start;
}

int abs_tracks(struct Prefs *p, const char *itemid, struct SubList *out,
               long *dur_ms)
{
    struct TrackCtx c;
    char path[120];
    char *resp = NULL;
    long len = 0;
    int rc;

    sprintf(path, "/api/items/%.40s", itemid);
    rc = abs_call(p, "GET", path, NULL, &resp, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    c.out = out;
    c.cur = NULL;
    c.dur = 0;
    rc = json_walk(resp, len, tracks_cb, &c);
    free(resp);
    if (rc < 0) {
        return abs_fail(SUB_EAPI, "Audiobookshelf: unreadable book details");
    }
    /* media.duration fehlt in dieser Antwort - dann die Summe. */
    if (c.dur == 0) {
        c.dur = tracks_finish(out);
    } else {
        tracks_finish(out);
    }
    if (out->count == 0) {
        return abs_fail(SUB_EAPI, "Audiobookshelf: book has no audio files");
    }
    if (dur_ms) {
        *dur_ms = c.dur;
    }
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Hoerposition                                                        */
/* ------------------------------------------------------------------ */

struct PosCtx {
    long pos;
    BOOL fin;
};

static int pos_cb(const struct JsonEv *ev, void *ud)
{
    struct PosCtx *c = (struct PosCtx *)ud;

    if (JSON_IS(ev, "currentTime")) {
        c->pos = json_ms(ev);
    } else if (JSON_IS(ev, "isFinished")) {
        c->fin = (ev->len == 4 && strncmp(ev->val, "true", 4) == 0);
    }
    return 0;
}

int abs_get_pos(struct Prefs *p, const char *itemid, long *pos_ms,
                BOOL *finished)
{
    struct PosCtx c;
    char path[120];
    char *resp = NULL;
    long len = 0;
    int rc;

    c.pos = 0;
    c.fin = FALSE;
    sprintf(path, "/api/me/progress/%.40s", itemid);
    rc = abs_call(p, "GET", path, NULL, &resp, &len);
    if (rc != SUB_OK) {
        /* 404 heisst: dieses Buch hat der Benutzer noch nie angefangen. */
        if (sub_last_http_status() == 404) {
            *pos_ms = 0;
            if (finished) {
                *finished = FALSE;
            }
            return SUB_OK;
        }
        return rc;
    }
    json_walk(resp, len, pos_cb, &c);
    free(resp);
    *pos_ms = c.pos;
    if (finished) {
        *finished = c.fin;
    }
    return SUB_OK;
}

int abs_set_pos(struct Prefs *p, const char *itemid, long pos_ms,
                long dur_ms)
{
    char path[120];
    char body[160];
    char frac[8];
    char *resp = NULL;
    long len = 0;
    long r;
    int rc, k;

    if (pos_ms < 0) {
        pos_ms = 0;
    }
    if (dur_ms > 0 && pos_ms > dur_ms) {
        pos_ms = dur_ms;
    }

    /* Der Anteil (0..1) mit sechs Stellen, als schriftliche Division:
     * pos*1000000 passte bei einem elfstuendigen Buch (4e7 ms) nicht in
     * 32 Bit, und Fliesskomma gibt es hier nicht. Der Rest bleibt stets
     * kleiner als dur, mal zehn passt das bis 59 Stunden Spieldauer. */
    strcpy(frac, "000000");
    if (dur_ms > 0) {
        r = pos_ms;
        if (r >= dur_ms) {
            strcpy(frac, "999999");
        } else {
            for (k = 0; k < 6; k++) {
                r *= 10;
                frac[k] = (char)('0' + r / dur_ms);
                r %= dur_ms;
            }
        }
    }

    sprintf(body, "{\"currentTime\":%ld.%03ld,\"duration\":%ld.%03ld,"
                  "\"progress\":0.%s}",
            pos_ms / 1000, pos_ms % 1000, dur_ms / 1000, dur_ms % 1000,
            frac);
    sprintf(path, "/api/me/progress/%.40s", itemid);
    rc = abs_call(p, "PATCH", path, body, &resp, &len);
    if (resp) {
        free(resp);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Dateien abspielen                                                   */
/* ------------------------------------------------------------------ */

int abs_open_file(struct Prefs *p, const char *itemid, const char *ino,
                  long offset, struct SubStream *st)
{
    char path[700];
    int rc, attempt;

    /* Der Token geht im Query mit statt als Kopfzeile: so bleibt der
     * Stromweg derselbe wie beim Radio, und Audiobookshelf nimmt ihn
     * dort an (gemessen, HTTP/1.0, 206). */
    for (attempt = 0; attempt < 2; attempt++) {
        if (g_token[0] == '\0') {
            rc = abs_login(p);
            if (rc != SUB_OK) {
                return rc;
            }
        }
        sprintf(path, "/api/items/%.40s/file/%.15s?token=%s",
                itemid, ino, g_token);
        rc = sub_url_stream_open(p->absurl, path, offset, st);
        if (rc == SUB_OK || sub_last_http_status() != 401) {
            return rc;
        }
        g_token[0] = '\0';
    }
    return rc;
}

int abs_find_track(struct SubList *tracks, long pos_ms, long *in_ms)
{
    int i;

    for (i = 0; i < tracks->count; i++) {
        struct AbsTrack *t = (struct AbsTrack *)list_get(tracks, i);

        if (pos_ms < t->start_ms + t->dur_ms) {
            *in_ms = pos_ms > t->start_ms ? pos_ms - t->start_ms : 0;
            return i;
        }
    }
    *in_ms = 0;
    return tracks->count > 0 ? tracks->count - 1 : 0;
}

long abs_byte_for_ms(const struct AbsTrack *t, long in_ms)
{
    long kbps = t->bitrate / 1000;

    if (kbps <= 0 && t->dur_ms > 0) {
        /* Ohne Bitrate aus der Groesse schaetzen. */
        kbps = (t->size / (t->dur_ms / 1000 + 1)) / 125;
    }
    /* kbit/s * ms / 8 = Bytes. in_ms * kbps passt bis 4,6e8 (eine
     * Stunde bei 128 kbps) locker in 32 Bit. */
    return in_ms * kbps / 8;
}

/* ------------------------------------------------------------------ */
/* Cover                                                               */
/* ------------------------------------------------------------------ */

void abs_cover_id(const char *itemid, char *out)
{
    /* "ab-" und nicht "abs-": mit der 36 Zeichen langen UUID passen
     * genau 39 Zeichen plus Nullbyte in SUB_ID_LEN. */
    sprintf(out, "ab-%.36s", itemid);
}

int abs_cover_cached(struct Prefs *p, const char *coverid, int size,
                     char *out, int outsize)
{
    char path[120];
    char *resp = NULL;
    long len = 0;
    BPTR fh;
    int rc;

    if (strncmp(coverid, "ab-", 3) != 0) {
        return abs_fail(SUB_EAPI, "not an Audiobookshelf cover");
    }
    cache_path(p, coverid, out, outsize);
    if (out[0] == '\0') {
        return abs_fail(SUB_EMEM, "cache path too long");
    }
    if (cache_have(p, coverid) || cache_from_jpg(out)) {
        return SUB_OK;
    }
    rc = cache_ensure(p);
    if (rc != SUB_OK) {
        return rc;
    }

    /* format=jpeg: von sich aus liefert Audiobookshelf WebP (gemessen),
     * und dafuer hat kaum ein Amiga einen Datatype. */
    sprintf(path, "/api/items/%.40s/cover?width=%d&format=jpeg",
            coverid + 3, size > 0 ? size : COVER_PX);
    rc = abs_call(p, "GET", path, NULL, &resp, &len);
    if (rc != SUB_OK) {
        out[0] = '\0';
        return rc;
    }
    fh = Open((STRPTR)out, MODE_NEWFILE);
    if (!fh) {
        free(resp);
        out[0] = '\0';
        return abs_fail(SUB_EMEM, "cannot write cover into cache");
    }
    if (Write(fh, resp, len) != len) {
        /* Halbe Datei weg - sie laege sonst fuer immer im Cache. */
        Close(fh);
        DeleteFile((STRPTR)out);
        free(resp);
        out[0] = '\0';
        return abs_fail(SUB_EMEM, "cover write failed");
    }
    Close(fh);
    free(resp);
    rc = cache_finish(out);
    if (rc != SUB_OK) {
        out[0] = '\0';
    }
    return rc;
}
