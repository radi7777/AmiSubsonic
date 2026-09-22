/* AmiSubsonic - selbst gezeichnete Liste mit Farbverlauf-Hintergrund. */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <intuition/intuition.h>
#include <libraries/mui.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracklist.h"

extern struct Library *CyberGfxBase;     /* gehoert panel.c */

static struct MUI_CustomClass *g_mcc = NULL;

/* Obergrenze fuer den vorberechneten Verlauf, wie in panel.c. */
#define TL_MAXBUF  (1024L * 1024L)

/* Waagerechte Raender und der Abstand zwischen den Zeilen. */
#define PAD_X   6
#define PAD_Y   2

/* Rollleiste am rechten Rand, wie bei der Bildwand - gleiche Masse,
 * gleiches Verhalten, damit sich beide Listen gleich anfuehlen. */
#define TL_SBW    12
#define TL_SBMIN  24

/* Kantenlaenge des kleinen Covers in TLK_TRACKS. So gross, dass man das
 * Album erkennt, und so klein, dass noch rund fuenfzehn Zeilen ins
 * Fenster passen. Das Mass steht im Header - der Auftraggeber rechnet
 * die Miniaturen genau darauf herunter. */
#define TL_THUMB     TL_THUMB_SIZE

/* Luft ueber und unter der Ueberschrift zusammen. */
#define TL_HEAD_PAD  14

struct TLData {
    int    kind;
    ULONG  colour;

    struct SubList *list;       /* fremde Daten, nicht kopiert */
    char           *text;       /* dito, fuer TLK_TEXT */

    LONG   active;              /* gewaehlte Zeile, -1 = keine */
    LONG   playing;             /* laufender Titel, -1 = keiner */
    LONG   first;               /* oberste sichtbare Zeile */

    ULONG *buf;                 /* vorgerechneter Verlauf */
    LONG   buf_w, buf_h;
    ULONG  buf_col;

    /* Der Eintrag in der Ereigniskette des Fensters. Muss die ganze
     * Zeit ueber leben, MUI verkettet ihn nur - deshalb hier in den
     * Instanzdaten und nicht als Ortsvariable. */
    struct MUI_EventHandlerNode ehn;

    /* Fuer die Doppelklickerkennung. */
    ULONG  last_secs, last_micros;
    LONG   last_row;

    /* Ziehen am Schieber der Rollleiste. */
    BOOL   drag;
    LONG   dragoff;

    /* Nur TLK_TRACKS: je Zeile ein Zeiger auf die Miniatur ihres
     * Albums. Gehoert dem Auftraggeber, wird nur angesehen. */
    struct AlbumThumb **thumbs;

    /* Ueberschrift, kopiert. Leer heisst: kein Kopfstreifen. */
    char   title[64];
};

struct MUI_CustomClass *tl_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */
/* Zeilen                                                              */
/* ------------------------------------------------------------------ */

static LONG count_rows(struct TLData *d)
{
    if (d->kind == TLK_TEXT) {
        LONG n = 0;
        const char *p = d->text;

        if (!p || !*p) {
            return 0;
        }
        while (*p) {
            if (*p++ == '\n') {
                n++;
            }
        }
        /* Eine letzte Zeile ohne abschliessenden Umbruch zaehlt mit. */
        if (p > d->text && p[-1] != '\n') {
            n++;
        }
        return n;
    }
    return d->list ? d->list->count : 0;
}

/* Baut die drei Spalten einer Zeile.
 *
 * Drei getrennte Zeichenketten statt einer mit Tabulatoren: die mittlere
 * wird bei Bedarf beschnitten, die aeussere rechts bleibt vollstaendig.
 * Mit einer einzigen Zeichenkette muesste man beim Kuerzen raten, wo
 * gekuerzt werden darf. */
static void row_text(struct TLData *d, LONG i,
                     char *left, int lsize,
                     char *mid, int msize,
                     char *right, int rsize)
{
    left[0] = mid[0] = right[0] = '\0';

    if (d->kind == TLK_SONGS || d->kind == TLK_TRACKS) {
        struct Song *s = (struct Song *)list_get(d->list, (int)i);

        if (!s) {
            return;
        }
        if (d->kind == TLK_SONGS) {
            sprintf(left, "%d", s->track);
        }
        strncpy(mid, s->title, msize - 1);
        mid[msize - 1] = '\0';
        duration_text(s->duration, right, rsize);
    } else if (d->kind == TLK_RADIO) {
        struct Radio *r = (struct Radio *)list_get(d->list, (int)i);

        if (!r) {
            return;
        }
        strncpy(mid, r->name, msize - 1);
        mid[msize - 1] = '\0';
    } else if (d->kind == TLK_ALBUMS) {
        struct Album *a = (struct Album *)list_get(d->list, (int)i);

        if (!a) {
            return;
        }
        strncpy(left, a->artist, lsize - 1);
        left[lsize - 1] = '\0';
        strncpy(mid, a->name, msize - 1);
        mid[msize - 1] = '\0';
        if (a->year > 0) {
            sprintf(right, "%d", a->year);
        }
    } else {
        const char *p = d->text;
        LONG n = 0;
        const char *end;

        if (!p) {
            return;
        }
        while (n < i && *p) {
            if (*p++ == '\n') {
                n++;
            }
        }
        end = strchr(p, '\n');
        {
            int len = end ? (int)(end - p) : (int)strlen(p);
            if (len > msize - 1) {
                len = msize - 1;
            }
            memcpy(mid, p, (size_t)len);
            mid[len] = '\0';
        }
    }
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static BOOL build_gradient(struct TLData *d, LONG w, LONG h)
{
    LONG r0, g0, b0, y, x;
    ULONG *p;

    if (d->buf && d->buf_w == w && d->buf_h == h && d->buf_col == d->colour) {
        return TRUE;
    }
    if ((long)w * (long)h * (long)sizeof(ULONG) > TL_MAXBUF) {
        return FALSE;
    }
    if (!d->buf || d->buf_w != w || d->buf_h != h) {
        ULONG *nb = realloc(d->buf, (size_t)w * (size_t)h * sizeof(ULONG));
        if (!nb) {
            return FALSE;
        }
        d->buf = nb;
        d->buf_w = w;
        d->buf_h = h;
    }

    r0 = (d->colour >> 16) & 0xff;
    g0 = (d->colour >>  8) & 0xff;
    b0 =  d->colour        & 0xff;

    p = d->buf;
    for (y = 0; y < h; y++) {
        LONG f = (h > 1) ? (h - 1 - y) : 1;
        LONG n = (h > 1) ? (h - 1) : 1;
        ULONG v = ((ULONG)(r0 * f / n) << 16)
                | ((ULONG)(g0 * f / n) <<  8)
                |  (ULONG)(b0 * f / n);

        for (x = 0; x < w; x++) {
            *p++ = v;
        }
    }
    d->buf_col = d->colour;
    return TRUE;
}

static void draw_gradient(Object *obj, struct TLData *d)
{
    struct RastPort *rp = _rp(obj);
    LONG l = _mleft(obj), t = _mtop(obj);
    LONG w = _mwidth(obj), h = _mheight(obj);

    if (w <= 0 || h <= 0 || !CyberGfxBase) {
        return;
    }

    if (build_gradient(d, w, h)) {
        WritePixelArray(d->buf, 0, 0, (UWORD)(w * 4), rp,
                        (UWORD)l, (UWORD)t, (UWORD)w, (UWORD)h,
                        RECTFMT_ARGB);
        return;
    }

    {
        LONG r0 = (d->colour >> 16) & 0xff, g0 = (d->colour >> 8) & 0xff;
        LONG b0 = d->colour & 0xff, y;

        for (y = 0; y < h; y++) {
            LONG f = (h > 1) ? (h - 1 - y) : 1;
            LONG n = (h > 1) ? (h - 1) : 1;

            FillPixelArray(rp, (UWORD)l, (UWORD)(t + y), (UWORD)w, 1,
                           ((ULONG)(r0 * f / n) << 16)
                         | ((ULONG)(g0 * f / n) <<  8)
                         |  (ULONG)(b0 * f / n));
        }
    }
}

static LONG text_height(Object *obj)
{
    struct TextFont *font = _font(obj);

    return font ? font->tf_YSize : 8;
}

/* Hoehe des Kopfstreifens. Er rollt NICHT mit, deshalb rechnen alle
 * Zeilen- und Trefferrechnungen ab list_top() statt ab _mtop(). */
static LONG head_height(Object *obj, struct TLData *d)
{
    if (!d->title[0]) {
        return 0;
    }
    return text_height(obj) + TL_HEAD_PAD;
}

static LONG list_top(Object *obj, struct TLData *d)
{
    return _mtop(obj) + head_height(obj, d);
}

static LONG list_height(Object *obj, struct TLData *d)
{
    LONG h = _mheight(obj) - head_height(obj, d);

    return h > 0 ? h : 1;
}

static LONG row_height_kind(Object *obj, struct TLData *d)
{
    LONG th = text_height(obj) + PAD_Y;

    /* Bei TLK_TRACKS gibt das Cover die Hoehe vor, sobald es hoeher ist
     * als eine Textzeile. */
    if ((d->kind == TLK_TRACKS || d->kind == TLK_RADIO)
            && TL_THUMB + 4 > th) {
        return TL_THUMB + 4;
    }
    return th;
}

/* Die Breite, die dem Text bleibt - die Rollleiste geht ab. */
static LONG content_width(Object *obj)
{
    LONG w = _mwidth(obj) - TL_SBW;

    return w > 0 ? w : 1;
}

static LONG visible_rows(Object *obj, struct TLData *d)
{
    LONG vis = list_height(obj, d) / row_height_kind(obj, d);

    return vis > 0 ? vis : 1;
}

/* Lage des Schiebers. FALSE, wenn alles hineinpasst. */
static BOOL thumb_rect(Object *obj, struct TLData *d, LONG *ty, LONG *th)
{
    LONG h    = list_height(obj, d);
    LONG rows = count_rows(d);
    LONG vis  = visible_rows(obj, d);
    LONG hh, yy;

    if (rows <= vis || h <= 0) {
        return FALSE;
    }

    hh = h * vis / rows;
    if (hh < TL_SBMIN) {
        hh = TL_SBMIN;
    }
    /* Ueber die Reststrecke, nicht ueber die ganze Hoehe - sonst stuende
     * der Schieber bei der letzten Zeile unten ueber. */
    yy = (h - hh) * d->first / (rows - vis);

    *ty = list_top(obj, d) + yy;
    *th = hh;
    return TRUE;
}

static void draw_scrollbar(Object *obj, struct TLData *d)
{
    struct RastPort *rp = _rp(obj);
    LONG x = _mright(obj) - TL_SBW + 1;
    LONG t = list_top(obj, d), h = list_height(obj, d);
    LONG ty, th;

    if (!CyberGfxBase || h <= 0) {
        return;
    }

    FillPixelArray(rp, (UWORD)x, (UWORD)t, TL_SBW, (UWORD)h, 0x00100810);

    if (!thumb_rect(obj, d, &ty, &th)) {
        return;
    }

    {
        LONG r0 = ((d->colour >> 16) & 0xff) + 70;
        LONG g0 = ((d->colour >>  8) & 0xff) + 70;
        LONG b0 = ( d->colour        & 0xff) + 70;

        if (r0 > 255) { r0 = 255; }
        if (g0 > 255) { g0 = 255; }
        if (b0 > 255) { b0 = 255; }

        FillPixelArray(rp, (UWORD)(x + 2), (UWORD)ty,
                       TL_SBW - 4, (UWORD)th,
                       ((ULONG)r0 << 16) | ((ULONG)g0 << 8) | (ULONG)b0);
    }
}

static void draw_cell(Object *obj, const char *s, LONG x, LONG y,
                      LONG maxw, ULONG pen, BOOL rightalign)
{
    struct RastPort *rp = _rp(obj);
    struct TextFont *font = _font(obj);
    LONG len = (LONG)strlen(s);
    struct TextExtent te;
    ULONG fit;

    if (!s[0] || !font || maxw <= 0) {
        return;
    }

    SetAPen(rp, pen);
    fit = TextFit(rp, (STRPTR)s, (ULONG)len, &te, NULL, 1,
                  (UWORD)maxw, (UWORD)font->tf_YSize);
    if (fit < (ULONG)len) {
        len = (LONG)fit;
    }
    if (len <= 0) {
        return;
    }
    if (rightalign) {
        x = x + maxw - TextLength(rp, (STRPTR)s, (ULONG)len);
    }
    Move(rp, (WORD)x, (WORD)(y + font->tf_Baseline));
    Text(rp, (STRPTR)s, (ULONG)len);
}

/* Das kleine Albumcover einer Zeile. Es gehoert dem Auftraggeber und
 * wird nur angesehen. Fehlt es noch, bleibt der Platz LEER: die Bilder
 * trudeln nach und nach ein, und ein Platzhalterrahmen, der gleich
 * wieder verschwindet, ist unruhiger als eine Luecke. */
static void draw_thumb(Object *obj, struct TLData *d, LONG row,
                       LONG x, LONG y, LONG rh)
{
    struct AlbumThumb *th;
    LONG ox, oy;

    if (!d->thumbs || !CyberGfxBase) {
        return;
    }
    th = d->thumbs[row];
    if (!th || !th->rgb || th->w <= 0 || th->h <= 0) {
        return;
    }

    ox = x + (TL_THUMB - th->w) / 2;
    oy = y + (rh - th->h) / 2;
    WritePixelArray((APTR)th->rgb, 0, 0, (UWORD)(th->w * 3), _rp(obj),
                    (UWORD)ox, (UWORD)oy, (UWORD)th->w, (UWORD)th->h,
                    RECTFMT_RGB);
}

/* Das Sendersymbol fuer TLK_RADIO: eine Antenne mit zwei Wellen.
 * Selbst gezeichnet aus Linien - ein Bild waere eine Datei mehr, und
 * ein Zeichen aus dem Systemfont haette in jeder Schrift anders
 * ausgesehen. Gerechnet wird in Achteln der Zellenhoehe, damit es bei
 * einer groesseren Schrift mitwaechst. */
static void draw_antenna(Object *obj, LONG x, LONG y, LONG rh, ULONG pen)
{
    struct RastPort *rp = _rp(obj);
    LONG s  = (TL_THUMB < rh ? TL_THUMB : rh) - 6;
    LONG cx, base, i;

    if (s < 8) {
        return;
    }
    cx   = x + TL_THUMB / 2;
    base = y + (rh + s) / 2;

    SetAPen(rp, pen);

    /* Der Mast. */
    Move(rp, (WORD)cx, (WORD)base);
    Draw(rp, (WORD)cx, (WORD)(base - s / 2));

    /* Zwei Fuesse. */
    Move(rp, (WORD)(cx - s / 4), (WORD)base);
    Draw(rp, (WORD)(cx + s / 4), (WORD)base);

    /* Zwei Wellen als offene Winkel nach oben. */
    for (i = 1; i <= 2; i++) {
        LONG dx = i * s / 4;
        LONG dy = i * s / 4;

        Move(rp, (WORD)(cx - dx), (WORD)(base - s / 2 - dy));
        Draw(rp, (WORD)cx,        (WORD)(base - s / 2));
        Draw(rp, (WORD)(cx + dx), (WORD)(base - s / 2 - dy));
    }
}

static ULONG tl_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct TLData *d = INST_DATA(cl, obj);
    struct RastPort *rp;
    LONG l, t, w, h, rh, rows, visible, i;
    LONG lw, rw, cw;

    DoSuperMethodA(cl, obj, (Msg)msg);

    if (!(msg->flags & MADF_DRAWOBJECT)) {
        return 0;
    }

    l = _mleft(obj);
    w = _mwidth(obj);
    if (w <= 0 || _mheight(obj) <= 0) {
        return 0;
    }

    draw_gradient(obj, d);

    rp = _rp(obj);
    SetFont(rp, _font(obj));
    SetDrMd(rp, JAM1);

    /* Die Ueberschrift steht im Kopfstreifen und rollt nicht mit. */
    if (d->title[0]) {
        SetAPen(rp, _pens(obj)[MPEN_SHINE]);
        Move(rp, l + PAD_X,
             _mtop(obj) + TL_HEAD_PAD / 2 + _font(obj)->tf_Baseline);
        Text(rp, (STRPTR)d->title, (LONG)strlen(d->title));
    }

    /* Ab hier zaehlt nur die Flaeche UNTER dem Kopfstreifen. */
    t = list_top(obj, d);
    h = list_height(obj, d);

    rh = row_height_kind(obj, d);
    rows = count_rows(d);
    visible = h / rh;

    /* Alle Spaltenmasse rechnen ab hier mit der Breite OHNE Rollleiste -
     * sonst schoebe sich die rechtsbuendige Dauer darunter. */
    cw = content_width(obj);

    /* Spaltenbreiten. Bei Titeln ist links nur die Nummer, bei Alben der
     * Interpret - deshalb unterschiedlich breit. */
    if (d->kind == TLK_RADIO) {
        /* Links das Sendersymbol, rechts nichts - die Homepage steht in
         * der zweiten Texthaelfte. */
        lw = TL_THUMB;
        rw = 0;
    } else if (d->kind == TLK_TRACKS) {
        /* Links das Cover statt einer Textspalte, rechts die Dauer.
         * Interpret und Album teilen sich die rechte Haelfte des
         * Textbereichs - siehe die Zeichenschleife. */
        lw = TL_THUMB;
        rw = TextLength(rp, (STRPTR)"88:88", 5);
    } else if (d->kind == TLK_SONGS) {
        lw = TextLength(rp, (STRPTR)"8888", 4);
        rw = TextLength(rp, (STRPTR)"88:88", 5);
    } else if (d->kind == TLK_ALBUMS) {
        lw = cw / 3;
        rw = TextLength(rp, (STRPTR)"8888", 4);
    } else {
        lw = 0;
        rw = 0;
    }

    for (i = 0; i < visible; i++) {
        LONG row = d->first + i;
        LONG y = t + i * rh;
        char left[SUB_ARTIST_LEN], mid[SUB_NAME_LEN], right[16];
        ULONG pen;

        if (row < 0 || row >= rows) {
            break;
        }

        row_text(d, row, left, sizeof(left), mid, sizeof(mid),
                 right, sizeof(right));

        /* Die gewaehlte Zeile bekommt einen Balken. Aufgehellt statt in
         * einer festen Farbe, damit sie zum jeweiligen Album passt. */
        if (row == d->active && d->kind != TLK_TEXT) {
            LONG r0 = ((d->colour >> 16) & 0xff) + 60;
            LONG g0 = ((d->colour >>  8) & 0xff) + 60;
            LONG b0 = ( d->colour        & 0xff) + 60;

            if (r0 > 255) { r0 = 255; }
            if (g0 > 255) { g0 = 255; }
            if (b0 > 255) { b0 = 255; }

            if (CyberGfxBase) {
                FillPixelArray(rp, (UWORD)l, (UWORD)y, (UWORD)cw, (UWORD)rh,
                               ((ULONG)r0 << 16) | ((ULONG)g0 << 8)
                             | (ULONG)b0);
            }
        }

        /* Der laufende Titel hebt sich zusaetzlich ab. */
        pen = _pens(obj)[(row == d->playing) ? MPEN_SHINE : MPEN_HALFSHINE];
        if (row == d->active) {
            pen = _pens(obj)[MPEN_SHINE];
        }

        if (d->kind == TLK_RADIO) {
            struct Radio *r = (struct Radio *)list_get(d->list, (int)row);
            LONG tx = l + PAD_X + lw + PAD_X;
            LONG textw = cw - 2 * PAD_X - lw - PAD_X;
            LONG halfw = textw / 2;
            LONG ty2 = y + (rh - text_height(obj)) / 2;

            /* Ein Sender in einem Format, das keiner der Dekoder kann
             * (weder MP3 noch AAC), laesst sich nicht abspielen. Er
             * bleibt in der Liste - der Anwender soll sehen, dass es ihn
             * gibt und warum er nicht geht -, aber grau und mit Vermerk
             * statt der Homepage. HALFSHADOW wie bei den frueher toten
             * Eintraegen der Seitenleiste. */
            if (r && r->unplayable) {
                pen = _pens(obj)[MPEN_HALFSHADOW];
            }

            /* Das Sendercover, sobald es da ist - bis dahin, und bei
             * einem Sender ohne Cover, die gezeichnete Antenne. */
            if (d->thumbs && d->thumbs[row] && d->thumbs[row]->rgb) {
                draw_thumb(obj, d, row, l + PAD_X, y, rh);
            } else {
                draw_antenna(obj, l + PAD_X, y, rh, pen);
            }

            draw_cell(obj, mid, tx, ty2, halfw, pen, FALSE);
            if (r) {
                draw_cell(obj, r->unplayable ? "format not supported"
                                      : r->home,
                          tx + halfw, ty2, halfw, pen, FALSE);
            }
        } else if (d->kind == TLK_TRACKS) {
            struct Song *sg = (struct Song *)list_get(d->list, (int)row);
            LONG tx = l + PAD_X + lw + PAD_X;
            LONG textw = cw - 2 * PAD_X - lw - rw - 2 * PAD_X;
            LONG halfw = textw / 2;
            /* Die Zeile ist hoeher als der Text, weil das Cover die
             * Hoehe vorgibt - der Text sitzt also mittig, nicht oben. */
            LONG ty2 = y + (rh - text_height(obj)) / 2;

            draw_thumb(obj, d, row, l + PAD_X, y, rh);

            /* Links der Titel, rechts daneben Interpret und Album zu
             * gleichen Teilen. Die Dauer steht ganz rechts. */
            draw_cell(obj, mid, tx, ty2, halfw, pen, FALSE);
            if (sg) {
                draw_cell(obj, sg->artist, tx + halfw, ty2, halfw / 2,
                          pen, FALSE);
                draw_cell(obj, sg->album, tx + halfw + halfw / 2, ty2,
                          halfw - halfw / 2, pen, FALSE);
            }
            draw_cell(obj, right, l + cw - PAD_X - rw, ty2, rw, pen, TRUE);
        } else {
            if (lw > 0) {
                draw_cell(obj, left, l + PAD_X, y, lw,
                          pen, d->kind == TLK_SONGS);
            }
            draw_cell(obj, mid, l + PAD_X + lw + PAD_X, y,
                      cw - 2 * PAD_X - lw - rw - 2 * PAD_X, pen, FALSE);
            if (rw > 0) {
                draw_cell(obj, right, l + cw - PAD_X - rw, y, rw, pen, TRUE);
            }
        }
    }

    draw_scrollbar(obj, d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Eingaben                                                            */
/* ------------------------------------------------------------------ */

static void scroll_to(Object *obj, struct TLData *d, LONG row)
{
    LONG rh = row_height_kind(obj, d);
    LONG visible = list_height(obj, d) / rh;
    LONG rows = count_rows(d);
    LONG first = d->first;

    if (visible < 1) {
        visible = 1;
    }
    if (row < first) {
        first = row;
    } else if (row >= first + visible) {
        first = row - visible + 1;
    }
    if (first > rows - visible) {
        first = rows - visible;
    }
    if (first < 0) {
        first = 0;
    }
    if (first != d->first) {
        d->first = first;
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
}

static void scroll_by(Object *obj, struct TLData *d, LONG delta)
{
    LONG rh = row_height_kind(obj, d);
    LONG visible = list_height(obj, d) / rh;
    LONG rows = count_rows(d);
    LONG first = d->first + delta;

    if (first > rows - visible) {
        first = rows - visible;
    }
    if (first < 0) {
        first = 0;
    }
    if (first != d->first) {
        d->first = first;
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
}

/* Aus der Lage des Schiebers die oberste Zeile rechnen. Wie bei der
 * Bildwand laeuft er ueber (Hoehe - Schieberhoehe), sonst waere das
 * Listenende nicht erreichbar. */
static void scroll_to_pixel(Object *obj, struct TLData *d, LONG my)
{
    LONG t = list_top(obj, d), h = list_height(obj, d);
    LONG rows = count_rows(d);
    LONG vis  = visible_rows(obj, d);
    LONG ty, th, travel, first;

    if (!thumb_rect(obj, d, &ty, &th)) {
        return;
    }
    travel = h - th;
    if (travel <= 0) {
        return;
    }

    first = (my - t) * (rows - vis) / travel;
    if (first > rows - vis) {
        first = rows - vis;
    }
    if (first < 0) {
        first = 0;
    }
    if (first != d->first) {
        d->first = first;
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
}

/* Liegt der Punkt im Objekt?
 *
 * MUI kennt dafuer ein Makro _isinobject(x,y), das aber ein Objekt
 * namens "obj" im Sichtbereich VORAUSSETZT - und in den hier benutzten
 * 3.8-Headern gibt es das Makro ohnehin nicht. Also ausgeschrieben. */
static BOOL in_object(Object *obj, LONG x, LONG y)
{
    return x >= _mleft(obj) && x <= _mright(obj)
        && y >= _mtop(obj)  && y <= _mbottom(obj);
}

static ULONG tl_handleevent(struct IClass *cl, Object *obj,
                            struct MUIP_HandleEvent *msg)
{
    struct TLData *d = INST_DATA(cl, obj);
    struct IntuiMessage *im = msg->imsg;
    if (!im) {
        return 0;
    }

    /* Eine Mausbewegung ohne laufendes Ziehen geht niemanden etwas an -
     * billig und als erstes abfangen. */
    if (im->Class == IDCMP_MOUSEMOVE && !d->drag) {
        return 0;
    }

    if (d->drag) {
        if (im->Class == IDCMP_MOUSEMOVE) {
            /* Ohne gedrueckte Taste endet das Ziehen - sonst haengt es
             * fest, wenn ausserhalb des Fensters losgelassen wurde. */
            if (!(im->Qualifier & IEQUALIFIER_LEFTBUTTON)) {
                d->drag = FALSE;
                return MUI_EventHandlerRC_Eat;
            }
            scroll_to_pixel(obj, d, im->MouseY - d->dragoff);
            return MUI_EventHandlerRC_Eat;
        }
        if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTUP) {
            d->drag = FALSE;
        }
        return MUI_EventHandlerRC_Eat;
    }

    if (im->Class == IDCMP_RAWKEY) {
        /* Mausrad. Die Codes 0x7A/0x7B melden Rad hoch/runter; MUI
         * reicht sie als RAWKEY durch. */
        if (im->Code == 0x7A) {
            scroll_by(obj, d, -3);
            return MUI_EventHandlerRC_Eat;
        }
        if (im->Code == 0x7B) {
            scroll_by(obj, d, 3);
            return MUI_EventHandlerRC_Eat;
        }
        return 0;
    }

    if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN) {
        LONG rh, row;
        if (!in_object(obj, im->MouseX, im->MouseY)) {
            return 0;
        }

        /* Rechter Rand: die Rollleiste. Sie kommt VOR der Pruefung auf
         * den Textmodus - im Liedtext gibt es zwar keine Zeilenauswahl,
         * aber rollen will man dort erst recht. */
        if (im->MouseX >= _mright(obj) - TL_SBW + 1) {
            LONG ty, th;

            if (!thumb_rect(obj, d, &ty, &th)) {
                return MUI_EventHandlerRC_Eat;
            }
            if (im->MouseY >= ty && im->MouseY < ty + th) {
                d->drag    = TRUE;
                d->dragoff = im->MouseY - ty;
            } else {
                scroll_by(obj, d,
                          (im->MouseY < ty) ? -visible_rows(obj, d)
                                            :  visible_rows(obj, d));
            }
            return MUI_EventHandlerRC_Eat;
        }

        if (d->kind == TLK_TEXT) {
            return 0;
        }

        /* In den Kopfstreifen getippt: nichts. Ohne die Abfrage kaeme
         * eine negative Differenz heraus, und die ganzzahlige Division
         * schneidet zur Null hin ab - der Klick landete auf Zeile 0. */
        if (im->MouseY < list_top(obj, d)) {
            return MUI_EventHandlerRC_Eat;
        }

        rh = row_height_kind(obj, d);
        row = d->first + (im->MouseY - list_top(obj, d)) / rh;
        if (row < 0 || row >= count_rows(d)) {
            return MUI_EventHandlerRC_Eat;
        }

        /* Doppelklick: DoubleClick() aus der intuition.library beachtet
         * die vom Anwender eingestellte Zeitspanne - eine feste Grenze
         * waere eine Bevormundung. */
        if (row == d->last_row
                && DoubleClick(d->last_secs, d->last_micros,
                               im->Seconds, im->Micros)) {
            set(obj, MUIA_TL_Active, row);
            /* Ueber set() und damit durch OM_SET: nur so bekommt MUIs
             * Notify-Oberklasse die Aenderung zu sehen und loest die
             * angehaengte Benachrichtigung aus. */
            set(obj, MUIA_TL_DoubleClick, TRUE);
            d->last_row = -1;
        } else {
            set(obj, MUIA_TL_Active, row);
            d->last_secs = im->Seconds;
            d->last_micros = im->Micros;
            d->last_row = row;
        }
        return MUI_EventHandlerRC_Eat;
    }

    return 0;
}

/* ------------------------------------------------------------------ */

static ULONG tl_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct TLData *d;

    obj = (Object *)DoSuperMethodA(cl, obj, (Msg)msg);
    if (!obj) {
        return 0;
    }
    d = INST_DATA(cl, obj);
    /* MUIA_FillArea, FALSE: MUIs Area-Oberklasse malt sonst VOR jedem
     * MUIM_Draw ihren eigenen Hintergrund in der MUI-Grundfarbe, und
     * unser Zeichnen legt sich erst danach darueber. Bei einem Objekt,
     * das sich im Sekundentakt auffrischt, sieht man dieses Aufblitzen -
     * genau der weisse Blitz in der Bedienleiste.
     *
     * Erlaubt ist das nur, weil diese Klasse ihre Flaeche VOLLSTAENDIG
     * selbst faerbt. Wer das nicht tut, bekommt mit FALSE stehen
     * gebliebenen Muell statt eines Hintergrunds. */
    set(obj, MUIA_FillArea, FALSE);

    d->kind    = (int)GetTagData(MUIA_TL_Kind, TLK_SONGS, msg->ops_AttrList);
    {
        const char *t = (const char *)GetTagData(MUIA_TL_Title, (ULONG)"",
                                                 msg->ops_AttrList);

        strncpy(d->title, t ? t : "", sizeof(d->title) - 1);
        d->title[sizeof(d->title) - 1] = '\0';
    }
    d->colour  = GetTagData(MUIA_TL_Colour, 0x00281640, msg->ops_AttrList);
    d->active  = -1;
    d->playing = -1;
    d->first   = 0;
    d->last_row = -1;
    return (ULONG)obj;
}

static ULONG tl_dispose(struct IClass *cl, Object *obj, Msg msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    if (d->buf) {
        free(d->buf);
    }
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG tl_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct TLData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;
    BOOL redraw = FALSE;

    while ((ti = NextTagItem(&tags)) != NULL) {
        switch (ti->ti_Tag) {
        case MUIA_TL_Colour:
            if (d->colour != ti->ti_Data) {
                d->colour = ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_TL_Active:
            if (d->active != (LONG)ti->ti_Data) {
                d->active = (LONG)ti->ti_Data;
                scroll_to(obj, d, d->active);
                redraw = TRUE;
            }
            break;
        case MUIA_TL_Title: {
            const char *t = (const char *)ti->ti_Data;

            if (!t) {
                t = "";
            }
            if (strcmp(d->title, t) != 0) {
                strncpy(d->title, t, sizeof(d->title) - 1);
                d->title[sizeof(d->title) - 1] = '\0';
                redraw = TRUE;
            }
            break;
        }
        case MUIA_TL_Playing:
            if (d->playing != (LONG)ti->ti_Data) {
                d->playing = (LONG)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        }
    }

    if (redraw) {
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
    /* IMMER an die Oberklasse: erst dort loest MUI die angehaengten
     * Benachrichtigungen aus. */
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG tl_get(struct IClass *cl, Object *obj, struct opGet *msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    switch (msg->opg_AttrID) {
    case MUIA_TL_Active:  *msg->opg_Storage = (ULONG)d->active;  return TRUE;
    case MUIA_TL_Playing: *msg->opg_Storage = (ULONG)d->playing; return TRUE;
    case MUIA_TL_Colour:  *msg->opg_Storage = d->colour;         return TRUE;
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG tl_askminmax(struct IClass *cl, Object *obj,
                          struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);

    msg->MinMaxInfo->MinWidth  += 200;
    msg->MinMaxInfo->MinHeight += 60;
    msg->MinMaxInfo->DefWidth  += 320;
    msg->MinMaxInfo->DefHeight += 240;
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MaxHeight += MUI_MAXMAX;
    return 0;
}

static ULONG tl_setup(struct IClass *cl, Object *obj, Msg msg)
{
    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }
    /* ZWEI Schritte, und der zweite ist der entscheidende:
     *
     *   MUI_RequestIDCMP() sagt dem FENSTER, welche Intuition-Ereignisse
     *   es ueberhaupt anfordern soll.
     *
     *   MUIM_Window_AddEventHandler traegt DIESES OBJEKT in die
     *   Ereigniskette des Fensters ein. Erst dadurch ruft MUI
     *   MUIM_HandleEvent auf.
     *
     * Mit dem ersten Schritt allein passiert gar nichts, und zwar
     * lautlos: gemessen kamen bei drei erfolgreichen MUIM_Setup null
     * Aufrufe von MUIM_HandleEvent an. */
    MUI_RequestIDCMP(obj, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY
                          | IDCMP_MOUSEMOVE);
    return TRUE;
}

static ULONG tl_cleanupm(struct IClass *cl, Object *obj, Msg msg)
{
    MUI_RejectIDCMP(obj, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY
                         | IDCMP_MOUSEMOVE);
    return DoSuperMethodA(cl, obj, msg);
}

/* Eingetragen wird erst beim SICHTBARWERDEN, nicht schon bei MUIM_Setup.
 * Titelliste und Liedtext liegen auf zwei Seiten derselben Gruppe mit
 * MUIA_Group_PageMode, also punktgenau uebereinander - eingetragen
 * bliebe die unsichtbare von beiden mitfangen und der sichtbaren die
 * Klicks wegnehmen. Ausfuehrlich steht das in albumgrid.c. */
static ULONG tl_show(struct IClass *cl, Object *obj, Msg msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }

    d->ehn.ehn_Object   = obj;
    d->ehn.ehn_Class    = cl;
    d->ehn.ehn_Events   = IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY
                          | IDCMP_MOUSEMOVE;
    d->ehn.ehn_Flags    = 0;
    d->ehn.ehn_Priority = 0;
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, (ULONG)&d->ehn);
    return TRUE;
}

static ULONG tl_hide(struct IClass *cl, Object *obj, Msg msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    d->drag = FALSE;
    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG tl_setlist(struct IClass *cl, Object *obj,
                        struct MUIP_TL_SetList *msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    d->list = msg->list;
    d->first = 0;
    d->active = -1;
    d->playing = -1;
    d->last_row = -1;
    /* Die Miniaturen gehoeren zur ALTEN Liste - mit ihr abhaengen,
     * sonst zeigten sie eine Zeile lang auf fremde Daten. */
    d->thumbs = NULL;
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

static ULONG tl_settext(struct IClass *cl, Object *obj,
                        struct MUIP_TL_SetText *msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    d->text = msg->text;
    d->first = 0;
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

static ULONG tl_setthumbs(struct IClass *cl, Object *obj,
                          struct MUIP_TL_SetThumbs *msg)
{
    struct TLData *d = INST_DATA(cl, obj);

    d->thumbs = msg->thumbs;
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

static ULONG tl_dispatch(struct IClass *cl  __asm("a0"),
                         Object       *obj __asm("a2"),
                         Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:            return tl_new(cl, obj, (struct opSet *)msg);
    case OM_DISPOSE:        return tl_dispose(cl, obj, msg);
    case OM_SET:            return tl_set(cl, obj, (struct opSet *)msg);
    case OM_GET:            return tl_get(cl, obj, (struct opGet *)msg);
    case MUIM_AskMinMax:    return tl_askminmax(cl, obj,
                                    (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:         return tl_draw(cl, obj,
                                    (struct MUIP_Draw *)msg);
    case MUIM_Setup:        return tl_setup(cl, obj, msg);
    case MUIM_Cleanup:      return tl_cleanupm(cl, obj, msg);
    case MUIM_Show:         return tl_show(cl, obj, msg);
    case MUIM_Hide:         return tl_hide(cl, obj, msg);
    case MUIM_HandleEvent:  return tl_handleevent(cl, obj,
                                    (struct MUIP_HandleEvent *)msg);
    case MUIM_TL_SetList:   return tl_setlist(cl, obj,
                                    (struct MUIP_TL_SetList *)msg);
    case MUIM_TL_SetText:   return tl_settext(cl, obj,
                                    (struct MUIP_TL_SetText *)msg);
    case MUIM_TL_SetThumbs: return tl_setthumbs(cl, obj,
                                    (struct MUIP_TL_SetThumbs *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

BOOL tl_init(void)
{
    g_mcc = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                  sizeof(struct TLData),
                                  (APTR)tl_dispatch);
    return g_mcc != NULL;
}

void tl_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
}
