/* AmiSubsonic - die Bedienleiste unten, selbst gezeichnet. */

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

#include "player.h"

/* Voruebergehende Suchhilfe: schreibt bei jedem Klick in die Leiste eine
 * Zeile nach RAM:AmiSubsonic/pl.log - Mausposition, die geprueften
 * Trefferflaechen und was daraus wurde. */
#define PL_DEBUG_HIT 0

#if PL_DEBUG_HIT
#include <proto/dos.h>

static void hitlog(const char *fmt, LONG a, LONG b, LONG c, LONG d,
                   LONG e, LONG f)
{
    BPTR fh = Open((STRPTR)"RAM:AmiSubsonic/pl.log", MODE_READWRITE);

    if (!fh) {
        BPTR lock = CreateDir((STRPTR)"RAM:AmiSubsonic");

        if (lock) {
            UnLock(lock);
        }
        fh = Open((STRPTR)"RAM:AmiSubsonic/pl.log", MODE_READWRITE);
        if (!fh) {
            return;
        }
    }
    Seek(fh, 0, OFFSET_END);
    FPrintf(fh, (STRPTR)fmt, a, b, c, d, e, f);
    Close(fh);
}
#endif
#include "cover.h"
#include "amisub.h"

extern struct Library *CyberGfxBase;     /* gehoert panel.c */

static struct MUI_CustomClass *g_mcc = NULL;

#define PL_HEIGHT   52          /* Wunschhoehe der Leiste */
#define THUMB_PAD   6

/* Graustufen fuer die Symbole. Direkt als RGB und nicht ueber MUI-Stifte:
 * die Leiste ist immer schwarz, und auf RTG-Truecolor ist ein genauer
 * Grauwert genauso billig wie ein Stift - nur eben genau der gewuenschte. */
#define COL_ICON    0x00c8c8c8  /* Symbole */
#define COL_DIM     0x00707070  /* unbenutzte Teile, Balkenrest */
#define COL_WHITE   0x00ffffff

struct PlData {
    char  title[SUB_NAME_LEN];
    char  artist[SUB_ARTIST_LEN];
    char  album[SUB_ALBUM_LEN];
    char  coverfile[128];

    struct CoverImage img;
    BOOL   img_ok;
    UBYTE *thumb;               /* verkleinertes Cover */
    LONG   thumb_size;

    ULONG  colour;              /* Balkenfarbe */
    LONG   pos, total;
    BOOL   playing;
    LONG   volume;

    LONG   pressed;
    LONG   seekto;

    /* Der Eintrag in der Ereigniskette des Fensters. Muss die ganze Zeit
     * ueber leben, MUI verkettet ihn nur. */
    struct MUI_EventHandlerNode ehn;

    /* Beim Zeichnen berechnete Trefferflaechen, damit Klicks und
     * Zeichnen nicht zweimal dieselbe Rechnung machen und auseinander
     * laufen koennen. */
    struct { LONG x, y, w, h; } hit[10];
    LONG   bar_x, bar_w, bar_y;
    LONG   vol_x, vol_w, vol_y;

    /* Was seit dem letzten Zeichnen anders ist. upd_time meint Laufzeit
     * und Balken, upd_button den Abspielknopf - der wechselt nur beim
     * Umschalten zwischen Wiedergabe und Pause und hat im Sekundentakt
     * nichts zu suchen. */
    BOOL   upd_time;
    BOOL   upd_button;
};

struct MUI_CustomClass *pl_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */
/* Zeichenhilfen                                                       */
/* ------------------------------------------------------------------ */

static void fill_rect(Object *obj, LONG x, LONG y, LONG w, LONG h, ULONG rgb)
{
    if (w > 0 && h > 0 && CyberGfxBase) {
        FillPixelArray(_rp(obj), (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, rgb);
    }
}

/* Gefuelltes Dreieck, nach rechts oder links zeigend.
 *
 * Zeilenweise aus waagerechten Balken statt ueber AreaMove/AreaDraw: das
 * braeuchte einen TmpRas und einen AreaInfo am RastPort, und fuer ein
 * Symbol von zwoelf Pixeln ist das mehr Verwaltung als Rechnung. */
static void fill_triangle(Object *obj, LONG x, LONG y, LONG size,
                          BOOL right, ULONG rgb)
{
    LONG half = size / 2;
    LONG i;

    for (i = 0; i < size; i++) {
        LONG d = (i <= half) ? i : (size - 1 - i);   /* Breite dieser Zeile */
        LONG w = d + 1;

        if (right) {
            fill_rect(obj, x, y + i, w, 1, rgb);
        } else {
            fill_rect(obj, x + half - d, y + i, w, 1, rgb);
        }
    }
}

/* Gefuellter Kreis, ganzzahlig ueber x*x + y*y <= r*r. Kein Fliesskomma,
 * keine Wurzel - je Zeile eine Multiplikation und eine Schleife. */
static void fill_circle(Object *obj, LONG cx, LONG cy, LONG r, ULONG rgb)
{
    LONG y, x;

    for (y = -r; y <= r; y++) {
        LONG span = 0;

        for (x = r; x >= 0; x--) {
            if (x * x + y * y <= r * r) {
                span = x;
                break;
            }
        }
        if (span > 0) {
            fill_rect(obj, cx - span, cy + y, span * 2 + 1, 1, rgb);
        }
    }
}

/* Der Abspielknopf: geglaetteter weisser Kreis mit schwarzem Symbol.
 *
 * WARUM NICHT fill_circle()
 *
 * Der zeilenweise gefuellte Kreis hat harte Kanten, und bei Radius 13
 * sieht man jede Stufe - rund wirkt das nicht. Der Knopf ist aber das
 * auffaelligste Bedienelement der ganzen Leiste.
 *
 * Hier wird deshalb ueberabgetastet: jeder Bildpunkt wird in 4x4
 * Unterpunkte zerlegt, und wie viele davon im Kreis liegen, bestimmt die
 * Helligkeit. Gerechnet wird in ACHTELN eines Bildpunktes, alles
 * ganzzahlig - kein Fliesskomma, keine Wurzel, nur Quadrate und
 * Vergleiche.
 *
 * Der Hintergrund der Leiste ist immer Schwarz (siehe pl_draw), deshalb
 * ist die Mischung eine reine Multiplikation: Weiss mal Deckung. Ein
 * echtes Mischen mit dem Untergrund braeuchte den Untergrund - hier
 * waere das ein Auslesen der Grafikkarte je Bildpunkt.
 *
 * Am Ende geht alles in EINEM WritePixelArray heraus. */
#define BTN_RMAX  20
static ULONG g_btnbuf[(2 * BTN_RMAX + 1) * (2 * BTN_RMAX + 1)];

static void draw_play_button(Object *obj, LONG cx, LONG cy, LONG r,
                             BOOL playing)
{
    LONG d, px, py, i, j;
    LONG r8, rr;
    /* Das Symbol in Achteln, gemessen von der Mitte. */
    LONG hy  = r * 8 * 11 / 20;     /* halbe Hoehe des Dreiecks */
    LONG tx0 = -(r * 8 * 2 / 5);    /* linke Kante */
    LONG tx1 =  (r * 8 * 3 / 5);    /* Spitze */
    LONG bw  = r * 8 * 3 / 20;      /* halbe Breite eines Pausenbalkens */
    LONG bg  = r * 8 * 2 / 10;      /* Abstand der Balken von der Mitte */

    if (!CyberGfxBase || r < 4 || r > BTN_RMAX) {
        /* Notnagel: lieber der alte, kantige Kreis als gar kein Knopf. */
        fill_circle(obj, cx, cy, r, COL_WHITE);
        return;
    }

    d  = 2 * r + 1;
    r8 = r * 8;
    rr = r8 * r8;

    for (py = 0; py < d; py++) {
        for (px = 0; px < d; px++) {
            LONG inside = 0, onsym = 0;

            for (j = 0; j < 4; j++) {
                /* Mitte des Unterpunktes in Achteln: vier Unterpunkte je
                 * Bildpunkt liegen bei 1/8, 3/8, 5/8 und 7/8. */
                LONG fy = (py - r) * 8 + 2 * j + 1;

                for (i = 0; i < 4; i++) {
                    LONG fx = (px - r) * 8 + 2 * i + 1;

                    if (fx * fx + fy * fy > rr) {
                        continue;
                    }
                    inside++;

                    if (playing) {
                        /* Zwei Balken. */
                        LONG ax = fx < 0 ? -fx : fx;

                        if (ax >= bg && ax <= bg + 2 * bw
                                && fy >= -hy && fy <= hy) {
                            onsym++;
                        }
                    } else {
                        /* Dreieck nach rechts: fuer jede Hoehe endet es
                         * bei einer Kante, die von der Grundseite zur
                         * Spitze laeuft. */
                        LONG ay = fy < 0 ? -fy : fy;

                        if (ay <= hy && fx >= tx0
                                && fx <= tx1 - (tx1 - tx0) * ay / hy) {
                            onsym++;
                        }
                    }
                }
            }

            {
                /* Deckung 0..16, das Symbol zieht wieder ab. */
                LONG cov = inside - onsym;
                LONG v;

                if (cov < 0) {
                    cov = 0;
                }
                v = 255 * cov / 16;
                g_btnbuf[py * d + px] =
                    ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;
            }
        }
    }

    WritePixelArray(g_btnbuf, 0, 0, (UWORD)(d * 4), _rp(obj),
                    (UWORD)(cx - r), (UWORD)(cy - r),
                    (UWORD)d, (UWORD)d, RECTFMT_ARGB);
}

static void draw_text(Object *obj, const char *s, LONG x, LONG y,
                      LONG maxw, ULONG pen, BOOL bold)
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

    SetFont(rp, font);
    SetDrMd(rp, JAM1);
    SetAPen(rp, pen);
    SetSoftStyle(rp, bold ? FSF_BOLD : FS_NORMAL, AskSoftStyle(rp));

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

/* ------------------------------------------------------------------ */
/* Miniatur-Cover                                                      */
/* ------------------------------------------------------------------ */

static void drop_thumb(struct PlData *d)
{
    if (d->thumb) {
        free(d->thumb);
        d->thumb = NULL;
    }
    d->thumb_size = 0;
}

static void load_cover(struct PlData *d)
{
    drop_thumb(d);
    if (d->img_ok) {
        cover_unload(&d->img);
        d->img_ok = FALSE;
    }
    if (d->coverfile[0]) {
        d->img_ok = cover_load(d->coverfile, &d->img);
    }
}

/* Verkleinert auf ein Quadrat. Naechster Nachbar - bei vierzig Pixeln
 * Kantenlaenge faellt Mitteln ohnehin nicht auf, kostet aber je Zielpunkt
 * vier Quellpunkte. */
static BOOL make_thumb(struct PlData *d, LONG size)
{
    LONG x, y;
    UBYTE *dst;

    if (!d->img_ok || !d->img.rgb || size <= 0) {
        return FALSE;
    }
    if (d->thumb && d->thumb_size == size) {
        return TRUE;
    }

    dst = malloc((size_t)size * (size_t)size * 3);
    if (!dst) {
        return FALSE;
    }

    for (y = 0; y < size; y++) {
        const UBYTE *srow = d->img.rgb
                          + (long)(y * d->img.h / size) * d->img.w * 3;
        UBYTE *drow = dst + (long)y * size * 3;

        for (x = 0; x < size; x++) {
            const UBYTE *s = srow + (long)(x * d->img.w / size) * 3;
            *drow++ = s[0];
            *drow++ = s[1];
            *drow++ = s[2];
        }
    }

    drop_thumb(d);
    d->thumb = dst;
    d->thumb_size = size;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static void set_hit(struct PlData *d, int id, LONG x, LONG y, LONG w, LONG h)
{
    d->hit[id].x = x;
    d->hit[id].y = y;
    d->hit[id].w = w;
    d->hit[id].h = h;
}

/* Die Mittelgruppe: Transportknoepfe, Zeiten und Fortschrittsbalken.
 *
 * Als eigene Funktion, weil sie an ZWEI Stellen gebraucht wird - beim
 * vollstaendigen Zeichnen und beim Auffrischen im Sekundentakt. Zweimal
 * dieselbe Rechnung waere die sichere Art, Trefferflaechen und Zeichnung
 * auseinanderlaufen zu lassen. */
static void draw_progress(Object *obj, struct PlData *d, LONG fh);

static void draw_center(Object *obj, struct PlData *d,
                        LONG l, LONG t, LONG w, LONG cx, LONG by,
                        LONG icon, LONG fh)
{

    /* DREI Knoepfe, mehr nicht: zurueck, abspielen, vor.
     *
     * Stop, Zufall und Wiederholen sind bewusst weg. Zufall und
     * Wiederholen taten ohnehin nichts Sinnvolles, und Stop ist neben
     * einer Pausetaste eine Verdopplung. Ihre Trefferflaechen werden
     * unten ausdruecklich geleert - stehen gebliebene Koordinaten aus
     * einem frueheren Zeichnen wuerden sonst weiter Klicks fangen, an
     * einer Stelle, wo nichts mehr zu sehen ist. */
    set_hit(d, PLB_STOP,    0, 0, 0, 0);
    set_hit(d, PLB_SHUFFLE, 0, 0, 0, 0);
    set_hit(d, PLB_REPEAT,  0, 0, 0, 0);

    /* Vorheriger: Strich plus Dreieck nach links. */
    set_hit(d, PLB_PREV, cx - 56, by - 4, 24, icon + 8);
    fill_rect(obj, cx - 52, by, 2, icon, COL_ICON);
    fill_triangle(obj, cx - 49, by, icon, FALSE, COL_ICON);

    /* Play/Pause: geglaetteter weisser Kreis, Symbol schon darin. */
    set_hit(d, PLB_PLAY, cx - 15, by - 4, 30, 30);
    draw_play_button(obj, cx, by + 11, 15, d->playing);

    /* Naechster: Dreieck nach rechts plus Strich. */
    set_hit(d, PLB_NEXT, cx + 34, by - 4, 24, icon + 8);
    fill_triangle(obj, cx + 38, by, icon, TRUE, COL_ICON);
    fill_rect(obj, cx + 38 + icon, by, 2, icon, COL_ICON);

    /* ---- Mitte unten: Zeit und Fortschritt ---- */
    /* Der Balken sitzt MITTIG unter den Knoepfen, nicht an einem
     * Drittelpunkt - so steht die ganze Mittelgruppe als Einheit da,
     * wie in der Vorlage. */
    d->bar_y = by + 30;
    d->bar_w = w / 3;
    if (d->bar_w < 60) {
        d->bar_w = 60;
    }
    if (d->bar_w > 300) {
        d->bar_w = 300;
    }
    d->bar_x = cx - d->bar_w / 2;

    /* Zeiten und Balken zeichnet dieselbe Funktion wie beim Auffrischen -
     * eine zweite Fassung hier waere die sichere Art, beide Wege
     * auseinanderlaufen zu lassen. */
    draw_progress(obj, d, fh);

    set_hit(d, PLB_SEEK, d->bar_x, d->bar_y - 6, d->bar_w, 16);
}

/* Zeiten und Fortschrittsbalken auffrischen - OHNE die Flaeche vorher zu
 * loeschen.
 *
 * Der erste Anlauf hat die ganze Mittelgruppe schwarz uebermalt und dann
 * neu gezeichnet. Genau das war weiter als Flackern zu sehen: zwischen
 * Loeschen und Zeichnen liegt ein Bildaufbau, in dem die Flaeche leer
 * ist. Einmal je Sekunde reicht, damit das Auge es erwischt.
 *
 * Jetzt wird DECKEND gezeichnet:
 *
 *   - Der Balken besteht aus zwei aneinandergrenzenden Fuellungen, dem
 *     gespielten und dem uebrigen Teil. Zusammen decken sie ihn
 *     vollstaendig, es bleibt kein Moment, in dem er leer waere.
 *   - Nur die beiden Zeitzellen muessen geloescht werden, weil Text
 *     nicht deckend ist. Das sind zwei Rechtecke in Textgroesse statt
 *     der halben Leiste.
 *   - Der Abspielknopf wird gar nicht angefasst; er aendert sich nur
 *     beim Umschalten und wird dann als vollstaendiges Bild
 *     hineingeschrieben, das seinen eigenen Hintergrund mitbringt. */
static void draw_progress(Object *obj, struct PlData *d, LONG fh)
{
    char a[16], b[16];
    LONG done = 0;

    if (d->bar_w <= 0) {
        return;
    }

    duration_text(d->pos, a, sizeof(a));
    duration_text(d->total > 0 ? d->total : 0, b, sizeof(b));

    fill_rect(obj, d->bar_x - 36, d->bar_y - fh / 2, 34, fh, 0x00000000);
    fill_rect(obj, d->bar_x + d->bar_w + 6, d->bar_y - fh / 2, 34, fh,
              0x00000000);

    draw_text(obj, a, d->bar_x - 36, d->bar_y - fh / 2, 34,
              _pens(obj)[MPEN_HALFSHINE], FALSE);
    draw_text(obj, b, d->bar_x + d->bar_w + 6, d->bar_y - fh / 2, 34,
              _pens(obj)[MPEN_HALFSHINE], FALSE);

    if (d->total > 0 && d->pos > 0) {
        done = d->bar_w * d->pos / d->total;
        if (done > d->bar_w) {
            done = d->bar_w;
        }
    }
    if (done > 0) {
        fill_rect(obj, d->bar_x, d->bar_y, done, 4, COL_WHITE);
    }
    if (done < d->bar_w) {
        fill_rect(obj, d->bar_x + done, d->bar_y, d->bar_w - done, 4,
                  COL_DIM);
    }
}

static ULONG pl_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct PlData *d = INST_DATA(cl, obj);
    struct TextFont *font = _font(obj);
    LONG l, t, w, h, fh;
    LONG tx, ty, thumb;
    LONG cx, by, icon;

    DoSuperMethodA(cl, obj, (Msg)msg);

    l = _mleft(obj);
    t = _mtop(obj);
    w = _mwidth(obj);
    h = _mheight(obj);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    fh = font ? font->tf_YSize : 8;

    /* TEILAUFFRISCHUNG.
     *
     * Die Laufzeit aendert sich jede Sekunde. Die ganze Leiste dafuer neu
     * zu zeichnen heisst: Miniatur skalieren, drei Textzeilen setzen,
     * Lautstaerkeregler malen - jede Sekunde, obwohl sich nichts davon
     * geaendert hat. Sichtbar wurde das als Flackern.
     *
     * MUI bietet dafuer MADF_DRAWUPDATE: das Objekt wird gebeten, nur
     * seine Aenderung nachzuziehen. Hier ist das die Mittelgruppe -
     * schwarz uebermalen, neu zeichnen, fertig. */
    if ((msg->flags & MADF_DRAWUPDATE) && !(msg->flags & MADF_DRAWOBJECT)
            && (d->upd_time || d->upd_button)) {
        if (d->upd_button) {
            /* Der Knopf bringt seinen eigenen Hintergrund mit - ein
             * vollstaendiges Bild, das alles ueberschreibt, was da war.
             * Deshalb hier kein Loeschen. */
            draw_play_button(obj, l + w / 2, t + 8 + 11, 15, d->playing);
            d->upd_button = FALSE;
        }
        if (d->upd_time) {
            draw_progress(obj, d, fh);
            d->upd_time = FALSE;
        }
        return 0;
    }

    d->upd_time   = FALSE;
    d->upd_button = FALSE;

    if (!(msg->flags & MADF_DRAWOBJECT)) {
        return 0;
    }

    /* Grundfarbe: immer Schwarz, unabhaengig vom Album. */
    fill_rect(obj, l, t, w, h, 0x00000000);

    /* ---- links: Miniatur und die drei Zeilen ---- */
    thumb = h - 2 * THUMB_PAD;
    if (thumb > 48) {
        thumb = 48;
    }
    if (d->img_ok && make_thumb(d, thumb) && CyberGfxBase) {
        WritePixelArray(d->thumb, 0, 0, (UWORD)(thumb * 3), _rp(obj),
                        (UWORD)(l + THUMB_PAD), (UWORD)(t + THUMB_PAD),
                        (UWORD)thumb, (UWORD)thumb, RECTFMT_RGB);
    }
    /* Anklickbar auch ohne Bild - ein Sender ohne Cover soll trotzdem
     * zu seiner Liste fuehren. */
    set_hit(d, PLB_COVER, l + THUMB_PAD, t + THUMB_PAD, thumb, thumb);

    tx = l + THUMB_PAD + thumb + 8;
    ty = t + (h - 3 * fh - 4) / 2;
    {
        /* Der Textblock darf hoechstens bis kurz vor die Knoepfe. */
        LONG tw = (w / 3) - (tx - l) - 8;

        draw_text(obj, d->title,  tx, ty,              tw,
                  _pens(obj)[MPEN_SHINE], TRUE);
        draw_text(obj, d->artist, tx, ty + fh + 2,     tw,
                  _pens(obj)[MPEN_HALFSHINE], FALSE);
        draw_text(obj, d->album,  tx, ty + 2 * fh + 4, tw,
                  _pens(obj)[MPEN_HALFSHINE], FALSE);
    }

    /* ---- Mitte: Knoepfe ---- */
    cx = l + w / 2;
    by = t + 8;
    icon = 12;

    draw_center(obj, d, l, t, w, cx, by, icon, fh);
    /* ---- rechts: Lautstaerke ---- */
    d->vol_w = 80;
    d->vol_x = l + w - d->vol_w - 12;
    d->vol_y = t + h / 2 - 2;

    /* Lautsprecher, angedeutet: ein kleines Trapez. */
    fill_rect(obj, d->vol_x - 22, d->vol_y - 3, 4, 6, COL_ICON);
    fill_triangle(obj, d->vol_x - 18, d->vol_y - 6, 12, FALSE, COL_ICON);

    fill_rect(obj, d->vol_x, d->vol_y, d->vol_w, 4, COL_DIM);
    {
        /* AmigaAMP kennt 0 bis 26 - siehe AREXX.readme. */
        LONG done = d->vol_w * d->volume / 26;

        if (done < 0) { done = 0; }
        if (done > d->vol_w) { done = d->vol_w; }
        fill_rect(obj, d->vol_x, d->vol_y, done, 4, COL_WHITE);
        fill_circle(obj, d->vol_x + done, d->vol_y + 2, 4, COL_WHITE);
    }
    set_hit(d, PLB_VOLUME, d->vol_x - 6, d->vol_y - 8, d->vol_w + 12, 20);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Eingaben                                                            */
/* ------------------------------------------------------------------ */

static BOOL in_rect(struct PlData *d, int id, LONG x, LONG y)
{
    return x >= d->hit[id].x && x < d->hit[id].x + d->hit[id].w
        && y >= d->hit[id].y && y < d->hit[id].y + d->hit[id].h;
}

static ULONG pl_handleevent(struct IClass *cl, Object *obj,
                            struct MUIP_HandleEvent *msg)
{
    struct PlData *d = INST_DATA(cl, obj);
    struct IntuiMessage *im = msg->imsg;
    int i;

    if (!im || im->Class != IDCMP_MOUSEBUTTONS || im->Code != SELECTDOWN) {
        return 0;
    }
    if (im->MouseX < _mleft(obj) || im->MouseX > _mright(obj)
            || im->MouseY < _mtop(obj) || im->MouseY > _mbottom(obj)) {
        return 0;
    }

#if PL_DEBUG_HIT
    hitlog("klick x=%ld y=%ld | play x=%ld y=%ld w=%ld h=%ld\n",
           (LONG)im->MouseX, (LONG)im->MouseY,
           d->hit[PLB_PLAY].x, d->hit[PLB_PLAY].y,
           d->hit[PLB_PLAY].w, d->hit[PLB_PLAY].h);
    hitlog("        seek x=%ld y=%ld w=%ld h=%ld total=%ld\n",
           d->hit[PLB_SEEK].x, d->hit[PLB_SEEK].y,
           d->hit[PLB_SEEK].w, d->hit[PLB_SEEK].h, d->total, 0);
#endif

    /* Fortschrittsbalken: die angeklickte Stelle in Sekunden umrechnen. */
    if (in_rect(d, PLB_SEEK, im->MouseX, im->MouseY) && d->total > 0) {
        LONG rel = im->MouseX - d->bar_x;

        if (rel < 0) { rel = 0; }
        if (rel > d->bar_w) { rel = d->bar_w; }
        d->seekto = d->total * rel / d->bar_w;
#if PL_DEBUG_HIT
        hitlog("        -> SEEK auf %ld s\n", d->seekto, 0, 0, 0, 0, 0);
#endif
        set(obj, MUIA_Pl_Pressed, PLB_SEEK);
        return MUI_EventHandlerRC_Eat;
    }

    if (in_rect(d, PLB_VOLUME, im->MouseX, im->MouseY)) {
        LONG rel = im->MouseX - d->vol_x;

        if (rel < 0) { rel = 0; }
        if (rel > d->vol_w) { rel = d->vol_w; }
        d->volume = 26 * rel / d->vol_w;
        MUI_Redraw(obj, MADF_DRAWOBJECT);
#if PL_DEBUG_HIT
        hitlog("        -> LAUTSTAERKE %ld\n", d->volume, 0, 0, 0, 0, 0);
#endif
        set(obj, MUIA_Pl_Pressed, PLB_VOLUME);
        return MUI_EventHandlerRC_Eat;
    }

    if (in_rect(d, PLB_COVER, im->MouseX, im->MouseY)) {
        set(obj, MUIA_Pl_Pressed, PLB_COVER);
        return MUI_EventHandlerRC_Eat;
    }

    for (i = PLB_STOP; i <= PLB_REPEAT; i++) {
        if (in_rect(d, i, im->MouseX, im->MouseY)) {
#if PL_DEBUG_HIT
            hitlog("        -> knopf %ld\n", (LONG)i, 0, 0, 0, 0, 0);
#endif
            /* Ueber set() und damit durch OM_SET: nur so bekommt MUIs
             * Notify-Oberklasse die Aenderung zu sehen. */
            set(obj, MUIA_Pl_Pressed, i);
            return MUI_EventHandlerRC_Eat;
        }
    }

    return 0;
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

static ULONG pl_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct PlData *d;

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

    d->colour = GetTagData(MUIA_Pl_Colour, 0x00808080, msg->ops_AttrList);
    d->volume = 20;
    d->total = 0;
    return (ULONG)obj;
}

static ULONG pl_dispose(struct IClass *cl, Object *obj, Msg msg)
{
    struct PlData *d = INST_DATA(cl, obj);

    drop_thumb(d);
    if (d->img_ok) {
        cover_unload(&d->img);
    }
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG pl_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct PlData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;
    BOOL redraw = FALSE;
    BOOL update = FALSE;

    while ((ti = NextTagItem(&tags)) != NULL) {
        switch (ti->ti_Tag) {
        case MUIA_Pl_Title:
            set_string(d->title, sizeof(d->title), (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        case MUIA_Pl_Artist:
            set_string(d->artist, sizeof(d->artist), (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        case MUIA_Pl_Album:
            set_string(d->album, sizeof(d->album), (const char *)ti->ti_Data);
            redraw = TRUE;
            break;
        case MUIA_Pl_Cover:
            set_string(d->coverfile, sizeof(d->coverfile),
                       (const char *)ti->ti_Data);
            load_cover(d);
            redraw = TRUE;
            break;
        case MUIA_Pl_Colour:
            d->colour = ti->ti_Data;
            redraw = TRUE;
            break;
        /* Diese drei aendern NUR die Mittelgruppe. Sie merken das an und
         * bitten um eine Teilauffrischung statt um ein vollstaendiges
         * Neuzeichnen - sonst liefe im Sekundentakt die ganze Leiste
         * mit Miniatur und Textzeilen durch. */
        case MUIA_Pl_Pos:
            if (d->pos != (LONG)ti->ti_Data) {
                d->pos = (LONG)ti->ti_Data;
                d->upd_time = TRUE;
                update = TRUE;
            }
            break;
        case MUIA_Pl_Total:
            if (d->total != (LONG)ti->ti_Data) {
                d->total = (LONG)ti->ti_Data;
                d->upd_time = TRUE;
                update = TRUE;
            }
            break;
        case MUIA_Pl_Playing:
            if (d->playing != (BOOL)ti->ti_Data) {
                d->playing = (BOOL)ti->ti_Data;
                d->upd_button = TRUE;
                update = TRUE;
            }
            break;
        case MUIA_Pl_Volume:
            if (d->volume != (LONG)ti->ti_Data) {
                d->volume = (LONG)ti->ti_Data;
                redraw = TRUE;
            }
            break;
        case MUIA_Pl_Pressed:
            d->pressed = (LONG)ti->ti_Data;
            break;
        }
    }

    if (redraw) {
        /* Ein vollstaendiges Neuzeichnen erledigt beides ohnehin mit. */
        d->upd_time   = FALSE;
        d->upd_button = FALSE;
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    } else if (update) {
        MUI_Redraw(obj, MADF_DRAWUPDATE);
    }
    /* IMMER an die Oberklasse: erst dort loest MUI die angehaengten
     * Benachrichtigungen aus. */
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG pl_get(struct IClass *cl, Object *obj, struct opGet *msg)
{
    struct PlData *d = INST_DATA(cl, obj);

    switch (msg->opg_AttrID) {
    case MUIA_Pl_Pressed: *msg->opg_Storage = (ULONG)d->pressed; return TRUE;
    case MUIA_Pl_SeekTo:  *msg->opg_Storage = (ULONG)d->seekto;  return TRUE;
    case MUIA_Pl_Volume:  *msg->opg_Storage = (ULONG)d->volume;  return TRUE;
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG pl_askminmax(struct IClass *cl, Object *obj,
                          struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);

    /* Feste Hoehe: die Leiste soll beim Vergroessern des Fensters nicht
     * mitwachsen, der Platz gehoert der Liste darueber. */
    msg->MinMaxInfo->MinWidth  += 480;
    msg->MinMaxInfo->MinHeight += PL_HEIGHT;
    msg->MinMaxInfo->DefWidth  += 640;
    msg->MinMaxInfo->DefHeight += PL_HEIGHT;
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MaxHeight += PL_HEIGHT;
    return 0;
}

static ULONG pl_setup(struct IClass *cl, Object *obj, Msg msg)
{
    struct PlData *d = INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }

    /* Beide Schritte noetig: MUI_RequestIDCMP sagt dem FENSTER, welche
     * Ereignisse es anfordern soll; MUIM_Window_AddEventHandler traegt
     * dieses Objekt in die Ereigniskette ein. Ohne den zweiten kommt
     * MUIM_HandleEvent nie an, und zwar lautlos. */
    MUI_RequestIDCMP(obj, IDCMP_MOUSEBUTTONS);

    d->ehn.ehn_Object   = obj;
    d->ehn.ehn_Class    = cl;
    d->ehn.ehn_Events   = IDCMP_MOUSEBUTTONS;
    d->ehn.ehn_Flags    = 0;
    d->ehn.ehn_Priority = 0;
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, (ULONG)&d->ehn);
    return TRUE;
}

static ULONG pl_cleanupm(struct IClass *cl, Object *obj, Msg msg)
{
    struct PlData *d = INST_DATA(cl, obj);

    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
    MUI_RejectIDCMP(obj, IDCMP_MOUSEBUTTONS);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG pl_dispatch(struct IClass *cl  __asm("a0"),
                         Object       *obj __asm("a2"),
                         Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:           return pl_new(cl, obj, (struct opSet *)msg);
    case OM_DISPOSE:       return pl_dispose(cl, obj, msg);
    case OM_SET:           return pl_set(cl, obj, (struct opSet *)msg);
    case OM_GET:           return pl_get(cl, obj, (struct opGet *)msg);
    case MUIM_AskMinMax:   return pl_askminmax(cl, obj,
                                   (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:        return pl_draw(cl, obj, (struct MUIP_Draw *)msg);
    case MUIM_Setup:       return pl_setup(cl, obj, msg);
    case MUIM_Cleanup:     return pl_cleanupm(cl, obj, msg);
    case MUIM_HandleEvent: return pl_handleevent(cl, obj,
                                   (struct MUIP_HandleEvent *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

BOOL pl_init(void)
{
    g_mcc = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                  sizeof(struct PlData),
                                  (APTR)pl_dispatch);
    return g_mcc != NULL;
}

void pl_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
}
