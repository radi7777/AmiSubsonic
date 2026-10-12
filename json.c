/* AmiSubsonic - kleiner JSON-Leser, siehe json.h.
 *
 * Rekursiver Abstieg. Die Tiefe ist durch den Pfadpuffer begrenzt
 * (JSON_DEPTH), damit eine kaputte oder boesartige Antwort den Stapel
 * nicht sprengt - Audiobookshelf kommt mit sechs Ebenen aus. */

#include <string.h>
#include <ctype.h>

#include "json.h"
#include "amisub.h"             /* utf8_to_latin1 */

#define JSON_PATH   256
#define JSON_DEPTH  24

struct JCtx {
    const char *p;
    const char *end;
    JsonCb      cb;
    void       *ud;
    int         stop;           /* der Aufrufer will nicht mehr */
    int         depth;
    int         plen;
    char        path[JSON_PATH];
};

static void skip_ws(struct JCtx *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static int emit(struct JCtx *c, int type, const char *val, int len, int index)
{
    struct JsonEv ev;

    if (c->stop) {
        return 1;
    }
    ev.type  = type;
    ev.path  = c->path;
    ev.val   = val;
    ev.len   = len;
    ev.index = index;
    if (c->cb(&ev, c->ud)) {
        c->stop = 1;
    }
    return c->stop;
}

/* Haengt ".name" (oder "name" an der Wurzel) an den Pfad. Liefert die
 * alte Laenge zum Zuruecksetzen, -1 wenn der Puffer nicht reicht. */
static int path_push(struct JCtx *c, const char *name, int nlen)
{
    int old = c->plen;
    int need = nlen + (old ? 1 : 0);

    if (old + need >= JSON_PATH) {
        return -1;
    }
    if (old) {
        c->path[c->plen++] = '.';
    }
    memcpy(c->path + c->plen, name, nlen);
    c->plen += nlen;
    c->path[c->plen] = '\0';
    return old;
}

static void path_pop(struct JCtx *c, int old)
{
    c->plen = old;
    c->path[old] = '\0';
}

/* Zeigt nach dem Aufruf hinter das schliessende '"'. Liefert Anfang und
 * Laenge des rohen Inhalts. Maskierte Anfuehrungszeichen (\") beenden
 * die Zeichenkette nicht - deshalb wird jedes '\\' samt Folgezeichen
 * uebersprungen. */
static int scan_string(struct JCtx *c, const char **s, int *len)
{
    const char *start;

    c->p++;                     /* das oeffnende '"' */
    start = c->p;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\') {
            c->p++;
        }
        c->p++;
    }
    if (c->p >= c->end) {
        return -1;
    }
    *s = start;
    *len = (int)(c->p - start);
    c->p++;
    return 0;
}

static int parse_value(struct JCtx *c, int index);

static int parse_object(struct JCtx *c, int index)
{
    if (emit(c, JSON_OBJ_BEGIN, c->p, 1, index)) {
        return 0;
    }
    c->p++;
    for (;;) {
        const char *key;
        int klen, old;

        skip_ws(c);
        if (c->p >= c->end) {
            return -1;
        }
        if (*c->p == '}') {
            c->p++;
            break;
        }
        if (*c->p != '"' || scan_string(c, &key, &klen) < 0) {
            return -1;
        }
        /* Schluessel mit Maskierungen gibt es bei Audiobookshelf nicht;
         * sie gehen roh in den Pfad und passen dann eben auf nichts. */
        old = path_push(c, key, klen);
        if (old < 0) {
            return -1;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            return -1;
        }
        c->p++;
        if (parse_value(c, -1) < 0) {
            return -1;
        }
        path_pop(c, old);
        if (c->stop) {
            return 0;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            break;
        }
        return -1;
    }
    emit(c, JSON_OBJ_END, c->p - 1, 1, index);
    return 0;
}

static int parse_array(struct JCtx *c, int index)
{
    int old, i = 0;

    if (emit(c, JSON_ARR_BEGIN, c->p, 1, index)) {
        return 0;
    }
    c->p++;
    old = path_push(c, "#", 1);
    if (old < 0) {
        return -1;
    }
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end) {
            return -1;
        }
        if (*c->p == ']') {
            c->p++;
            break;
        }
        if (parse_value(c, i++) < 0) {
            return -1;
        }
        if (c->stop) {
            return 0;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            break;
        }
        return -1;
    }
    path_pop(c, old);
    emit(c, JSON_ARR_END, c->p - 1, 1, index);
    return 0;
}

static int parse_value(struct JCtx *c, int index)
{
    const char *s;
    int len, rc;

    skip_ws(c);
    if (c->p >= c->end) {
        return -1;
    }
    if (++c->depth > JSON_DEPTH) {
        return -1;
    }

    switch (*c->p) {
    case '{':
        rc = parse_object(c, index);
        break;
    case '[':
        rc = parse_array(c, index);
        break;
    case '"':
        rc = scan_string(c, &s, &len);
        if (rc == 0) {
            emit(c, JSON_STR, s, len, index);
        }
        break;
    default:
        /* Zahl oder true/false/null: alles bis zum naechsten Trenner. */
        s = c->p;
        while (c->p < c->end && *c->p != ',' && *c->p != '}' &&
               *c->p != ']' && *c->p != ' ' && *c->p != '\n' &&
               *c->p != '\r' && *c->p != '\t') {
            c->p++;
        }
        len = (int)(c->p - s);
        if (len == 0) {
            rc = -1;
            break;
        }
        emit(c, (*s == '-' || isdigit((unsigned char)*s)) ? JSON_NUM : JSON_LIT,
             s, len, index);
        rc = 0;
        break;
    }
    c->depth--;
    return rc;
}

int json_walk(const char *text, long len, JsonCb cb, void *ud)
{
    struct JCtx c;

    c.p = text;
    c.end = text + len;
    c.cb = cb;
    c.ud = ud;
    c.stop = 0;
    c.depth = 0;
    c.plen = 0;
    c.path[0] = '\0';

    return parse_value(&c, -1) < 0 ? -1 : 0;
}

/* Wie append_utf8 in amisub.c - die Maskierungen werden nach UTF-8
 * aufgeloest und erst am Ende in einem Rutsch nach Latin-1 gewandelt,
 * damit es dafuer nur eine Stelle gibt (utf8_to_latin1). */
static int put_utf8(char *out, int o, int outsize, unsigned long cp)
{
    if (cp < 0x80) {
        if (o + 1 < outsize) {
            out[o++] = (char)cp;
        }
    } else if (cp < 0x800) {
        if (o + 2 < outsize) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    } else if (o + 3 < outsize) {
        out[o++] = (char)(0xe0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[o++] = (char)(0x80 | (cp & 0x3f));
    }
    return o;
}

void json_text(const struct JsonEv *ev, char *out, int outsize)
{
    int i = 0, o = 0;

    out[0] = '\0';
    if (ev->type != JSON_STR) {
        /* Eine Zahl als Text ist brauchbar (Audiobookshelf schreibt
         * "ino" mal so, mal so), null dagegen heisst: nichts. */
        if (ev->type == JSON_NUM) {
            int n = ev->len < outsize - 1 ? ev->len : outsize - 1;
            memcpy(out, ev->val, n);
            out[n] = '\0';
        }
        return;
    }

    while (i < ev->len && o < outsize - 1) {
        char ch = ev->val[i++];

        if (ch != '\\' || i >= ev->len) {
            out[o++] = ch;
            continue;
        }
        ch = ev->val[i++];
        switch (ch) {
        case 'n': case 't':
            out[o++] = ' ';     /* Titelzeilen haben keinen Umbruch */
            break;
        case 'r': case 'b': case 'f':
            break;
        case 'u': {
            unsigned long cp = 0;
            int k;

            for (k = 0; k < 4 && i < ev->len &&
                        isxdigit((unsigned char)ev->val[i]); k++, i++) {
                char h = ev->val[i];
                cp = cp * 16 + (unsigned long)
                     (h <= '9' ? h - '0' : (tolower(h) - 'a' + 10));
            }
            o = put_utf8(out, o, outsize, cp);
            break;
        }
        default:
            out[o++] = ch;      /* \" \\ \/ */
            break;
        }
    }
    out[o] = '\0';
    utf8_to_latin1(out);
}

long json_long(const struct JsonEv *ev)
{
    long v = 0;
    int i = 0, neg = 0;

    if (ev->type == JSON_STR || ev->type == JSON_NUM) {
        if (i < ev->len && ev->val[i] == '-') {
            neg = 1;
            i++;
        }
        while (i < ev->len && isdigit((unsigned char)ev->val[i])) {
            v = v * 10 + (ev->val[i++] - '0');
        }
    }
    return neg ? -v : v;
}

long json_ms(const struct JsonEv *ev)
{
    long sec = 0, frac = 0;
    int i = 0, k, neg = 0;

    if (ev->type != JSON_NUM && ev->type != JSON_STR) {
        return 0;
    }
    if (i < ev->len && ev->val[i] == '-') {
        neg = 1;
        i++;
    }
    while (i < ev->len && isdigit((unsigned char)ev->val[i])) {
        sec = sec * 10 + (ev->val[i++] - '0');
    }
    if (i < ev->len && ev->val[i] == '.') {
        i++;
        for (k = 0; k < 3; k++) {
            frac *= 10;
            if (i < ev->len && isdigit((unsigned char)ev->val[i])) {
                frac += ev->val[i++] - '0';
            }
        }
    }
    sec = sec * 1000 + frac;
    return neg ? -sec : sec;
}
