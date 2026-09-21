/* AmiSubsonic - die "laeuft gerade"-Flaeche, selbst gezeichnet. */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <graphics/gfxmacros.h>
#include <libraries/mui.h>
#include <clib/alib_protos.h>

#include <stdlib.h>
#include <string.h>

#include "panel.h"
#include "cover.h"

extern struct Library *MUIMasterBase;
struct Library *CyberGfxBase = NULL;

static struct MUI_CustomClass *g_mcc = NULL;

/* Obergrenze fuer den vorberechneten Verlauf. Darueber wird zeilenweise
 * gefuellt - das kostet ein paar Millisekunden mehr statt Megabyte.
 * Gemessen liegen beide Wege bei 3 bis 6 ms; die Streuung ueber mehrere
 * Laeufe war groesser als der Unterschied. */
#define PANEL_MAXBUF  (1024L * 1024L)

#define LINE_TITLE  0
#define LINE_ARTIST 1
#define LINE_ALBUM  2
#define LINE_META   3
#define NUM_LINES   4

struct PanelData {
    ULONG colour;

    char  line[NUM_LINES][160];
    char  coverfile[128];

    struct CoverImage img;
    BOOL   img_ok;

    /* Das Cover, auf die verfuegbare Flaeche gerechnet. Wird neu
     * gerechnet, wenn sich Bild oder Flaeche aendern - nicht bei jedem
     * Zeichnen. */
    UBYTE *sc;
    LONG   sc_w, sc_h;          /* Groesse des gerechneten Bildes */
    LONG   sc_box;              /* Kantenlaenge, fuer die es gilt */

    ULONG *buf;                 /* vorgerechneter Verlauf */
    LONG   buf_w, buf_h;
    ULONG  buf_col;
};

struct MUI_CustomClass *panel_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */

static void set_string(char *dst, int size, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

static void drop_scaled(struct PanelData *d)
{
    if (d->sc) {
        free(d->sc);
        d->sc = NULL;
    }
    d->sc_w = d->sc_h = d->sc_box = 0;
}

static void load_cover(struct PanelData *d)
{
    drop_scaled(d);
    if (d->img_ok) {
        cover_unload(&d->img);
        d->img_ok = FALSE;
    }
    if (d->coverfile[0]) {
        d->img_ok = cover_load(d->coverfile, &d->img);
    }
}

/* Rechnet das Cover auf ein Quadrat der Kantenlaenge box herunter.
 *
 * Das eigentliche Rechnen steht in cover.c, weil die Bildwand dasselbe
 * braucht - eine zweite Fassung hier waere eine sichere Fehlerquelle.
 * Was HIER bleibt, ist das Zwischenspeichern: dieselbe Groesse soll
 * nicht bei jedem Neuzeichnen erneut berechnet werden. */
static BOOL scale_cover(struct PanelData *d, LONG box)
{
    UBYTE *dst = NULL;
    LONG dw = 0, dh = 0;

    if (!d->img_ok || !d->img.rgb || box <= 0) {
        return FALSE;
    }
    if (d->sc && d->sc_box == box) {
        return TRUE;
    }

    /* Mit grow=TRUE: die Flaeche soll sich der Fenstergroesse anpassen,
     * also wird ein kleineres Cover auch hochgezogen. Die Quelle ist 300
     * Punkte gross - darueber hinaus wird es weich, aber ein Cover, das
     * in einer halbleeren Flaeche klein in der Mitte klebt, sieht
     * schlechter aus. */
    if (!cover_scale_rgb(&d->img, box, TRUE, &dst, &dw, &dh)) {
        return FALSE;
    }

    if (d->sc) {
        free(d->sc);
    }
    d->sc = dst;
    d->sc_w = dw;
    d->sc_h = dh;
    d->sc_box = box;
    return TRUE;
}

/* ------------------------------------------------------------------ */

static BOOL build_gradient(struct PanelData *d, LONG w, LONG h)
{
    LONG r0, g0, b0, y, x;
    ULONG *p;

    if (d->buf && d->buf_w == w && d->buf_h == h && d->buf_col == d->colour) {
        return TRUE;
    }
    if ((long)w * (long)h * (long)sizeof(ULONG) > PANEL_MAXBUF) {
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
        /* Ganzzahlig nach Schwarz. Nenner h-1, damit die letzte Zeile
         * wirklich schwarz ist; bei h == 1 waere das eine Division durch
         * Null. Kein Fliesskomma - das Zielprofil hat keine FPU. */
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

static void draw_gradient(Object *obj, struct PanelData *d)
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
        LONG r0 = (d->colour >> 16) & 0xff;
        LONG g0 = (d->colour >>  8) & 0xff;
        LONG b0 =  d->colour        & 0xff;
        LONG y;

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

/* Eine Zeile mittig. Gibt die Hoehe zurueck, die sie belegt hat. */
static LONG draw_line(Object *obj, const char *s, LONG cx, LONG y,
                      LONG maxw, ULONG pen, BOOL bold)
{
    struct RastPort *rp = _rp(obj);
    struct TextFont *font = _font(obj);
    LONG len = (LONG)strlen(s);
    LONG px;

    if (!s[0] || !font) {
        return font ? font->tf_YSize : 0;
    }

    SetFont(rp, font);
    SetDrMd(rp, JAM1);
    SetAPen(rp, pen);
    if (bold) {
        SetSoftStyle(rp, FSF_BOLD, AskSoftStyle(rp));
    } else {
        SetSoftStyle(rp, FS_NORMAL, AskSoftStyle(rp));
    }

    /* Zu lange Zeilen abschneiden statt ueber den Rand zu malen.
     * TextFit() rechnet aus, wie viele Zeichen hineinpassen - von Hand
     * ueber TextLength() zu iterieren waere dasselbe, nur langsamer. */
    {
        struct TextExtent te;
        ULONG fit = TextFit(rp, (STRPTR)s, (ULONG)len, &te, NULL, 1,
                            (UWORD)maxw, (UWORD)font->tf_YSize);
        if (fit < (ULONG)len) {
            len = (LONG)fit;
        }
        px = cx - TextLength(rp, (STRPTR)s, (ULONG)len) / 2;
    }

    Move(rp, (WORD)px, (WORD)(y + font->tf_Baseline));
    Text(rp, (STRPTR)s, (ULONG)len);

    return font->tf_YSize;
}

/* Schlagschatten hinter dem Cover.
 *
 * Gerechnet wird gegen den VERLAUF, nicht gegen eine feste Farbe: die
 * Flaeche ist an jeder Zeile anders hell, ein Schatten in fester Farbe
 * saesse dort als grauer Fleck. Die Verlaufsfarbe einer Zeile steht
 * ueber dieselbe Formel fest, die build_gradient benutzt - also wird sie
 * hier einfach noch einmal gerechnet, statt die Grafikkarte
 * zurueckzulesen.
 *
 * Der Schatten ist genau so gross wie das Bild und sitzt nur versetzt
 * dahinter - er laeuft NICHT nach aussen aus. Weich ist allein die Kante,
 * ueber eine lineare Rampe von 2*BLUR Punkten, mittig darauf. Bei BLUR=3
 * sind das drei Punkte Auslauf: gerade genug, dass die Kante nicht
 * treppt, und weit entfernt von einer Wolke.
 *
 * KEIN malloc: die Zeile geht ueber einen festen Puffer heraus. Die
 * Oberflaeche darf nicht anlegen, solange ein Netzauftrag laeuft (siehe
 * netjob.h), und MUI kann jederzeit ein Neuzeichnen verlangen - etwa
 * wenn das Fenster wieder freigelegt wird. */
#define SH_BLUR    3        /* halbe Breite des weichen Randes */
#define SH_DY      8        /* Versatz nach unten */
#define SH_DX      6        /* Versatz nach rechts */
#define SH_ALPHA 147        /* Deckung in der Mitte, 0..255 */
#define SH_MAXW  800        /* breiter wird keine Zeile geschrieben */

static ULONG g_shadowrow[SH_MAXW];

/* Deckung 0..255 an einer Koordinate: voll innerhalb, 0 weit ausserhalb,
 * dazwischen linear. */
static LONG sh_ramp(LONG pos, LONG lo, LONG hi)
{
    LONG a = pos - lo;
    LONG b = hi - pos;
    LONG m = (a < b) ? a : b;

    m += SH_BLUR;
    if (m <= 0) {
        return 0;
    }
    if (m >= 2 * SH_BLUR) {
        return 255;
    }
    return m * 255 / (2 * SH_BLUR);
}

static ULONG sh_darken(ULONG rgb, LONG alpha)
{
    LONG k = 255 - alpha;

    return ((((rgb >> 16) & 0xff) * k / 255) << 16)
         | ((((rgb >>  8) & 0xff) * k / 255) <<  8)
         |  ((( rgb        & 0xff) * k / 255));
}

static void draw_shadow(Object *obj, struct PanelData *d,
                        LONG cvx, LONG cvy, LONG cvw, LONG cvh)
{
    struct RastPort *rp = _rp(obj);
    LONG l = _mleft(obj), t = _mtop(obj);
    LONG w = _mwidth(obj), h = _mheight(obj);
    LONG sx = cvx + SH_DX, sy = cvy + SH_DY;
    LONG y0, y1, x0, x1, y;
    LONG r0, g0, b0;

    if (!CyberGfxBase || cvw <= 0 || cvh <= 0) {
        return;
    }

    y0 = sy - SH_BLUR;  y1 = sy + cvh + SH_BLUR;
    x0 = sx - SH_BLUR;  x1 = sx + cvw + SH_BLUR;

    if (y0 < t)         { y0 = t; }
    if (y1 > t + h)     { y1 = t + h; }
    if (x0 < l)         { x0 = l; }
    if (x1 > l + w)     { x1 = l + w; }
    if (x1 - x0 > SH_MAXW || x1 <= x0 || y1 <= y0) {
        return;
    }

    r0 = (d->colour >> 16) & 0xff;
    g0 = (d->colour >>  8) & 0xff;
    b0 =  d->colour        & 0xff;

    for (y = y0; y < y1; y++) {
        LONG ay = sh_ramp(y, sy, sy + cvh - 1);
        LONG rel = y - t;
        LONG f = (h > 1) ? (h - 1 - rel) : 1;
        LONG n = (h > 1) ? (h - 1) : 1;
        ULONG bg;
        LONG x;

        if (ay <= 0) {
            continue;
        }

        /* Dieselbe Rechnung wie in build_gradient - sonst saesse der
         * Schatten auf einer anderen Grundfarbe als seine Umgebung. */
        bg = ((ULONG)(r0 * f / n) << 16)
           | ((ULONG)(g0 * f / n) <<  8)
           |  (ULONG)(b0 * f / n);

        for (x = x0; x < x1; x++) {
            LONG a = SH_ALPHA * sh_ramp(x, sx, sx + cvw - 1) / 255 * ay / 255;

            g_shadowrow[x - x0] = sh_darken(bg, a);
        }

        WritePixelArray(g_shadowrow, 0, 0, (UWORD)((x1 - x0) * 4), rp,
                        (UWORD)x0, (UWORD)y, (UWORD)(x1 - x0), 1,
                        RECTFMT_ARGB);
    }
}

static ULONG panel_draw(struct IClass *cl, Object *obj,
                        struct MUIP_Draw *msg)
{
    struct PanelData *d = INST_DATA(cl, obj);
    LONG l, t, w, h, cx, y;
    int i;

    DoSuperMethodA(cl, obj, (Msg)msg);
    if (!(msg->flags & MADF_DRAWOBJECT)) {
        return 0;
    }

    l = _mleft(obj);
    t = _mtop(obj);
    w = _mwidth(obj);
    h = _mheight(obj);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    cx = l + w / 2;

    draw_gradient(obj, d);

    /* Das Cover mittig ins obere Drittel. Nicht skaliert: es wird schon
     * mit size=300 vom Server geholt, und eine Skalierung auf 68k waere
     * teuer fuer wenig Gewinn. Passt es nicht, wird beschnitten. */
    /* Der Platz fuer die Textzeilen wird VORHER abgezogen, damit das
     * Cover nicht unten abgeschnitten wird. Genau das war der erste
     * Fehler: das Bild wurde in Originalgroesse gezeichnet und lief
     * unten aus der Flaeche heraus. */
    {
        struct TextFont *font = _font(obj);
        LONG lineh = font ? font->tf_YSize + 2 : 14;
        LONG textblock = NUM_LINES * lineh + 6 + 12;
        LONG box = h - textblock - 32;
        LONG iw, ih;

        if (box > w - 32) {
            box = w - 32;
        }

        /* Halbe Kantenlaenge, mit Absicht.
         *
         * Die Flaeche ganz auszufuellen sah schlechter aus, nicht
         * besser: die Quelle ist 300 Punkte gross, und alles darueber
         * hinaus rechnet der naechste Nachbar nur auseinander - es wird
         * grob, ohne mehr zu zeigen. Bei halber Groesse wird fast immer
         * VERKLEINERT, und das bleibt scharf. Der Leerraum darum ist
         * gewollt und gehoert zum Bild dieser Ansicht. */
        box /= 2;

        /* Cover und Textzeilen sitzen als EIN Block mittig in der
         * Flaeche. Vorher klebten sie oben, und bei einem aufgezogenen
         * Fenster blieben zwei Drittel leer. Dafuer muss die Bildhoehe
         * VOR dem Zeichnen feststehen - also erst skalieren, dann den
         * Anfang ausrechnen. */
        y = t + 16;
        if (box > 8 && scale_cover(d, box)) {
            LONG block = d->sc_h + 12 + textblock;
            LONG top = t + (h - block) / 2;

            if (top > y) {
                y = top;
            }
        }

        if (box > 8 && scale_cover(d, box)) {
            const UBYTE *src = d->sc ? d->sc : d->img.rgb;
            LONG srcw = d->sc ? d->sc_w : d->img.w;

            iw = d->sc_w;
            ih = d->sc_h;

            /* Erst der Schatten, dann das Bild darauf. */
            draw_shadow(obj, d, cx - iw / 2, y, iw, ih);

            /* Die Rohpixel unmittelbar in Truecolor schreiben.
             *
             * Vorher lief das ueber die vom datatype auf den Bildschirm
             * umgerechnete BitMap - und die war sichtbar gerastert, weil
             * picture.datatype dabei auf wenige Farben dithert
             * (PDTA_NumColors meldete 16). Ueber PDTM_READPIXELARRAY
             * kommen die Bildpunkte unveraendert. */
            WritePixelArray((APTR)src, 0, 0, (UWORD)(srcw * 3), _rp(obj),
                            (UWORD)(cx - iw / 2), (UWORD)y,
                            (UWORD)iw, (UWORD)ih, RECTFMT_RGB);
            y += ih;
        } else {
            y += box > 0 ? box : 0;
        }
    }

    y += 12;

    /* Textfarben ueber MUIs Stifte, nicht ueber RGB-Werte: das
     * funktioniert auf Truecolor genauso wie auf einem Palettenschirm,
     * und ein RPTAG_FgColor gibt es in den OS3-Headern nicht. */
    for (i = 0; i < NUM_LINES; i++) {
        ULONG pen = _pens(obj)[(i == LINE_TITLE) ? MPEN_SHINE
                                                 : MPEN_HALFSHINE];
        if (i == LINE_META) {
            y += 6;
        }
        y += draw_line(obj, d->line[i], cx, y, w - 16, pen,
                       i == LINE_TITLE) + 2;
    }

    return 0;
}

/* ------------------------------------------------------------------ */

static ULONG panel_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct PanelData *d;

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

    d->colour = GetTagData(MUIA_Panel_Colour, 0x00404040, msg->ops_AttrList);
    return (ULONG)obj;
}

static ULONG panel_dispose(struct IClass *cl, Object *obj, Msg msg)
{
    struct PanelData *d = INST_DATA(cl, obj);

    if (d->img_ok) {
        cover_unload(&d->img);
    }
    if (d->buf) {
        free(d->buf);
    }
    drop_scaled(d);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG panel_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct PanelData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;
    BOOL redraw = FALSE;

    while ((ti = NextTagItem(&tags)) != NULL) {
        switch (ti->ti_Tag) {
        case MUIA_Panel_Colour:
            if (d->colour != ti->ti_Data) {
                d->colour = ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_Panel_Cover:
            set_string(d->coverfile, sizeof(d->coverfile),
                       (const char *)ti->ti_Data);
            load_cover(d);
            redraw = TRUE;
            break;
        case MUIA_Panel_Title:
            set_string(d->line[LINE_TITLE], sizeof(d->line[0]),
                       (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        case MUIA_Panel_Artist:
            set_string(d->line[LINE_ARTIST], sizeof(d->line[0]),
                       (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        case MUIA_Panel_Album:
            set_string(d->line[LINE_ALBUM], sizeof(d->line[0]),
                       (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        case MUIA_Panel_Meta:
            set_string(d->line[LINE_META], sizeof(d->line[0]),
                       (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        }
    }

    if (redraw) {
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG panel_askminmax(struct IClass *cl, Object *obj,
                             struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);

    /* Breit genug fuer ein 300er Cover mit Rand, hoch genug fuer Cover
     * plus vier Textzeilen. Wachsen darf es beliebig. */
    msg->MinMaxInfo->MinWidth  += 320;
    msg->MinMaxInfo->MinHeight += 240;
    msg->MinMaxInfo->DefWidth  += 340;
    msg->MinMaxInfo->DefHeight += 420;
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MaxHeight += MUI_MAXMAX;
    return 0;
}

static ULONG panel_dispatch(struct IClass *cl  __asm("a0"),
                            Object       *obj __asm("a2"),
                            Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:         return panel_new(cl, obj, (struct opSet *)msg);
    case OM_DISPOSE:     return panel_dispose(cl, obj, msg);
    case OM_SET:         return panel_set(cl, obj, (struct opSet *)msg);
    case MUIM_AskMinMax: return panel_askminmax(cl, obj,
                                    (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:      return panel_draw(cl, obj,
                                    (struct MUIP_Draw *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

BOOL panel_init(void)
{
    CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
    if (!CyberGfxBase) {
        return FALSE;
    }

    /* MUIC_Area, NICHT MUIC_Group - siehe den Kopf von panel.h. */
    g_mcc = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                  sizeof(struct PanelData),
                                  (APTR)panel_dispatch);
    if (!g_mcc) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
        return FALSE;
    }
    return TRUE;
}

void panel_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
    if (CyberGfxBase) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
    }
}
