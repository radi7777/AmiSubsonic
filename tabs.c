/* AmiSubsonic - die Reiterleiste ueber der Liste. */

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

#include "tabs.h"

extern struct Library *CyberGfxBase;     /* gehoert panel.c */

static struct MUI_CustomClass *g_mcc = NULL;

#define TB_PAD_Y     6      /* Luft ueber und unter der Schrift */
#define TB_UNDER     2      /* Dicke des Strichs unter dem aktiven Reiter */

/* Leerraum UNTER dem Strich, bevor die Liste anfaengt. Er gehoert zur
 * Leiste und nicht zur Liste: die Leiste hat denselben Vollton, mit dem
 * der Verlauf der Liste oben beginnt, also sieht man nur eine ruhige
 * Zeile Abstand - und die Liste muss nichts von einem Rand wissen. */
#define TB_GAP      12

struct TBData {
    const char **titles;        /* fremdes Feld, nicht kopiert */
    int    count;

    ULONG  colour;
    LONG   active;
    LONG   pressed;

    struct MUI_EventHandlerNode ehn;
};

struct MUI_CustomClass *tb_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */

static LONG bar_height(Object *obj)
{
    struct TextFont *font = _font(obj);

    return (font ? font->tf_YSize : 8) + 2 * TB_PAD_Y + TB_UNDER + TB_GAP;
}

/* Oberkante des Strichs, gemessen von der Oberkante der Leiste. Fest
 * unter der Schrift, NICHT am unteren Rand - darunter kommt noch die
 * Leerzeile. */
static LONG under_y(Object *obj)
{
    struct TextFont *font = _font(obj);

    return (font ? font->tf_YSize : 8) + 2 * TB_PAD_Y;
}

/* Alle Reiter gleich breit. Bei zwei Beschriftungen sehr unterschiedlicher
 * Laenge waere eine Aufteilung nach Textbreite genauer, aber die Leiste
 * soll ruhig wirken und nicht bei jedem Albumwechsel springen. */
static LONG tab_width(Object *obj, struct TBData *d)
{
    LONG w = _mwidth(obj);

    return (d->count > 0) ? (w / d->count) : w;
}

static ULONG lighten(ULONG rgb, LONG add)
{
    LONG r = ((rgb >> 16) & 0xff) + add;
    LONG g = ((rgb >>  8) & 0xff) + add;
    LONG b = ( rgb        & 0xff) + add;

    if (r > 255) { r = 255; }
    if (g > 255) { g = 255; }
    if (b > 255) { b = 255; }
    if (r < 0)   { r = 0;   }
    if (g < 0)   { g = 0;   }
    if (b < 0)   { b = 0;   }

    return ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
}

static ULONG tb_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct TBData *d = INST_DATA(cl, obj);
    struct RastPort *rp;
    struct TextFont *font;
    LONG l, t, w, h, tw;
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

    rp = _rp(obj);

    /* Die Leiste sitzt unmittelbar ueber der Liste, und deren Verlauf
     * beginnt oben mit genau dieser Farbe. Ein Vollton in derselben Farbe
     * setzt die Leiste also nahtlos auf die Liste - man sieht eine
     * Flaeche, keinen aufgesetzten Kasten. */
    if (CyberGfxBase) {
        FillPixelArray(rp, (UWORD)l, (UWORD)t, (UWORD)w, (UWORD)h,
                       d->colour);
    }

    font = _font(obj);
    SetFont(rp, font);
    SetDrMd(rp, JAM1);

    tw = tab_width(obj, d);

    for (i = 0; i < d->count; i++) {
        const char *s = d->titles[i];
        LONG x = l + i * tw;
        LONG len, tx;
        ULONG pen;

        if (!s || !s[0] || !font) {
            continue;
        }
        len = (LONG)strlen(s);

        /* Mittig im eigenen Abschnitt. */
        tx = x + (tw - TextLength(rp, (STRPTR)s, (ULONG)len)) / 2;

        pen = _pens(obj)[(i == (int)d->active) ? MPEN_SHINE : MPEN_HALFSHINE];
        SetAPen(rp, pen);
        Move(rp, (WORD)tx, (WORD)(t + TB_PAD_Y + font->tf_Baseline));
        Text(rp, (STRPTR)s, (ULONG)len);

        /* Der aktive Reiter bekommt einen Strich darunter - so macht es
         * das Vorbild, und es kommt ohne zweite Flaechenfarbe aus. */
        if (i == (int)d->active && CyberGfxBase && tw > 16) {
            FillPixelArray(rp, (UWORD)(x + 8),
                           (UWORD)(t + under_y(obj)),
                           (UWORD)(tw - 16), TB_UNDER,
                           lighten(d->colour, 120));
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */

static BOOL in_object(Object *obj, LONG x, LONG y)
{
    return x >= _mleft(obj) && x <= _mright(obj)
        && y >= _mtop(obj)  && y <= _mbottom(obj);
}

static ULONG tb_handleevent(struct IClass *cl, Object *obj,
                            struct MUIP_HandleEvent *msg)
{
    struct TBData *d = INST_DATA(cl, obj);
    struct IntuiMessage *im = msg->imsg;
    LONG tw, i;

    if (!im) {
        return 0;
    }
    if (im->Class != IDCMP_MOUSEBUTTONS || im->Code != SELECTDOWN) {
        return 0;
    }
    if (!in_object(obj, im->MouseX, im->MouseY)) {
        return 0;
    }

    tw = tab_width(obj, d);
    if (tw <= 0) {
        return MUI_EventHandlerRC_Eat;
    }
    i = (im->MouseX - _mleft(obj)) / tw;
    /* Bei ungerader Breite bleibt rechts ein Rest, der rechnerisch hinter
     * dem letzten Reiter liegt. Ohne dieses Zurechtruecken tut ein Klick
     * auf die letzten Bildpunkte gar nichts. */
    if (i >= d->count) {
        i = d->count - 1;
    }
    if (i < 0) {
        return MUI_EventHandlerRC_Eat;
    }

    set(obj, MUIA_Tb_Active, i);
    /* IMMER setzen, auch auf denselben Wert - siehe sidebar.c. */
    set(obj, MUIA_Tb_Pressed, i);
    return MUI_EventHandlerRC_Eat;
}

/* ------------------------------------------------------------------ */

static ULONG tb_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct TBData *d;
    int n = 0;

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

    d->titles = (const char **)GetTagData(MUIA_Tb_Titles, 0,
                                          msg->ops_AttrList);
    d->colour = GetTagData(MUIA_Tb_Colour, 0x00281640, msg->ops_AttrList);
    d->active = (LONG)GetTagData(MUIA_Tb_Active, 0, msg->ops_AttrList);
    d->pressed = -1;

    if (d->titles) {
        while (d->titles[n]) {
            n++;
        }
    }
    d->count = n;
    return (ULONG)obj;
}

static ULONG tb_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct TBData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;
    BOOL redraw = FALSE;

    while ((ti = NextTagItem(&tags)) != NULL) {
        switch (ti->ti_Tag) {
        case MUIA_Tb_Colour:
            if (d->colour != ti->ti_Data) {
                d->colour = ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_Tb_Active:
            if (d->active != (LONG)ti->ti_Data) {
                d->active = (LONG)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_Tb_Pressed:
            d->pressed = (LONG)ti->ti_Data;
            break;
        }
    }

    if (redraw) {
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG tb_get(struct IClass *cl, Object *obj, struct opGet *msg)
{
    struct TBData *d = INST_DATA(cl, obj);

    switch (msg->opg_AttrID) {
    case MUIA_Tb_Active:  *msg->opg_Storage = (ULONG)d->active;  return TRUE;
    case MUIA_Tb_Pressed: *msg->opg_Storage = (ULONG)d->pressed; return TRUE;
    case MUIA_Tb_Colour:  *msg->opg_Storage = d->colour;         return TRUE;
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG tb_askminmax(struct IClass *cl, Object *obj,
                          struct MUIP_AskMinMax *msg)
{
    struct TBData *d = INST_DATA(cl, obj);
    LONG h;

    DoSuperMethodA(cl, obj, (Msg)msg);

    /* Feste Hoehe: die Leiste soll beim Aufziehen des Fensters nicht
     * mitwachsen, die Liste darunter schon. _font(obj) steht in
     * MUIM_AskMinMax noch nicht sicher zur Verfuegung, deshalb ueber
     * denselben Weg wie beim Zeichnen - und notfalls ein fester Wert. */
    h = bar_height(obj);
    if (h < 12) {
        h = 20;
    }

    msg->MinMaxInfo->MinWidth  += 80 * (d->count > 0 ? d->count : 1);
    msg->MinMaxInfo->DefWidth  += 120 * (d->count > 0 ? d->count : 1);
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MinHeight += h;
    msg->MinMaxInfo->DefHeight += h;
    msg->MinMaxInfo->MaxHeight += h;
    return 0;
}

static ULONG tb_setup(struct IClass *cl, Object *obj, Msg msg)
{
    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }
    MUI_RequestIDCMP(obj, IDCMP_MOUSEBUTTONS);
    return TRUE;
}

static ULONG tb_cleanupm(struct IClass *cl, Object *obj, Msg msg)
{
    MUI_RejectIDCMP(obj, IDCMP_MOUSEBUTTONS);
    return DoSuperMethodA(cl, obj, msg);
}

/* Erst beim Sichtbarwerden eintragen - die Leiste liegt auf der
 * Playerseite, und solange die Bildwand zu sehen ist, hat sie dort
 * nichts zu fangen. Begruendung ausfuehrlich in albumgrid.c. */
static ULONG tb_show(struct IClass *cl, Object *obj, Msg msg)
{
    struct TBData *d = INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }

    d->ehn.ehn_Object   = obj;
    d->ehn.ehn_Class    = cl;
    d->ehn.ehn_Events   = IDCMP_MOUSEBUTTONS;
    d->ehn.ehn_Flags    = 0;
    d->ehn.ehn_Priority = 0;
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, (ULONG)&d->ehn);
    return TRUE;
}

static ULONG tb_hide(struct IClass *cl, Object *obj, Msg msg)
{
    struct TBData *d = INST_DATA(cl, obj);

    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG tb_dispatch(struct IClass *cl  __asm("a0"),
                         Object       *obj __asm("a2"),
                         Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:            return tb_new(cl, obj, (struct opSet *)msg);
    case OM_SET:            return tb_set(cl, obj, (struct opSet *)msg);
    case OM_GET:            return tb_get(cl, obj, (struct opGet *)msg);
    case MUIM_AskMinMax:    return tb_askminmax(cl, obj,
                                    (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:         return tb_draw(cl, obj,
                                    (struct MUIP_Draw *)msg);
    case MUIM_Setup:        return tb_setup(cl, obj, msg);
    case MUIM_Cleanup:      return tb_cleanupm(cl, obj, msg);
    case MUIM_Show:         return tb_show(cl, obj, msg);
    case MUIM_Hide:         return tb_hide(cl, obj, msg);
    case MUIM_HandleEvent:  return tb_handleevent(cl, obj,
                                    (struct MUIP_HandleEvent *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

BOOL tb_init(void)
{
    g_mcc = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                  sizeof(struct TBData),
                                  (APTR)tb_dispatch);
    return g_mcc != NULL;
}

void tb_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
}
