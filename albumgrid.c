/* AmiSubsonic - die Alben als Bildwand. */

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

#include "albumgrid.h"

#if AG_DEBUG_CLICK
#include <proto/dos.h>

/* Anhaengen, nicht neu anlegen - so steht am Ende die ganze Folge da.
 * Das Verzeichnis wird beim ersten Mal angelegt; schlaegt es fehl,
 * passiert einfach nichts. */
static void clicklog(const char *fmt, LONG a, LONG b, LONG c, LONG d,
                     LONG e, LONG f, LONG g)
{
    BPTR fh;

    fh = Open((STRPTR)"RAM:AmiSubsonic/click.log", MODE_READWRITE);
    if (!fh) {
        BPTR lock = CreateDir((STRPTR)"RAM:AmiSubsonic");

        if (lock) {
            UnLock(lock);
        }
        fh = Open((STRPTR)"RAM:AmiSubsonic/click.log", MODE_READWRITE);
        if (!fh) {
            return;
        }
    }
    Seek(fh, 0, OFFSET_END);
    FPrintf(fh, (STRPTR)fmt, a, b, c, d, e, f, g);
    Close(fh);
}
#endif

extern struct Library *CyberGfxBase;     /* gehoert panel.c */

static struct MUI_CustomClass *g_mcc = NULL;

/* Obergrenze fuer den vorberechneten Verlauf, wie in panel.c. */
#define AG_MAXBUF  (1024L * 1024L)

/* Kantenlaenge des Bildfeldes je Zelle und die Raender darum. Der Wert
 * ist am Vorbild abgelesen: bei 760 Fensterbreite minus Seitenleiste
 * bleiben so fuenf Spalten. */
#define AG_COVER   AG_THUMB_SIZE

/* Luft ueber und unter der Ueberschrift zusammen. */
#define AG_HEAD_PAD  14
#define AG_GAP_X   10
#define AG_GAP_Y   10
#define AG_LINES   3            /* Album, Interpret, Jahr */

/* Breite der Rollleiste am rechten Rand und die kleinste Hoehe, die der
 * Schieber haben darf - bei vielen Alben waere er sonst nicht mehr zu
 * treffen. */
#define AG_SBW     12
#define AG_SBMIN   24

/* Warum eine SELBST GEZEICHNETE Rollleiste und keine von MUI:
 * MUIs Scrollbar bringt ihren eigenen grauen Hintergrund mit und saesse
 * als Fremdkoerper neben dem Farbverlauf - dieselbe Sache, die schon bei
 * panel.c und tracklist.c gegen MUIs Bordmittel entschieden hat. Hier
 * kostet sie ausserdem fast nichts: zwei Rechtecke. */

struct AGData {
    ULONG  colour;

    struct SubList    *list;    /* fremde Daten, nicht kopiert */
    struct AlbumThumb *thumbs;  /* dito, so viele wie die Liste lang ist */

    LONG   active;              /* gewaehltes Album, -1 = keines */

    /* Rollstand in BILDPUNKTEN, nicht in Zeilen.
     *
     * Zeilenweise war die erste Fassung, und sie sprang in Schritten von
     * einer ganzen Zellenhoehe - rund 140 Punkte. Punktweise kostet zwei
     * Dinge: die oberste und unterste Reihe sind angeschnitten und
     * muessen geklippt werden (MUI_AddClipping), und jeder Rollschritt
     * ist ein vollstaendiges Neuzeichnen. Zweiteres ist der Preis; ob er
     * zu hoch ist, entscheidet die Maschine und nicht die Theorie. */
    LONG   off;

    /* Ziehen am Schieber. dragoff ist der Abstand zwischen Mauszeiger
     * und Schieberkante beim Anfassen - ohne ihn springt der Schieber
     * beim ersten Ziehen unter die Maus. */
    BOOL   drag;
    LONG   dragoff;

    ULONG *buf;                 /* vorgerechneter Verlauf */
    LONG   buf_w, buf_h;
    ULONG  buf_col;

    /* Ueberschrift. Kopiert, weil der Auftraggeber sie aus einem
     * kurzlebigen Puffer setzen darf. Leer heisst: kein Streifen, die
     * Wand faengt oben am Rand an. */
    char   title[64];

    /* Muss die ganze Zeit leben, MUI verkettet ihn nur. */
    struct MUI_EventHandlerNode ehn;
};

struct MUI_CustomClass *ag_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */
/* Geometrie                                                           */
/* ------------------------------------------------------------------ */

static LONG line_height(Object *obj)
{
    struct TextFont *font = _font(obj);

    return (font ? font->tf_YSize : 8) + 1;
}

static LONG cell_width(void)
{
    return AG_COVER + AG_GAP_X;
}

static LONG cell_height(Object *obj)
{
    return AG_COVER + AG_LINES * line_height(obj) + AG_GAP_Y;
}

/* Hoehe des Kopfstreifens mit der Ueberschrift. Er ROLLT NICHT MIT:
 * die Wand darunter rollt, die Ueberschrift steht. Deshalb rechnen alle
 * Zeilen- und Trefferrechnungen ab grid_top() statt ab _mtop(). */
static LONG head_height(Object *obj, struct AGData *d)
{
    if (!d->title[0]) {
        return 0;
    }
    return line_height(obj) + AG_HEAD_PAD;
}

static LONG grid_top(Object *obj, struct AGData *d)
{
    return _mtop(obj) + head_height(obj, d);
}

static LONG grid_height(Object *obj, struct AGData *d)
{
    LONG h = _mheight(obj) - head_height(obj, d);

    return h > 0 ? h : 1;
}

/* Die Breite, die den Zellen bleibt - die Rollleiste geht ab. */
static LONG content_width(Object *obj)
{
    LONG w = _mwidth(obj) - AG_SBW;

    return w > 0 ? w : 1;
}

static LONG columns(Object *obj)
{
    LONG cols = content_width(obj) / cell_width();

    return cols > 0 ? cols : 1;
}

static LONG count_albums(struct AGData *d)
{
    return d->list ? d->list->count : 0;
}

static LONG count_lines(Object *obj, struct AGData *d)
{
    LONG cols = columns(obj);

    return (count_albums(d) + cols - 1) / cols;
}

/* Gesamthoehe aller Zeilen und die groesste zulaessige Rollstellung. */
static LONG total_height(Object *obj, struct AGData *d)
{
    return count_lines(obj, d) * cell_height(obj);
}

static LONG max_off(Object *obj, struct AGData *d)
{
    LONG m = total_height(obj, d) - grid_height(obj, d);

    return m > 0 ? m : 0;
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static BOOL build_gradient(struct AGData *d, LONG w, LONG h)
{
    LONG r0, g0, b0, y, x;
    ULONG *p;

    if (d->buf && d->buf_w == w && d->buf_h == h && d->buf_col == d->colour) {
        return TRUE;
    }
    if ((long)w * (long)h * (long)sizeof(ULONG) > AG_MAXBUF) {
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

static void draw_gradient(Object *obj, struct AGData *d)
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

/* Text in eine Zelle, links buendig, bei Bedarf beschnitten. */
static void draw_line(Object *obj, const char *s, LONG x, LONG y,
                      LONG maxw, ULONG pen)
{
    struct RastPort *rp = _rp(obj);
    struct TextFont *font = _font(obj);
    LONG len;
    struct TextExtent te;
    ULONG fit;

    if (!s || !s[0] || !font || maxw <= 0) {
        return;
    }
    len = (LONG)strlen(s);

    SetAPen(rp, pen);
    fit = TextFit(rp, (STRPTR)s, (ULONG)len, &te, NULL, 1,
                  (UWORD)maxw, (UWORD)font->tf_YSize);
    if (fit < (ULONG)len) {
        len = (LONG)fit;
    }
    if (len <= 0) {
        return;
    }
    Move(rp, (WORD)x, (WORD)(y + font->tf_Baseline));
    Text(rp, (STRPTR)s, (ULONG)len);
}

/* Ein waagerechter Streifen, oben und unten am Objektrand abgeschnitten.
 *
 * WARUM VON HAND UND NICHT MIT MUI_AddClipping: die Klipp-Funktionen der
 * muimaster.library stehen im hier vendorierten Inline-Header nicht drin,
 * und ihre LVOs zu raten waere genau die Art Fehler, die in diesem
 * Projekt jedes Mal lautlos war. Zuschneiden ist ohnehin billiger als
 * eine Klipp-Region: WritePixelArray kann bei einer oben angeschnittenen
 * Reihe einfach weiter unten in der Quelle anfangen. */
static void draw_thumb(Object *obj, struct AlbumThumb *th, LONG x, LONG y,
                       LONG top, LONG bot)
{
    struct RastPort *rp = _rp(obj);
    LONG ox, oy, sy, hh;

    if (!CyberGfxBase) {
        return;
    }

    if (!th || !th->rgb || th->w <= 0 || th->h <= 0) {
        /* Platzhalter, solange das Cover noch unterwegs ist. Etwas
         * heller als der Verlauf, damit man sieht, dass da noch etwas
         * kommt - und damit die Wand nicht leer wirkt. */
        oy = y;
        hh = AG_COVER;
        if (oy < top)       { hh -= top - oy; oy = top; }
        if (oy + hh > bot)  { hh = bot - oy; }
        if (hh > 0) {
            FillPixelArray(rp, (UWORD)x, (UWORD)oy,
                           (UWORD)AG_COVER, (UWORD)hh, 0x00201828);
        }
        return;
    }

    /* Mittig im Bildfeld: nicht jedes Cover ist quadratisch. */
    ox = x + (AG_COVER - th->w) / 2;
    oy = y + (AG_COVER - th->h) / 2;
    sy = 0;
    hh = th->h;

    if (oy < top) {
        sy = top - oy;
        hh -= sy;
        oy = top;
    }
    if (oy + hh > bot) {
        hh = bot - oy;
    }
    if (hh <= 0 || sy >= th->h) {
        return;
    }

    WritePixelArray((APTR)th->rgb, 0, (UWORD)sy, (UWORD)(th->w * 3), rp,
                    (UWORD)ox, (UWORD)oy,
                    (UWORD)th->w, (UWORD)hh, RECTFMT_RGB);
}

/* Text nur zeichnen, wenn die Zeile GANZ ins Objekt passt. Eine
 * halbierte Textzeile liesse sich ohne Klipp-Region nicht darstellen,
 * und am Rand fehlender Text faellt weit weniger auf als einer, der
 * ueber die Kante laeuft. */
static BOOL line_fits(Object *obj, LONG y, LONG top, LONG bot)
{
    struct TextFont *font = _font(obj);
    LONG fh = font ? font->tf_YSize : 8;

    return (y >= top) && (y + fh <= bot);
}

/* Lage des Schiebers in Bildschirmkoordinaten. FALSE, wenn alles
 * hineinpasst - dann gibt es nichts zu rollen und nichts zu zeichnen. */
static BOOL thumb_rect(Object *obj, struct AGData *d, LONG *ty, LONG *th)
{
    LONG h     = grid_height(obj, d);
    LONG total = total_height(obj, d);
    LONG mo    = max_off(obj, d);
    LONG hh, yy;

    if (mo <= 0 || h <= 0 || total <= 0) {
        return FALSE;
    }

    hh = h * h / total;
    if (hh < AG_SBMIN) {
        hh = AG_SBMIN;
    }
    /* Der Schieber laeuft ueber die Reststrecke, nicht ueber die ganze
     * Hoehe - sonst stuende er am Listenende unten ueber. */
    yy = (h - hh) * d->off / mo;

    *ty = grid_top(obj, d) + yy;
    *th = hh;
    return TRUE;
}

static void draw_scrollbar(Object *obj, struct AGData *d)
{
    struct RastPort *rp = _rp(obj);
    LONG x = _mright(obj) - AG_SBW + 1;
    LONG t = grid_top(obj, d), h = grid_height(obj, d);
    LONG ty, th;

    if (!CyberGfxBase || h <= 0) {
        return;
    }

    /* Die Bahn: etwas dunkler als der Verlauf an dieser Stelle, damit
     * sie sich abhebt, ohne ein Fremdkoerper zu sein. */
    FillPixelArray(rp, (UWORD)x, (UWORD)t, AG_SBW, (UWORD)h, 0x00100810);

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
                       AG_SBW - 4, (UWORD)th,
                       ((ULONG)r0 << 16) | ((ULONG)g0 << 8) | (ULONG)b0);
    }
}

static ULONG ag_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct AGData *d = INST_DATA(cl, obj);
    struct RastPort *rp;
    LONG l, t, w, h;
    LONG cw, ch, lh, cols, lines, first, ytop, i, j;

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

    /* Die Ueberschrift steht im Kopfstreifen und rollt nicht mit. Der
     * Verlauf ist schon gezeichnet, sie wird nur hineingeschrieben. */
    if (d->title[0]) {
        SetAPen(rp, _dri(obj)->dri_Pens[SHINEPEN]);
        Move(rp, l + AG_GAP_X / 2,
             _mtop(obj) + AG_HEAD_PAD / 2 + _font(obj)->tf_Baseline);
        Text(rp, (STRPTR)d->title, (LONG)strlen(d->title));
    }

    /* Ab hier zaehlt nur noch die Flaeche UNTER dem Kopfstreifen. */
    t = grid_top(obj, d);
    h = grid_height(obj, d);

    cw    = cell_width();
    ch    = cell_height(obj);
    lh    = line_height(obj);
    cols  = columns(obj);
    lines = count_lines(obj, d);

    /* Oberste angeschnittene Reihe und ihr Versatz nach oben. */
    first = d->off / ch;
    ytop  = t - (d->off % ch);

    for (i = 0; ; i++) {
        LONG line = first + i;
        LONG y = ytop + i * ch;

        if (line < 0 || line >= lines || y >= t + h) {
            break;
        }

        for (j = 0; j < cols; j++) {
            LONG idx = line * cols + j;
            LONG x = l + j * cw + AG_GAP_X / 2;
            struct Album *a;
            ULONG pen;
            char year[16];

            if (idx >= count_albums(d)) {
                break;
            }
            a = (struct Album *)list_get(d->list, (int)idx);
            if (!a) {
                continue;
            }

            /* Das gewaehlte Album bekommt einen aufgehellten Kasten -
             * dieselbe Loesung wie in der Titelliste, damit die
             * Markierung zu jedem Farbton passt. */
            if (idx == d->active && CyberGfxBase) {
                LONG r0 = ((d->colour >> 16) & 0xff) + 60;
                LONG g0 = ((d->colour >>  8) & 0xff) + 60;
                LONG b0 = ( d->colour        & 0xff) + 60;
                LONG by = y, bh = ch;

                if (r0 > 255) { r0 = 255; }
                if (g0 > 255) { g0 = 255; }
                if (b0 > 255) { b0 = 255; }

                if (by < t)          { bh -= t - by; by = t; }
                if (by + bh > t + h) { bh = t + h - by; }

                if (bh > 0) {
                    FillPixelArray(rp, (UWORD)(x - AG_GAP_X / 2), (UWORD)by,
                                   (UWORD)cw, (UWORD)bh,
                                   ((ULONG)r0 << 16) | ((ULONG)g0 << 8)
                                 | (ULONG)b0);
                }
            }

            draw_thumb(obj, d->thumbs ? &d->thumbs[idx] : NULL, x, y,
                       t, t + h);

            pen = _pens(obj)[MPEN_SHINE];
            if (line_fits(obj, y + AG_COVER + 2, t, t + h)) {
                draw_line(obj, a->name, x, y + AG_COVER + 2, AG_COVER, pen);
            }

            pen = _pens(obj)[MPEN_HALFSHINE];
            if (line_fits(obj, y + AG_COVER + 2 + lh, t, t + h)) {
                draw_line(obj, a->artist, x, y + AG_COVER + 2 + lh,
                          AG_COVER, pen);
            }

            year[0] = '\0';
            if (a->year > 0) {
                sprintf(year, "%d", a->year);
            }
            if (line_fits(obj, y + AG_COVER + 2 + 2 * lh, t, t + h)) {
                draw_line(obj, year, x, y + AG_COVER + 2 + 2 * lh,
                          AG_COVER, pen);
            }
        }
    }

    draw_scrollbar(obj, d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Eingaben                                                            */
/* ------------------------------------------------------------------ */

static void scroll_to(Object *obj, struct AGData *d, LONG off)
{
    LONG mo = max_off(obj, d);

    if (off > mo) {
        off = mo;
    }
    if (off < 0) {
        off = 0;
    }
    if (off != d->off) {
        d->off = off;
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
}

static void scroll_by(Object *obj, struct AGData *d, LONG delta)
{
    scroll_to(obj, d, d->off + delta);
}

/* Aus der Lage des Schiebers die Rollstellung rechnen. Er laeuft ueber
 * (Hoehe - Schieberhoehe), also muss dieselbe Strecke auch hier stehen -
 * sonst laesst sich das Listenende nicht erreichen. */
static void scroll_to_pixel(Object *obj, struct AGData *d, LONG my)
{
    LONG t = grid_top(obj, d), h = grid_height(obj, d);
    LONG ty, th, travel;

    if (!thumb_rect(obj, d, &ty, &th)) {
        return;
    }
    travel = h - th;
    if (travel <= 0) {
        return;
    }

    scroll_to(obj, d, (my - t) * max_off(obj, d) / travel);
}

static BOOL in_object(Object *obj, LONG x, LONG y)
{
    return x >= _mleft(obj) && x <= _mright(obj)
        && y >= _mtop(obj)  && y <= _mbottom(obj);
}

static ULONG ag_handleevent(struct IClass *cl, Object *obj,
                            struct MUIP_HandleEvent *msg)
{
    struct AGData *d = INST_DATA(cl, obj);
    struct IntuiMessage *im = msg->imsg;

    if (!im) {
        return 0;
    }

    /* Der haeufigste Fall zuerst und billig: eine Mausbewegung, ohne
     * dass gezogen wird, geht niemanden etwas an. */
    if (im->Class == IDCMP_MOUSEMOVE && !d->drag) {
        return 0;
    }

    if (im->Class == IDCMP_RAWKEY) {
        /* Ein Drittel Zellenhoehe je Rastung - drei Rastungen sind eine
         * Reihe. Eine ganze Reihe je Rastung war der Sprung, ueber den
         * sich beim Ausprobieren zu Recht beschwert wurde. */
        LONG step = cell_height(obj) / 3;

        if (step < 1) {
            step = 1;
        }
        if (im->Code == 0x7A) {
            scroll_by(obj, d, -step);
            return MUI_EventHandlerRC_Eat;
        }
        if (im->Code == 0x7B) {
            scroll_by(obj, d, step);
            return MUI_EventHandlerRC_Eat;
        }
        return 0;
    }

    /* Ziehen am Schieber. Kommt VOR allem anderen: solange gezogen wird,
     * geht kein anderes Mausereignis irgendwo anders hin. */
    if (d->drag) {
        if (im->Class == IDCMP_MOUSEMOVE) {
            /* Kam das Loslassen nicht an - etwa weil die Maus dabei
             * ausserhalb des Fensters war - haengt das Ziehen sonst
             * fest. Der Qualifier sagt, ob die Taste noch unten ist. */
            if (!(im->Qualifier & IEQUALIFIER_LEFTBUTTON)) {
                d->drag = FALSE;
                return MUI_EventHandlerRC_Eat;
            }
            scroll_to_pixel(obj, d, im->MouseY - d->dragoff);
            return MUI_EventHandlerRC_Eat;
        }
        if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTUP) {
            d->drag = FALSE;
            return MUI_EventHandlerRC_Eat;
        }
        return MUI_EventHandlerRC_Eat;
    }

    if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN) {
        LONG cw, ch, cols, line, col, idx;

        if (!in_object(obj, im->MouseX, im->MouseY)) {
            return 0;
        }

        /* Rechter Rand: die Rollleiste. */
        if (im->MouseX >= _mright(obj) - AG_SBW + 1) {
            LONG ty, th;

            if (!thumb_rect(obj, d, &ty, &th)) {
                return MUI_EventHandlerRC_Eat;
            }
            if (im->MouseY >= ty && im->MouseY < ty + th) {
                d->drag    = TRUE;
                d->dragoff = im->MouseY - ty;
            } else {
                /* Neben den Schieber getippt: eine Seite weiter. */
                scroll_by(obj, d,
                          (im->MouseY < ty) ? -grid_height(obj, d)
                                            :  grid_height(obj, d));
            }
            return MUI_EventHandlerRC_Eat;
        }

        /* In den Kopfstreifen getippt: nichts. Ohne diese Abfrage kaeme
         * eine negative Differenz heraus, und die ganzzahlige Division
         * schneidet zur Null hin ab - der Klick landete auf Zeile 0. */
        if (im->MouseY < grid_top(obj, d)) {
            return MUI_EventHandlerRC_Eat;
        }

        cw   = cell_width();
        ch   = cell_height(obj);
        cols = columns(obj);
        line = (d->off + im->MouseY - grid_top(obj, d)) / ch;
        col  = (im->MouseX - _mleft(obj)) / cw;
        if (col >= cols) {
            col = cols - 1;
        }
        idx = line * cols + col;

#if AG_DEBUG_CLICK
        clicklog("klick my=%ld mx=%ld off=%ld ch=%ld cw=%ld cols=%ld idx=%ld\n",
                 (LONG)im->MouseY - _mtop(obj), (LONG)im->MouseX - _mleft(obj),
                 d->off, ch, cw, cols, idx);
#endif

        if (idx < 0 || idx >= count_albums(d)) {
            return MUI_EventHandlerRC_Eat;
        }

        /* EINFACHklick oeffnet, kein Doppelklick: die Bildwand ist die
         * Startseite, und ein Album anzutippen ist dort die einzige
         * sinnvolle Handlung. */
        set(obj, MUIA_AG_Active, idx);
        /* Ueber set() und damit durch OM_SET - nur so sieht MUIs
         * Notify-Oberklasse die Aenderung. */
        set(obj, MUIA_AG_Click, TRUE);
        return MUI_EventHandlerRC_Eat;
    }

    return 0;
}

/* ------------------------------------------------------------------ */

static ULONG ag_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct AGData *d;

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

    d->colour = GetTagData(MUIA_AG_Colour, 0x00281640, msg->ops_AttrList);
    {
        const char *t = (const char *)GetTagData(MUIA_AG_Title, (ULONG)"",
                                                 msg->ops_AttrList);

        strncpy(d->title, t ? t : "", sizeof(d->title) - 1);
        d->title[sizeof(d->title) - 1] = '\0';
    }
    d->active = -1;
    d->off    = 0;
    return (ULONG)obj;
}

static ULONG ag_dispose(struct IClass *cl, Object *obj, Msg msg)
{
    struct AGData *d = INST_DATA(cl, obj);

    if (d->buf) {
        free(d->buf);
    }
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG ag_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct AGData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;
    BOOL redraw = FALSE;

    while ((ti = NextTagItem(&tags)) != NULL) {
        switch (ti->ti_Tag) {
        case MUIA_AG_Colour:
            if (d->colour != ti->ti_Data) {
                d->colour = ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_AG_Active:
            if (d->active != (LONG)ti->ti_Data) {
                d->active = (LONG)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_AG_Title: {
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
        }
    }

    if (redraw) {
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
    /* IMMER weiterreichen: erst die Oberklasse loest die angehaengten
     * Benachrichtigungen aus. */
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG ag_get(struct IClass *cl, Object *obj, struct opGet *msg)
{
    struct AGData *d = INST_DATA(cl, obj);

    switch (msg->opg_AttrID) {
    case MUIA_AG_Active: *msg->opg_Storage = (ULONG)d->active; return TRUE;
    case MUIA_AG_Colour: *msg->opg_Storage = d->colour;        return TRUE;
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG ag_askminmax(struct IClass *cl, Object *obj,
                          struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);

    /* Mindestens eine Spalte und eine Zeile, sonst sieht man nichts.
     * Die Vorgabe zielt auf fuenf Spalten - das ist die Aufteilung aus
     * dem Vorbild. */
    msg->MinMaxInfo->MinWidth  += cell_width() + AG_GAP_X;
    msg->MinMaxInfo->MinHeight += AG_COVER + 3 * 10 + AG_GAP_Y;
    msg->MinMaxInfo->DefWidth  += 5 * cell_width();
    msg->MinMaxInfo->DefHeight += 2 * (AG_COVER + 3 * 10 + AG_GAP_Y);
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MaxHeight += MUI_MAXMAX;
    return 0;
}

/* WARUM DIE EREIGNISKETTE AN MUIM_Show HAENGT UND NICHT AN MUIM_Setup
 *
 * Gekostet: die Playeransicht war unbedienbar. Ein Klick auf eine
 * Titelzeile tat nichts, ein Klick auf LYRICS lud ein fremdes Album.
 *
 * Grund: MUI ruft MUIM_HandleEvent bei JEDEM eingetragenen Objekt des
 * Fensters, auch bei einem, das gerade auf der unsichtbaren Seite liegt.
 * Und eine Gruppe mit MUIA_Group_PageMode legt alle Kinder auf DIESELBE
 * Flaeche - die unsichtbare Bildwand lag also punktgenau unter der
 * Playeransicht, rechnete jeden Klick in einen Albumindex um und fraas
 * ihn mit MUI_EventHandlerRC_Eat, bevor die Liste ihn sehen konnte.
 *
 * MUIM_Setup/MUIM_Cleanup sagen "das Objekt gehoert zu einem offenen
 * Fenster", MUIM_Show/MUIM_Hide sagen "es ist gerade zu sehen". Fuer
 * Ereignisse zaehlt das zweite. */
static ULONG ag_setup(struct IClass *cl, Object *obj, Msg msg)
{
    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }

    /* Beide Schritte noetig - siehe die ausfuehrliche Begruendung in
     * tracklist.c: MUI_RequestIDCMP allein bewirkt gar nichts, und zwar
     * lautlos. */
    /* MOUSEMOVE steht hier FEST in der Kette, obwohl es nur beim Ziehen
     * am Schieber gebraucht wird.
     *
     * Der erste Anlauf trug es erst beim Anfassen ein und danach wieder
     * aus - sparsamer, aber es funktionierte nicht: das Ziehen tat
     * nichts, nur das seitenweise Klicken ging. Der Grund ist, dass
     * MUIM_Window_Rem/AddEventHandler dabei MITTEN in MUIM_HandleEvent
     * gerufen wird, also waehrend MUI genau diese Liste durchlaeuft.
     * Wer die Liste unter dem Iterator umbaut, bekommt kein Ereignis
     * mehr - lautlos, wie so vieles hier.
     *
     * Die feste Anmeldung kostet Ereignisse bei jeder Mausbewegung ueber
     * dem Fenster. Der Handler steigt dafuer in der ersten Zeile wieder
     * aus, wenn nicht gezogen wird. */
    MUI_RequestIDCMP(obj, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY
                          | IDCMP_MOUSEMOVE);
    return TRUE;
}

static ULONG ag_cleanupm(struct IClass *cl, Object *obj, Msg msg)
{
    MUI_RejectIDCMP(obj, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY | IDCMP_MOUSEMOVE);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG ag_show(struct IClass *cl, Object *obj, Msg msg)
{
    struct AGData *d = INST_DATA(cl, obj);

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

static ULONG ag_hide(struct IClass *cl, Object *obj, Msg msg)
{
    struct AGData *d = INST_DATA(cl, obj);

    /* Ein Ziehen, das beim Umschalten der Seite laeuft, endet hier -
     * sonst haenge es beim naechsten Sichtbarwerden noch. */
    d->drag = FALSE;
    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG ag_setlist(struct IClass *cl, Object *obj,
                        struct MUIP_AG_SetList *msg)
{
    struct AGData *d = INST_DATA(cl, obj);

    d->list   = msg->list;
    d->thumbs = msg->thumbs;
    d->off    = 0;
    d->active = -1;
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

static ULONG ag_refresh(struct IClass *cl, Object *obj, Msg msg)
{
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

static ULONG ag_dispatch(struct IClass *cl  __asm("a0"),
                         Object       *obj __asm("a2"),
                         Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:            return ag_new(cl, obj, (struct opSet *)msg);
    case OM_DISPOSE:        return ag_dispose(cl, obj, msg);
    case OM_SET:            return ag_set(cl, obj, (struct opSet *)msg);
    case OM_GET:            return ag_get(cl, obj, (struct opGet *)msg);
    case MUIM_AskMinMax:    return ag_askminmax(cl, obj,
                                    (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:         return ag_draw(cl, obj,
                                    (struct MUIP_Draw *)msg);
    case MUIM_Setup:        return ag_setup(cl, obj, msg);
    case MUIM_Cleanup:      return ag_cleanupm(cl, obj, msg);
    case MUIM_Show:         return ag_show(cl, obj, msg);
    case MUIM_Hide:         return ag_hide(cl, obj, msg);
    case MUIM_HandleEvent:  return ag_handleevent(cl, obj,
                                    (struct MUIP_HandleEvent *)msg);
    case MUIM_AG_SetList:   return ag_setlist(cl, obj,
                                    (struct MUIP_AG_SetList *)msg);
    case MUIM_AG_Refresh:   return ag_refresh(cl, obj, msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

BOOL ag_init(void)
{
    g_mcc = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                  sizeof(struct AGData),
                                  (APTR)ag_dispatch);
    return g_mcc != NULL;
}

void ag_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
}
