/* AmiSubsonic - die schwarze Seitenleiste links. */

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

#include "sidebar.h"

extern struct Library *CyberGfxBase;     /* gehoert panel.c */

static struct MUI_CustomClass *g_mcc = NULL;

#define SB_WIDTH   150
#define SB_PAD_X    14
#define SB_PAD_Y     4
#define SB_TOP      10

/* Art eines Eintrags. */
#define E_HEAD   0      /* Ueberschrift, nicht waehlbar */
#define E_ITEM   1      /* waehlbar */
#define E_DEAD   2      /* gezeichnet, aber noch ohne Funktion */
#define E_GAP    3      /* Leerzeile */

struct Entry {
    const char *label;
    int         type;
    int         id;             /* SB_..., sonst -1 */
};

/* Die Gliederung des Programms - fuenf Ansichten und die Einstellungen.
 * Kein E_DEAD mehr: was hier steht, laesst sich anklicken.
 *
 * Settings steht unten und ausserhalb der Ueberschrift, weil es keine
 * Ansicht der Bibliothek ist, sondern ein eigenes Fenster oeffnet. */
static const struct Entry g_entries[] = {
    { "Navidrome",   E_HEAD, -1 },
    { "Home",        E_ITEM, SB_HOME },
    { "Albums",      E_ITEM, SB_ALBUMS },
    { "Tracks",      E_ITEM, SB_TRACKS },
    { "Favorites",   E_ITEM, SB_FAVORITES },
    { "Radio",       E_ITEM, SB_RADIO },
    { "",            E_GAP,  -1 },

    /* Der eigene Bestand von der Platte, gleich gegliedert wie die
     * Serverseite - deshalb eine eigene Ueberschrift und darunter
     * dieselben zwei Begriffe. */
    { "Folder",      E_HEAD, -1 },
    { "Albums",      E_ITEM, SB_FOLDER_ALBUMS },
    { "Tracks",      E_ITEM, SB_FOLDER_TRACKS },
    { "",            E_GAP,  -1 },
    { "Scan",        E_ITEM, SB_SCAN },
    { "Settings",    E_ITEM, SB_SETTINGS },
};

#define SB_COUNT ((int)(sizeof(g_entries) / sizeof(g_entries[0])))

struct SBData {
    LONG   active;              /* SB_..., nicht der Zeilenindex */
    LONG   pressed;

    struct MUI_EventHandlerNode ehn;
};

struct MUI_CustomClass *sb_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */

static LONG row_height(Object *obj)
{
    struct TextFont *font = _font(obj);

    return (font ? font->tf_YSize : 8) + 2 * SB_PAD_Y;
}

static ULONG sb_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct SBData *d = INST_DATA(cl, obj);
    struct RastPort *rp;
    struct TextFont *font;
    LONG l, t, w, h, rh, y;
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

    /* Durchgehend schwarz, wie die Bedienleiste unten - unabhaengig vom
     * Cover. Die Leiste ist der ruhende Rahmen, in dem die Farbe der
     * Inhalte wirkt. */
    if (CyberGfxBase) {
        FillPixelArray(rp, (UWORD)l, (UWORD)t, (UWORD)w, (UWORD)h,
                       0x00000000);
    }

    font = _font(obj);
    SetFont(rp, font);
    SetDrMd(rp, JAM1);

    rh = row_height(obj);
    y  = t + SB_TOP;

    for (i = 0; i < SB_COUNT; i++) {
        const struct Entry *e = &g_entries[i];
        ULONG pen;
        LONG len;

        if (y + rh > t + h) {
            break;
        }

        if (e->type == E_GAP) {
            y += rh / 2;
            continue;
        }

        if (e->type == E_ITEM && e->id == (int)d->active && CyberGfxBase) {
            /* Ein schmaler blauer Balken links statt eines gefaerbten
             * Textes: eine eigene Textfarbe braeuchte ObtainBestPen und
             * damit eine Farbe, die wieder freigegeben werden muss.
             * FillPixelArray nimmt den RGB-Wert unmittelbar. */
            FillPixelArray(rp, (UWORD)(l + 4), (UWORD)(y + 2),
                           3, (UWORD)(rh - 4), 0x004C7CFE);
        }

        switch (e->type) {
        case E_HEAD:
            pen = _pens(obj)[MPEN_HALFSHINE];
            break;
        case E_DEAD:
            pen = _pens(obj)[MPEN_HALFSHADOW];
            break;
        default:
            pen = _pens(obj)[(e->id == (int)d->active)
                             ? MPEN_SHINE : MPEN_HALFSHINE];
            break;
        }

        len = (LONG)strlen(e->label);
        if (len > 0 && font) {
            struct TextExtent te;
            ULONG fit = TextFit(rp, (STRPTR)e->label, (ULONG)len, &te, NULL,
                                1, (UWORD)(w - 2 * SB_PAD_X),
                                (UWORD)font->tf_YSize);

            if (fit < (ULONG)len) {
                len = (LONG)fit;
            }
            SetAPen(rp, pen);

            /* Ueberschriften fett. SetSoftStyle rechnet das Fett aus dem
             * vorhandenen Zeichensatz aus - es braucht also keinen
             * zweiten Font, der auf einer fremden Maschine womoeglich
             * gar nicht da ist.
             *
             * AskSoftStyle fragt vorher, WELCHE Stile dieser Font
             * ueberhaupt nachbilden kann; was er nicht kann, setzt
             * SetSoftStyle stillschweigend nicht. Zurueckgesetzt wird
             * gleich danach, sonst waere alles Folgende ebenfalls
             * fett. */
            if (e->type == E_HEAD) {
                SetSoftStyle(rp, FSF_BOLD, AskSoftStyle(rp));
            }
            Move(rp, (WORD)(l + SB_PAD_X),
                 (WORD)(y + SB_PAD_Y + font->tf_Baseline));
            Text(rp, (STRPTR)e->label, (ULONG)len);
            if (e->type == E_HEAD) {
                SetSoftStyle(rp, FS_NORMAL, AskSoftStyle(rp));
            }
        }

        y += rh;
        if (e->type == E_HEAD) {
            y += SB_PAD_Y;
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

/* Welcher Eintrag liegt an dieser Bildschirmhoehe? Gerechnet wird
 * genauso wie gezeichnet - Ueberschriften und Luecken verschieben die
 * Zeilen, eine einfache Division durch die Zeilenhoehe waere falsch. */
static int entry_at(Object *obj, LONG my)
{
    LONG rh = row_height(obj);
    LONG y  = _mtop(obj) + SB_TOP;
    int i;

    for (i = 0; i < SB_COUNT; i++) {
        const struct Entry *e = &g_entries[i];

        if (e->type == E_GAP) {
            y += rh / 2;
            continue;
        }
        if (my >= y && my < y + rh) {
            return i;
        }
        y += rh;
        if (e->type == E_HEAD) {
            y += SB_PAD_Y;
        }
    }
    return -1;
}

static ULONG sb_handleevent(struct IClass *cl, Object *obj,
                            struct MUIP_HandleEvent *msg)
{
    struct IntuiMessage *im = msg->imsg;
    int i;

    if (!im) {
        return 0;
    }
    if (im->Class != IDCMP_MOUSEBUTTONS || im->Code != SELECTDOWN) {
        return 0;
    }
    if (!in_object(obj, im->MouseX, im->MouseY)) {
        return 0;
    }

    i = entry_at(obj, im->MouseY);
    if (i < 0 || g_entries[i].type != E_ITEM) {
        return MUI_EventHandlerRC_Eat;
    }

    set(obj, MUIA_Sb_Active, g_entries[i].id);
    /* IMMER setzen, auch auf denselben Wert: die Meldung soll auch dann
     * kommen, wenn der Anwender denselben Eintrag noch einmal antippt. */
    set(obj, MUIA_Sb_Pressed, g_entries[i].id);
    return MUI_EventHandlerRC_Eat;
}

/* ------------------------------------------------------------------ */

static ULONG sb_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct SBData *d;

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

    d->active  = (LONG)GetTagData(MUIA_Sb_Active, SB_ALBUMS,
                                  msg->ops_AttrList);
    d->pressed = -1;
    return (ULONG)obj;
}

static ULONG sb_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct SBData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;
    BOOL redraw = FALSE;

    while ((ti = NextTagItem(&tags)) != NULL) {
        switch (ti->ti_Tag) {
        case MUIA_Sb_Active:
            if (d->active != (LONG)ti->ti_Data) {
                d->active = (LONG)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_Sb_Pressed:
            d->pressed = (LONG)ti->ti_Data;
            break;
        }
    }

    if (redraw) {
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG sb_get(struct IClass *cl, Object *obj, struct opGet *msg)
{
    struct SBData *d = INST_DATA(cl, obj);

    switch (msg->opg_AttrID) {
    case MUIA_Sb_Active:  *msg->opg_Storage = (ULONG)d->active;  return TRUE;
    case MUIA_Sb_Pressed: *msg->opg_Storage = (ULONG)d->pressed; return TRUE;
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG sb_askminmax(struct IClass *cl, Object *obj,
                          struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);

    /* Feste Breite: Min == Max. Die Leiste soll beim Vergroessern des
     * Fensters stehen bleiben, alles andere waechst. */
    msg->MinMaxInfo->MinWidth  += SB_WIDTH;
    msg->MinMaxInfo->DefWidth  += SB_WIDTH;
    msg->MinMaxInfo->MaxWidth  += SB_WIDTH;
    msg->MinMaxInfo->MinHeight += 100;
    msg->MinMaxInfo->DefHeight += 300;
    msg->MinMaxInfo->MaxHeight += MUI_MAXMAX;
    return 0;
}

static ULONG sb_setup(struct IClass *cl, Object *obj, Msg msg)
{
    struct SBData *d = INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }
    MUI_RequestIDCMP(obj, IDCMP_MOUSEBUTTONS);

    d->ehn.ehn_Object   = obj;
    d->ehn.ehn_Class    = cl;
    d->ehn.ehn_Events   = IDCMP_MOUSEBUTTONS;
    d->ehn.ehn_Flags    = 0;
    d->ehn.ehn_Priority = 0;
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, (ULONG)&d->ehn);
    return TRUE;
}

static ULONG sb_cleanupm(struct IClass *cl, Object *obj, Msg msg)
{
    struct SBData *d = INST_DATA(cl, obj);

    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
    MUI_RejectIDCMP(obj, IDCMP_MOUSEBUTTONS);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG sb_dispatch(struct IClass *cl  __asm("a0"),
                         Object       *obj __asm("a2"),
                         Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:            return sb_new(cl, obj, (struct opSet *)msg);
    case OM_SET:            return sb_set(cl, obj, (struct opSet *)msg);
    case OM_GET:            return sb_get(cl, obj, (struct opGet *)msg);
    case MUIM_AskMinMax:    return sb_askminmax(cl, obj,
                                    (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:         return sb_draw(cl, obj,
                                    (struct MUIP_Draw *)msg);
    case MUIM_Setup:        return sb_setup(cl, obj, msg);
    case MUIM_Cleanup:      return sb_cleanupm(cl, obj, msg);
    case MUIM_HandleEvent:  return sb_handleevent(cl, obj,
                                    (struct MUIP_HandleEvent *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

BOOL sb_init(void)
{
    g_mcc = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                  sizeof(struct SBData),
                                  (APTR)sb_dispatch);
    return g_mcc != NULL;
}

void sb_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
}
