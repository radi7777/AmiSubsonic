/* AmiSubsonic - die ECS-Fassung: eigener Bildschirm mit 8 Farben.
 * Zur Palette siehe ecsgfx.h. */

#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <libraries/mui.h>

#include <string.h>

#include "ecsgfx.h"

static struct Screen *g_scr = NULL;
static BOOL  g_own[8];              /* exklusiv belegte Stifte */

/* Die Palette als 0x00RRGGBB, auf 12 Bit gerundet - so, wie sie auf dem
 * Schirm steht. Aus ihr wird die Nachschlagetabelle gebaut. */
static ULONG g_pal[8] = {
    0x000000, 0xffffff, 0x77aaff, 0xaaaaaa,
    0x223344, 0x112233, 0x4488cc, 0x444444
};

/* RGB mit 4 Bit je Kanal -> Stift. 4 KB, neu gebaut, wenn sich die
 * Albumfarbe aendert. Ein Nachschlagen statt acht Abstandsrechnungen je
 * Bildpunkt. */
static UBYTE g_lut[4096];

/* Dieselbe Tabelle fuer Cover, aber OHNE das Hellblau der Seitenleiste -
 * es gehoert zur Oberflaeche und streute sonst blaue Punkte in jedes
 * Bild. */
static UBYTE g_lutc[4096];

/* Geordnetes Raster (Bayer 4x4). Ohne Zwischenspeicher und ohne Zustand
 * von Bildpunkt zu Bildpunkt - Floyd-Steinberg braeuchte beides, und
 * MUI zeichnet zu beliebiger Zeit neu (kein malloc, siehe netjob.h).
 * Die Schwelle haengt an der Stelle IM BILD, nicht auf dem Schirm:
 * rollt die Bildwand, wandert das Muster mit dem Cover mit, statt
 * darueber zu flimmern. */
static const UBYTE g_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/* Eine Zeile Stiftnummern fuer WriteChunkyPixels. Fest, nicht
 * angelegt: MUI kann jederzeit neu zeichnen lassen, auch mitten in
 * einem Netzauftrag, und dann ist malloc verboten (siehe netjob.h). */
#define ROW_MAX 1024
static UBYTE g_row[ROW_MAX];

/* Graustufen: Schwarz, Dunkelgrau, Hellgrau, Weiss - die Stufen der
 * Palette, dazwischen gerastert. */
static const UBYTE g_glev[4] = { 0x00, 0x44, 0xaa, 0xff };
static const UBYTE g_gpen[4] = { EPEN_BLACK, EPEN_DGREY, EPEN_LGREY,
                                 EPEN_WHITE };

static LONG lum8(LONG r, LONG g, LONG b)
{
    return (r * 77 + g * 150 + b * 29) >> 8;
}

static UBYTE nearest(LONG r, LONG g, LONG b, BOOL noblue)
{
    ULONG best = 0xffffffffUL;
    UBYTE bp = 0;
    LONG p;

    for (p = 0; p < 8; p++) {
        LONG dr, dg, db;
        ULONG d;

        if (noblue && p == EPEN_BLUE) {
            continue;
        }
        dr = r - (LONG)((g_pal[p] >> 16) & 0xff);
        dg = g - (LONG)((g_pal[p] >> 8) & 0xff);
        db = b - (LONG)(g_pal[p] & 0xff);
        /* Gewichtet wie das Auge: Gruen zaehlt am meisten. */
        d = (ULONG)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
        if (d < best) {
            best = d;
            bp = (UBYTE)p;
        }
    }
    return bp;
}

static void build_lut(void)
{
    LONG i;

    for (i = 0; i < 4096; i++) {
        LONG r = ((i >> 8) & 15) * 17;
        LONG g = ((i >> 4) & 15) * 17;
        LONG b = (i & 15) * 17;

        g_lut[i]  = nearest(r, g, b, FALSE);
        g_lutc[i] = nearest(r, g, b, TRUE);
    }
}

ULONG ecs_mpen(const UWORD *pens, LONG i)
{
    switch (i) {
    case MPEN_SHINE:      return EPEN_WHITE;
    case MPEN_HALFSHINE:  return EPEN_LGREY;
    case MPEN_HALFSHADOW: return EPEN_DGREY;
    case MPEN_SHADOW:     return EPEN_BLACK;
    case MPEN_TEXT:       return EPEN_WHITE;
    }
    return pens[i];
}

UBYTE ecs_pen(ULONG rgb)
{
    return g_lut[((rgb >> 12) & 0xf00) | ((rgb >> 8) & 0x0f0)
                 | ((rgb >> 4) & 0x00f)];
}

/* Auf eine Zielhelligkeit bringen, Farbton bleibt. */
static ULONG to_lum(ULONG rgb, LONG target)
{
    LONG r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    LONG l = lum8(r, g, b);

    if (l < 1) {
        l = 1;
    }
    r = r * target / l;
    g = g * target / l;
    b = b * target / l;
    if (r > 255) { r = 255; }
    if (g > 255) { g = 255; }
    if (b > 255) { b = 255; }
    return ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
}

/* Auf 12 Bit runden, OHNE den Farbton zu verlieren.
 *
 * Schlichtes Runden macht aus einem dunklen Braun bei 4 Bit je Kanal
 * leicht ein Olivgruen - in der Vorschau vom 13.10.2026 zu sehen. Also
 * unter den 27 benachbarten 12-Bit-Werten den nehmen, dessen Verhaeltnis
 * der Kanaele am besten passt, mit einem kleinen Gewicht fuer die
 * Helligkeit. Ganzzahlig: die Fehler werden auf die Skala des Originals
 * gebracht, statt Brueche zu vergleichen. */
static ULONG q12_hue(ULONG rgb)
{
    LONG c[3], base[3], n[3], best[3];
    LONG mc, lc, i, d0, d1, d2;
    ULONG bs = 0xffffffffUL;

    c[0] = (rgb >> 16) & 0xff;
    c[1] = (rgb >> 8) & 0xff;
    c[2] = rgb & 0xff;
    mc = c[0];
    if (c[1] > mc) { mc = c[1]; }
    if (c[2] > mc) { mc = c[2]; }
    if (mc < 1) {
        return 0;
    }
    lc = lum8(c[0], c[1], c[2]);
    for (i = 0; i < 3; i++) {
        base[i] = (c[i] + 8) / 17;
        best[i] = base[i];
    }

    for (d0 = -1; d0 <= 1; d0++)
    for (d1 = -1; d1 <= 1; d1++)
    for (d2 = -1; d2 <= 1; d2++) {
        LONG mn, h = 0, dl;
        ULONG s;

        n[0] = base[0] + d0;
        n[1] = base[1] + d1;
        n[2] = base[2] + d2;
        for (i = 0; i < 3; i++) {
            if (n[i] < 0)  { n[i] = 0; }
            if (n[i] > 15) { n[i] = 15; }
        }
        mn = n[0];
        if (n[1] > mn) { mn = n[1]; }
        if (n[2] > mn) { mn = n[2]; }
        if (mn == 0) {
            continue;
        }
        mn *= 17;
        for (i = 0; i < 3; i++) {
            LONG e = (n[i] * 17 * mc - c[i] * mn) / mn;

            h += e * e;
        }
        dl = lum8(n[0] * 17, n[1] * 17, n[2] * 17) - lc;
        if (dl < 0) { dl = -dl; }
        dl *= mc;
        s = (ULONG)h * 16 + ((ULONG)dl * (ULONG)dl) / 100;
        if (s < bs) {
            bs = s;
            best[0] = n[0];
            best[1] = n[1];
            best[2] = n[2];
        }
    }
    return ((ULONG)(best[0] * 17) << 16) | ((ULONG)(best[1] * 17) << 8)
         | (ULONG)(best[2] * 17);
}

static void set_pen(LONG n, ULONG rgb)
{
    g_pal[n] = rgb;
    if (g_scr) {
        SetRGB32(&g_scr->ViewPort, (ULONG)n,
                 ((rgb >> 16) & 0xff) * 0x01010101UL,
                 ((rgb >> 8) & 0xff) * 0x01010101UL,
                 (rgb & 0xff) * 0x01010101UL);
    }
}

void ecs_album_colours(ULONG rgb, ULONG out[3])
{
    /* Helligkeiten wie in der abgenommenen Vorschau: die Flaeche so
     * dunkel, dass weisser Text gut steht, die Auswahl deutlich heller. */
    out[0] = q12_hue(to_lum(rgb, 60));
    out[1] = q12_hue(to_lum(rgb, 28));
    out[2] = q12_hue(to_lum(rgb, 125));
}

void ecs_set_album(ULONG rgb)
{
    ULONG c[3];

    ecs_album_colours(rgb, c);
    set_pen(EPEN_ALB,   c[0]);
    set_pen(EPEN_ALBDK, c[1]);
    set_pen(EPEN_ALBLT, c[2]);
    build_lut();
}

struct Screen *ecs_screen_open(ULONG displayid, const char *pubname)
{
    /* SA_Pens: Hintergrund schwarz, Text weiss - wie die Oberflaeche.
     * Reihenfolge nach intuition/screens.h: DETAIL, BLOCK, TEXT, SHINE,
     * SHADOW, FILL, FILLTEXT, BACKGROUND, HIGHLIGHTTEXT, BARDETAIL,
     * BARBLOCK, BARTRIM. */
    static UWORD pens[] = { 0, 1, 1, 1, 0, 2, 0, 0, 1, 1, 7, 0, (UWORD)~0 };
    ULONG cols[2 + 8 * 3];
    LONG i;

    if (displayid == 0 || displayid == (ULONG)INVALID_ID) {
        displayid = HIRESLACE_KEY;
    }

    cols[0] = (8UL << 16) | 0;
    for (i = 0; i < 8; i++) {
        cols[1 + i * 3]     = ((g_pal[i] >> 16) & 0xff) * 0x01010101UL;
        cols[1 + i * 3 + 1] = ((g_pal[i] >> 8) & 0xff) * 0x01010101UL;
        cols[1 + i * 3 + 2] = (g_pal[i] & 0xff) * 0x01010101UL;
    }
    cols[1 + 8 * 3] = 0;

    /* STDSCREENHEIGHT: 512 auf PAL, 400 auf NTSC - der Anwender hat
     * beides. SA_SysFont 1 ist die Bildschirmschrift der Workbench
     * (Wunsch des Anwenders). SA_SharePens: nur die Stifte aus SA_Pens
     * sind vergeben, der Rest bleibt frei - sonst koennte MUI 4 bis 6
     * nicht mehr ausgeschlossen werden. */
    g_scr = OpenScreenTags(NULL,
        SA_DisplayID,   displayid,
        SA_Width,       640,
        SA_Height,      STDSCREENHEIGHT,
        SA_Depth,       3,
        SA_Colors32,    (ULONG)cols,
        SA_Pens,        (ULONG)pens,
        SA_SysFont,     1,
        SA_SharePens,   TRUE,
        SA_Interleaved, TRUE,
        SA_ShowTitle,   FALSE,
        SA_Title,       (ULONG)"AmiSubsonic",
        SA_Type,        PUBLICSCREEN,
        SA_PubName,     (ULONG)pubname,
        TAG_DONE);
    if (!g_scr) {
        return NULL;
    }

    /* ALLE Stifte selbst belegen, bevor MUI das Fenster oeffnet. Ein
     * freier Stift ist fuer ObtainBestPen Freiwild: MUI nahm sich Stift 3
     * und faerbte das Hellgrau in ein #333 um (Bildschirmfoto der TF536,
     * 13.10.2026). Belegt kann MUI die Stifte nur noch MITbenutzen.
     * 4 bis 6 exklusiv - die werden beim Albumwechsel umgefaerbt. */
    for (i = 1; i < 8; i++) {
        BOOL ex = (i >= EPEN_ALB && i <= EPEN_ALBLT);

        g_own[i] = (ObtainPen(g_scr->ViewPort.ColorMap, (ULONG)i,
                              ((g_pal[i] >> 16) & 0xff) * 0x01010101UL,
                              ((g_pal[i] >> 8) & 0xff) * 0x01010101UL,
                              (g_pal[i] & 0xff) * 0x01010101UL,
                              ex ? PEN_EXCLUSIVE : 0) == (ULONG)i);
    }
    build_lut();
    PubScreenStatus(g_scr, 0);
    return g_scr;
}

void ecs_screen_close(void)
{
    LONG i;

    if (!g_scr) {
        return;
    }
    for (i = 0; i < 8; i++) {
        if (g_own[i]) {
            ReleasePen(g_scr->ViewPort.ColorMap, (ULONG)i);
            g_own[i] = FALSE;
        }
    }
    CloseScreen(g_scr);
    g_scr = NULL;
}

void ecs_fill_pen(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h,
                  UBYTE pen)
{
    ULONG old;

    if (w <= 0 || h <= 0) {
        return;
    }
    /* Den Stift zurueckstellen: Aufrufer setzen ihn oft vor dem Text
     * und fuellen danach noch etwas. */
    old = GetAPen(rp);
    SetAPen(rp, pen);
    RectFill(rp, x, y, x + w - 1, y + h - 1);
    SetAPen(rp, old);
}

void ecs_fill(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h, ULONG rgb)
{
    ecs_fill_pen(rp, x, y, w, h, ecs_pen(rgb));
}


static UBYTE gray_pen(LONG l, LONG th)
{
    LONG i = (l >= 0xaa) ? 2 : (l >= 0x44) ? 1 : 0;
    LONG frac = (l - g_glev[i]) * 16 / (g_glev[i + 1] - g_glev[i]);

    return (frac > th) ? g_gpen[i + 1] : g_gpen[i];
}

/* Rastern beim Umwandeln (ecs_cover_convert). Beide Funktionen haben
 * KEINEN gemeinsamen Zustand - sie laufen im Netzprozess, waehrend die
 * Oberflaeche mit g_lut zeichnet. */

void ecs_dither_levels(const UBYTE *rgb, LONG w, LONG h, UBYTE *lev)
{
    LONG x, y;

    for (y = 0; y < h; y++) {
        const UBYTE *bay = g_bayer[y & 3];

        for (x = 0; x < w; x++, rgb += 3) {
            LONG l = lum8(rgb[0], rgb[1], rgb[2]);
            LONG i = (l >= 0xaa) ? 2 : (l >= 0x44) ? 1 : 0;
            LONG frac = (l - g_glev[i]) * 16 / (g_glev[i + 1] - g_glev[i]);

            *lev++ = (UBYTE)((frac > bay[x & 3]) ? i + 1 : i);
        }
    }
}

UBYTE ecs_level_pen(UBYTE lev)
{
    return g_gpen[lev & 3];
}

void ecs_dither_colour(const UBYTE *rgb, LONG w, LONG h, ULONG dominant,
                       UBYTE *pens)
{
    ULONG pal[8];
    ULONG c[3];
    LONG x, y, p;

    /* Die Palette, die beim Zeigen DIESES Albums gilt - ohne das
     * Hellblau der Oberflaeche. */
    ecs_album_colours(dominant, c);
    for (p = 0; p < 8; p++) {
        pal[p] = g_pal[p];
    }
    pal[EPEN_ALB] = c[0];
    pal[EPEN_ALBDK] = c[1];
    pal[EPEN_ALBLT] = c[2];

    for (y = 0; y < h; y++) {
        const UBYTE *bay = g_bayer[y & 3];

        for (x = 0; x < w; x++, rgb += 3) {
            LONG o = (bay[x & 3] * 2 - 15) * 3;
            LONG r = rgb[0] + o, g = rgb[1] + o, b = rgb[2] + o;
            ULONG best = 0xffffffffUL;
            UBYTE bp = 0;

            for (p = 0; p < 8; p++) {
                LONG dr, dg, db;
                ULONG d;

                if (p == EPEN_BLUE) {
                    continue;
                }
                dr = r - (LONG)((pal[p] >> 16) & 0xff);
                dg = g - (LONG)((pal[p] >> 8) & 0xff);
                db = b - (LONG)(pal[p] & 0xff);
                d = (ULONG)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
                if (d < best) {
                    best = d;
                    bp = (UBYTE)p;
                }
            }
            *pens++ = bp;
        }
    }
}

ULONG ecs_write_mode(APTR src, LONG sx, LONG sy, LONG mod,
                     struct RastPort *rp, LONG x, LONG y, LONG w, LONG h,
                     LONG fmt, LONG mode)
{
    LONG bpp = (fmt == RECTFMT_RGB) ? 3 : 4;
    LONG off = (fmt == RECTFMT_RGB) ? 0 : 1;
    LONG yy, xx;

    if (mode == ECS_W_NEAR) {
        return ecs_write(src, sx, sy, mod, rp, x, y, w, h, fmt);
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (w > ROW_MAX) {
        w = ROW_MAX;
    }
    if (mode == ECS_W_PENS) {
        /* Schon fertige Stifte (aus der .ecs-Ablage): nur kopieren. */
        for (yy = 0; yy < h; yy++) {
            WriteChunkyPixels(rp, x, y + yy, x + w - 1, y + yy,
                              (UBYTE *)src + (sy + yy) * mod + sx, mod);
        }
        return (ULONG)(w * h);
    }
    for (yy = 0; yy < h; yy++) {
        const UBYTE *s = (const UBYTE *)src + (sy + yy) * mod
                       + sx * bpp + off;
        const UBYTE *bay = g_bayer[(sy + yy) & 3];

        for (xx = 0; xx < w; xx++, s += bpp) {
            LONG th = bay[(sx + xx) & 3];

            if (mode == ECS_W_GRAY) {
                g_row[xx] = gray_pen(lum8(s[0], s[1], s[2]), th);
            } else {
                /* Farbe: die Schwelle als Versatz auf alle drei Kanaele,
                 * etwa eine halbe Stufe der Palette in jede Richtung. */
                LONG o = (th * 2 - 15) * 3;
                LONG r = s[0] + o, g = s[1] + o, b = s[2] + o;

                if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
                if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
                if (b < 0) { b = 0; } else if (b > 255) { b = 255; }
                g_row[xx] = g_lutc[((r & 0xf0) << 4) | (g & 0xf0) | (b >> 4)];
            }
        }
        WriteChunkyPixels(rp, x, y + yy, x + w - 1, y + yy, g_row, ROW_MAX);
    }
    return (ULONG)(w * h);
}

ULONG ecs_write(APTR src, LONG sx, LONG sy, LONG mod, struct RastPort *rp,
                LONG x, LONG y, LONG w, LONG h, LONG fmt)
{
    LONG bpp = (fmt == RECTFMT_RGB) ? 3 : 4;
    LONG off = (fmt == RECTFMT_RGB) ? 0 : 1;   /* ARGB: A ueberspringen */
    LONG yy, xx;

    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (w > ROW_MAX) {
        w = ROW_MAX;
    }
    for (yy = 0; yy < h; yy++) {
        const UBYTE *s = (const UBYTE *)src + (sy + yy) * mod
                       + sx * bpp + off;

        for (xx = 0; xx < w; xx++, s += bpp) {
            g_row[xx] = g_lut[((ULONG)(s[0] & 0xf0) << 4)
                              | (s[1] & 0xf0) | (s[2] >> 4)];
        }
        WriteChunkyPixels(rp, x, y + yy, x + w - 1, y + yy, g_row, ROW_MAX);
    }
    return (ULONG)(w * h);
}
