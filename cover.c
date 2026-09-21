/* AmiSubsonic - Albumcover laden und seine Hauptfarbe bestimmen. */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/datatypes.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <datatypes/pictureclass.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cover.h"

struct Library *DataTypesBase = NULL;

/* Warum es schiefging. Alle Fehlerstellen hier melden von sich aus
 * nichts - ohne diesen Text sucht man im Dunkeln. */
static char g_why[160] = "";

const char *cover_last_error(void)
{
    return g_why[0] ? g_why : "kein Fehler";
}

/* Gehoert in der Oberflaeche panel.c. Das CLI hat kein panel.c, dort
 * greift diese schwache Definition, und read_bitmap() oeffnet die
 * Bibliothek bei Bedarf selbst. */
struct Library *CyberGfxBase __attribute__((weak)) = NULL;

/* Ausweg fuer picture.datatypes, die PDTM_READPIXELARRAY nicht koennen:
 * die fertige BitMap holen und ueber cybergraphics zuruecklesen - also
 * derselbe Weg, auf dem ein Bildbetrachter das Bild auf den Schirm
 * bringt.
 *
 * Gemessen am 19.9.2026 in Amiberry: picture.datatype 43.41 (1998)
 * erkannte das Bild (300x265, Tiefe 24), schrieb per
 * PDTM_READPIXELARRAY aber in keine einzige Zeile - weder Rueckgabewert
 * noch Pufferinhalt aenderten sich. Bildbetrachter zeigten dieselbe
 * Datei trotzdem an.
 *
 * Nur der Ausweg, nicht der Hauptweg: die BitMap ist auf den Schirm
 * umgerechnet, auf einem Bildschirm mit wenigen Farben also gerastert. */
static BOOL read_bitmap(Object *dt, UBYTE *buf, LONG w, LONG h)
{
    struct BitMap *bm = NULL;
    struct RastPort rp;
    BOOL opened = FALSE;
    BOOL ok = FALSE;

    if (!GetDTAttrs(dt, PDTA_DestBitMap, (ULONG)&bm, TAG_END) || !bm) {
        if (!GetDTAttrs(dt, PDTA_BitMap, (ULONG)&bm, TAG_END) || !bm) {
            return FALSE;
        }
    }

    if (!CyberGfxBase) {
        CyberGfxBase = OpenLibrary("cybergraphics.library", 40);
        opened = (CyberGfxBase != NULL);
    }
    if (CyberGfxBase) {
        InitRastPort(&rp);
        rp.BitMap = bm;
        ok = ReadPixelArray(buf, 0, 0, (UWORD)(w * 3), &rp, 0, 0,
                            (UWORD)w, (UWORD)h, RECTFMT_RGB) > 0;
    }
    if (opened) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Laden                                                               */
/* ------------------------------------------------------------------ */

BOOL cover_load(const char *path, struct CoverImage *img)
{
    Object *dt = NULL;
    struct Screen *scr;
    struct BitMapHeader *bmhd = NULL;
    UBYTE *buf = NULL;
    LONG w, h, y;
    LONG gotrows = 0;

    memset(img, 0, sizeof(*img));
    g_why[0] = '\0';

    if (!path || !path[0]) {
        strcpy(g_why, "kein Dateiname");
        return FALSE;
    }

    DataTypesBase = OpenLibrary("datatypes.library", 39);
    if (!DataTypesBase) {
        strcpy(g_why, "datatypes.library fehlt");
        return FALSE;
    }

    /* PDTA_Screen ist Pflicht. Ohne einen Bildschirm baut
     * picture.datatype die Bilddaten gar nicht erst auf - es liest nur
     * den Dateikopf. PDTM_READPIXELARRAY lieferte dann fuer jede Zeile 0
     * und liess den Puffer unberuehrt, PDTA_BitMap kam als NULL zurueck,
     * und zwar OHNE jede Fehlermeldung, obwohl der BitMapHeader schon
     * korrekt 300x300 bei Tiefe 24 meldete. */
    scr = LockPubScreen(NULL);
    if (!scr) {
        strcpy(g_why, "kein oeffentlicher Bildschirm");
        CloseLibrary(DataTypesBase);
        DataTypesBase = NULL;
        return FALSE;
    }

    dt = NewDTObject((APTR)path,
                     DTA_GroupID, GID_PICTURE,
                     PDTA_Screen, (ULONG)scr,
                     PDTA_Remap,  TRUE,
                     TAG_END);
    UnlockPubScreen(NULL, scr);

    if (!dt) {
        sprintf(g_why, "NewDTObject scheitert, IoErr %ld - Datentyp fuer "
                       "dieses Bild vorhanden?", (long)IoErr());
        CloseLibrary(DataTypesBase);
        DataTypesBase = NULL;
        return FALSE;
    }

    DoMethod(dt, DTM_PROCLAYOUT, NULL, 1);

    if (!GetDTAttrs(dt, PDTA_BitMapHeader, (ULONG)&bmhd, TAG_END) || !bmhd) {
        strcpy(g_why, "kein BitMapHeader vom Objekt");
        goto out;
    }

    w = bmhd->bmh_Width;
    h = bmhd->bmh_Height;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        sprintf(g_why, "unsinnige Bildgroesse %ld x %ld", (long)w, (long)h);
        goto out;
    }

    buf = malloc((size_t)w * (size_t)h * 3);
    if (!buf) {
        sprintf(g_why, "zu wenig Speicher fuer %ld x %ld", (long)w, (long)h);
        goto out;
    }

    /* Zeilenweise holen. Das ganze Bild in einem Aufruf ginge auch, aber
     * zeilenweise laesst sich sauber erkennen, ab wo nichts mehr kommt. */
    for (y = 0; y < h; y++) {
        if (DoMethod(dt, PDTM_READPIXELARRAY,
                     (ULONG)(buf + (long)y * w * 3),
                     PBPAFMT_RGB, (ULONG)(w * 3),
                     0, (ULONG)y, (ULONG)w, 1)) {
            gotrows++;
        }
    }

    if (gotrows == 0 && read_bitmap(dt, buf, w, h)) {
        gotrows = h;
        strcpy(g_why, "ueber die BitMap gelesen (PDTM_READPIXELARRAY "
                      "fehlt im picture.datatype)");
    }

    if (gotrows == 0) {
        sprintf(g_why, "PDTM_READPIXELARRAY liefert nichts, BitMap auch "
                       "nicht (%ld x %ld, Tiefe %ld)", (long)w, (long)h,
                       (long)bmhd->bmh_Depth);
        free(buf);
        buf = NULL;
        goto out;
    }

    img->rgb = buf;
    img->w = w;
    img->h = h;
    buf = NULL;

out:
    if (buf) {
        free(buf);
    }
    if (dt) {
        DisposeDTObject(dt);
    }
    CloseLibrary(DataTypesBase);
    DataTypesBase = NULL;
    return img->rgb != NULL;
}

void cover_unload(struct CoverImage *img)
{
    if (img->rgb) {
        free(img->rgb);
    }
    img->rgb = NULL;
    img->w = img->h = 0;
}

BOOL cover_scale_rgb(const struct CoverImage *src, LONG box, BOOL grow,
                     UBYTE **out, LONG *outw, LONG *outh)
{
    LONG dw, dh, x, y;
    UBYTE *dst;

    if (!src || !src->rgb || box <= 0 || !out) {
        return FALSE;
    }

    if (!grow && src->w <= box && src->h <= box) {
        dw = src->w;
        dh = src->h;
    } else if (src->w >= src->h) {
        dw = box;
        dh = src->h * box / src->w;
    } else {
        dh = box;
        dw = src->w * box / src->h;
    }
    if (dw < 1) { dw = 1; }
    if (dh < 1) { dh = 1; }

    dst = malloc((size_t)dw * (size_t)dh * 3);
    if (!dst) {
        return FALSE;
    }

    for (y = 0; y < dh; y++) {
        const UBYTE *srow = src->rgb
                          + (long)(y * src->h / dh) * src->w * 3;
        UBYTE *drow = dst + (long)y * dw * 3;

        for (x = 0; x < dw; x++) {
            const UBYTE *s = srow + (long)(x * src->w / dw) * 3;
            *drow++ = s[0];
            *drow++ = s[1];
            *drow++ = s[2];
        }
    }

    *out = dst;
    if (outw) { *outw = dw; }
    if (outh) { *outh = dh; }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Hauptfarbe                                                          */
/* ------------------------------------------------------------------ */

/* Schrittweite beim Abtasten. 8 heisst jedes achte Pixel in x und y,
 * also ein Vierundsechzigstel der Bildpunkte. Bei einem 300er Cover sind
 * das rund 1400 Proben - mehr als genug fuer 4096 Faecher. */
#define STEP 8

#define BUCKETS 4096
#define BUCKET(r, g, b) \
    (((int)((r) >> 4) << 8) | ((int)((g) >> 4) << 4) | (int)((b) >> 4))

/* Die Saettigung ist hier schlicht max-min der drei Kanaele, nicht die
 * Definition aus dem HSV-Modell - fuer "ist das bunt oder grau" reicht
 * das und kostet zwei Vergleiche statt einer Division. */
#define MIN_SAT     40
#define MIN_BRIGHT  24
#define MAX_BRIGHT  232

struct Hist {
    ULONG count[BUCKETS];
    ULONG sr[BUCKETS];
    ULONG sg[BUCKETS];
    ULONG sb[BUCKETS];
};

BOOL cover_dominant_img(const struct CoverImage *img, ULONG *rgb)
{
    struct Hist *h;
    ULONG nsamples = 0;
    LONG x, y;
    int i, best = -1, pass;
    BOOL ok = FALSE;

    if (!img->rgb || img->w <= 0 || img->h <= 0 || !rgb) {
        strcpy(g_why, "kein Bild geladen");
        return FALSE;
    }

    h = calloc(1, sizeof(struct Hist));
    if (!h) {
        strcpy(g_why, "zu wenig Speicher");
        return FALSE;
    }

    /* Zwei Durchgaenge, falls noetig.
     *
     * Der erste laesst Graues und fast Schwarzes/Weisses aussen vor -
     * sonst gewinnt bei fast jedem Cover der dunkle Hintergrund. Bei
     * einem SCHWARZWEISSFOTO bleibt dann aber gar nichts uebrig; genau
     * das passierte bei "a-ha - Hunting High and Low". Der zweite nimmt
     * alles ausser reinem Schwarz und Weiss. */
    for (pass = 0; pass < 2; pass++) {
        BOOL strict = (pass == 0);

        for (y = 0; y < img->h; y += STEP) {
            const UBYTE *row = img->rgb + (long)y * img->w * 3;

            for (x = 0; x < img->w; x += STEP) {
                const UBYTE *p = row + (long)x * 3;
                int r = p[0], g = p[1], b = p[2];
                int mx = r, mn = r, bright, idx;

                if (g > mx) { mx = g; }
                if (b > mx) { mx = b; }
                if (g < mn) { mn = g; }
                if (b < mn) { mn = b; }

                /* Helligkeit ganzzahlig: die uebliche Gewichtung
                 * 0,299/0,587/0,114 als 77/150/29 von 256. */
                bright = (r * 77 + g * 150 + b * 29) >> 8;

                if (strict) {
                    if ((mx - mn) < MIN_SAT) {
                        continue;
                    }
                    if (bright < MIN_BRIGHT || bright > MAX_BRIGHT) {
                        continue;
                    }
                } else if (bright < 8 || bright > 248) {
                    continue;
                }

                idx = BUCKET(r, g, b);
                h->count[idx]++;
                h->sr[idx] += (ULONG)r;
                h->sg[idx] += (ULONG)g;
                h->sb[idx] += (ULONG)b;
                nsamples++;
            }
        }
        if (nsamples > 0) {
            break;
        }
        memset(h, 0, sizeof(struct Hist));
    }

    if (nsamples == 0) {
        strcpy(g_why, "kein brauchbarer Bildpunkt");
        free(h);
        return FALSE;
    }

    for (i = 0; i < BUCKETS; i++) {
        if (best < 0 || h->count[i] > h->count[best]) {
            best = i;
        }
    }

    if (best >= 0 && h->count[best] > 0) {
        ULONG n = h->count[best];

        /* Mittelwert der Bildpunkte im Fach, nicht die Fachmitte: 4 Bit
         * je Kanal sind grob, und der Unterschied ist am Bildschirm zu
         * sehen. */
        *rgb = ((h->sr[best] / n) << 16)
             | ((h->sg[best] / n) <<  8)
             |  (h->sb[best] / n);
        ok = TRUE;
    }

    free(h);
    return ok;
}

BOOL cover_dominant(const char *path, ULONG *rgb)
{
    struct CoverImage img;
    BOOL ok;

    if (!cover_load(path, &img)) {
        return FALSE;
    }
    ok = cover_dominant_img(&img, rgb);
    cover_unload(&img);
    return ok;
}

/* ------------------------------------------------------------------ */

ULONG cover_scale(ULONG rgb, int percent)
{
    long r = (long)((rgb >> 16) & 0xff) * percent / 100;
    long g = (long)((rgb >>  8) & 0xff) * percent / 100;
    long b = (long)( rgb        & 0xff) * percent / 100;

    if (r > 255) { r = 255; }
    if (g > 255) { g = 255; }
    if (b > 255) { b = 255; }

    return ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
}

ULONG cover_gradient_top(ULONG rgb)
{
    long r = (rgb >> 16) & 0xff;
    long g = (rgb >>  8) & 0xff;
    long b =  rgb        & 0xff;
    long lum = (r * 77 + g * 150 + b * 29) >> 8;

    if (lum < 1) {
        lum = 1;
    }

    /* Auf eine feste Zielhelligkeit bringen, Farbton bleibt.
     *
     * Ein fester Aufhellfaktor taugt nicht: die gemessenen Hauptfarben
     * reichen von 0x361956 (sehr dunkles Violett) bis 0xe5cab4 (helles
     * Beige), und 130 Prozent machten aus dem Beige ein fast weisses
     * 0xffffea. Helle Farben muessen ABGEDUNKELT werden, damit der
     * Verlauf oben satt anfaengt. */
    return cover_scale(rgb, (int)(GRAD_TOP_LUM * 100 / lum));
}
