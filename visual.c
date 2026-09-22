/* AmiSubsonic - der Visualizer, Schritt 1: messen.
 *
 * Woher die Daten kommen: audio_vis_window() schneidet aus dem Puffer,
 * den AHI GERADE spielt, das Stueck an der hoerbaren Stelle aus (siehe
 * audio.c). Gerechnet wird hier in der Oberflaeche, nicht im
 * Audioprozess - der soll nur spielen, und so kostet der Visualizer
 * nichts, wenn man ihn nicht ansieht.
 *
 * Die FFT rechnet ganzzahlig und NUR mit 16x16-Bit-Multiplikationen
 * (muls.w). Das ist Absicht: der 68060 kann das 32x32->64 aus muls.l
 * nicht in Hardware, es wird dort in Software nachgebildet. Die
 * Genauigkeit reicht fuer eine Anzeige locker.
 *
 * Gezeichnet wird wie ueberall hier: alles in einen eigenen Puffer,
 * dann EIN WritePixelArray. So flackert nichts. */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <libraries/mui.h>
#include <clib/alib_protos.h>

#include <string.h>

#include "visual.h"
#include "audio.h"

extern struct Library *CyberGfxBase;     /* gehoert panel.c */
extern struct Device  *TimerBase;        /* gehoert audio.c */

static struct MUI_CustomClass *g_mcc = NULL;

#define VIS_N     1024          /* Punkte der FFT */
#define VIS_LOG2N 10
#define VIS_BINS  (VIS_N / 2)

/* Pegel in log2 * 256, und zwar der LEISTUNG (Betrag im Quadrat): eine
 * Stufe von 256 sind 3 dB. Der erste Anlauf rechnete mit 6 dB je Stufe
 * und zeigte nur das linke Stueck (22.9.2026).
 *
 * Wie der Anwender sagt: es geht um die Optik, nicht um Messtechnik.
 * Ermittelt am Mac mit einem Nachbau (numpy, gerendert als Bild) an einem
 * Mitschnitt von Absolut HOT, bis es aussah wie Feishin:
 *
 *   - Die Hoehen bekommen +3 dB je Oktave dazu. Ohne das ist Musik links
 *     immer am hoechsten, und Gesang geht in den Baessen unter.
 *   - Die Untergrenze laeuft mit: der Mittelwert des Spektrums (plus
 *     VIS_OFF) liegt auf der Grundlinie. So liegt das Grundrauschen flach, und
 *     nur was herausragt, steht als Spitze da - egal wie laut der Titel
 *     ist. Mit fester Untergrenze lag die Linie als Decke ueber allem.
 *   - Darueber 21 dB Anzeigebereich. */
/* +6 dB ueber dem Mittelwert. Am Mac mit der logarithmischen Achse
 * verglichen (3 bis 9 dB, zusammen mit der Neigung): bei 6 stehen die
 * Bass-Huegel hoch, ohne oben anzuschlagen, und rechts fallen die
 * Spitzen bis auf die Grundlinie - wie bei Feishin. */
#define VIS_OFF   512L
#define VIS_RANGE 1786L                 /* 21 dB: 21 / 3,01 * 256 */
#define VIS_MEAN0 2                     /* Mittelwert ueber diese Bins ... */
#define VIS_MEAN1 370                   /* ... bis 16 kHz, darueber ist MP3 leer */
/* Takt, einstellbar in Settings (MUIA_Vis_Fps): 15, 20, 30 oder 60.
 *
 * Eigener Zeitgeber (timer.device), nicht MUIs Timer-Handler. Der zaehlt
 * im Raster der Bildwiederholung (20 ms) und erst ab dem ENDE der
 * Arbeit: "30" ergab 35 Bilder/s, 60 ging gar nicht (Protokoll
 * 22.9.2026). Hier geht die naechste Anforderung zu Beginn eines Bildes
 * hinaus, der Takt haengt also nicht an der Rechenzeit. */
static ULONG fps_micros(LONG fps)
{
    return (fps > 0) ? 1000000UL / (ULONG)fps : 0;
}

/* Frequenzachse LOGARITHMISCH, von Bin 1 (43 Hz) bis 511 (22 kHz) - so
 * macht es Feishin: die wenigen Bass-Bins breit gezogen (runde Huegel
 * links), die vielen hohen eng zusammen (dichte Spitzen rechts). Der
 * Vergleich zweier Fotos desselben Titels (22.9.2026) zeigte es: mit
 * fast linearer Achse und Baendern hing bei uns alles in der linken
 * Haelfte, bei Feishin reichte es bis fast an den Rand. */
#define VIS_KMIN  1
#define VIS_KMAX  511

/* Feine Baesse. 1024 Punkte bei 44,1 kHz heissen 43 Hz je Bin, und auf
 * der logarithmischen Achse liegt das linke Drittel (43 bis 344 Hz) auf
 * nur 8 Bins - eine lange glatte Kurve, waehrend rechts alles voller
 * Spitzen war (der Anwender, 22.9.2026). Deshalb eine zweite FFT, ueber
 * 4096 Abtastwerte, vorher je 4 gemittelt: wieder 1024 Punkte, aber
 * 11 Hz je Bin. Bis VIS_FINE (fein gezaehlt, also 64 = 16 grobe Bins =
 * 689 Hz) kommt die Anzeige von dort, darueber wie bisher. Kostet eine
 * FFT mehr, rund 1,5 ms je Bild. */
#define VIS_DEC   4                     /* Abtastwerte je gemitteltem */
#define VIS_HIST  (VIS_N * VIS_DEC)     /* so viel Musik braucht die feine */
#define VIS_FINE  64                    /* feine Bins, die angezeigt werden */

/* 4096 / VIS_RANGE in 1/256. Mit festem Faktor und Schieben statt
 * "* 4096 / VIS_RANGE": gcc macht aus der Division durch eine Konstante
 * sonst ein mulu.l mit 64-Bit-Ergebnis - genau der Befehl, den der 68060
 * in Software nachbildet (gemessen im Assembler, 22.9.2026). */
#define VIS_SCALE (4096L * 256L / VIS_RANGE)

/* Tabellen in den Datenbereich. Ohne das legt dieser gcc auch Felder
 * OHNE const in den Codebereich (nachgesehen im Assembler), und dort
 * haelt "make check-fpu" ihre Bytes fuer Befehle. */
#define VIS_DATA __attribute__((section(".data")))

/* MESSUNG: Zeiten je Bild ins Protokoll. Aus - der Code bleibt stehen,
 * denn er hat jede Entscheidung an diesem Visualizer entschieden (auf 1
 * setzen und neu uebersetzen).
 *
 * Das Protokoll geht neben das Programm und NICHT nach T:. Nach einem
 * Freeze mit Reset ist der Arbeitsspeicher leer, und genau die letzten
 * Zeilen davor sind die interessanten (22.9.2026). Die Datei wird nach
 * jeder Zeile geschlossen, es geht also nichts verloren. */
#define VIS_MEASURE 0
#define VIS_LOG "PROGDIR:vis.log"

/* Was je Spalte feststeht, einmal beim Layout gerechnet (render_bg) statt
 * in jedem Bild: woher der Pegel kommt, mit welchen Gewichten gemischt
 * wird, wie weit am linken Rand eingeblendet. Vorher stand hier je Spalte
 * und Bild u.a. eine Division (22.9.2026). */
#define VC_FINE   0     /* feine Bins, k und k+1 gemischt */
#define VC_MAX    1     /* grobe Bins k bis ke, der lauteste */
#define VC_MIX    2     /* grobe Bins, k und k+1 gemischt */

struct VisCol {
    UBYTE mode;
    UBYTE pad;
    WORD  k, ke;        /* Bins */
    WORD  mw;           /* Mischgewicht fuer k+1, 0..256 (smoothstep) */
    WORD  tap;          /* Einblendung am linken Rand, 0..256 */
};

struct VisData {
    ULONG  colour;

    ULONG *buf;                 /* ARGB, AllocVec */
    LONG   buf_w, buf_h;
    ULONG  buf_col;             /* Farbton, fuer den der Hintergrund steht */
    BOOL   buf_ok;              /* Hintergrund steht im Puffer */

    /* Im selben Block wie buf: je Zeile die Hintergrundfarbe, je Spalte
     * die Spanne der Linie aus dem letzten Bild - die wird beim naechsten
     * mit der Zeilenfarbe uebermalt, statt die ganze Flaeche neu zu
     * fuellen. */
    ULONG *rowcol;
    WORD  *old0, *old1;

    /* Je Spalte zusaetzlich der VOLL gedeckte Innenteil der Spanne aus
     * dem letzten Bild. Dort steht schon die Linienfarbe - wenn er beim
     * naechsten Bild wieder innen liegt, muss niemand hinschreiben. */
    WORD  *in0, *in1;

    /* Je Zeile: Farbe der Linie (nach Hoehe), Farbe der Spiegelung bei
     * voller Deckung, und deren Deckkraft (0..6 von 16). Haengen an
     * Groesse UND Farbton. */
    ULONG *linecol;
    ULONG *refcol;
    UBYTE *rfade;

    /* Geaenderte Zeilen je senkrechtem Streifen - nur die gehen zur
     * Grafikkarte, siehe vis_draw(). */
    LONG   slo[8], shi[8];
    LONG  *colbin;              /* je Spalte (und eine dahinter): Bin in 1/256 */
    struct VisCol *cols;        /* je Spalte, siehe struct VisCol */
    LONG   base, amp;           /* Grundlinie, volle Hoehe */

    struct MUI_InputHandlerNode ihn;
    BOOL   ticking;

    /* Eigener Zeitgeber, siehe fps_micros(). Port und Anforderung
     * gehoeren dem Prozess der Oberflaeche - angelegt in OM_NEW, das in
     * ihm laeuft. */
    struct MsgPort     *tport;
    struct timerequest *treq;
    BOOL   tpending;
    BOOL   shown;               /* zwischen MUIM_Show und MUIM_Hide */
    LONG   fps;                 /* 0 = aus */

    WORD   level[VIS_BINS];     /* angezeigte Hoehe je Bin, 0..4096 */
    WORD   flevel[VIS_FINE];    /* dasselbe fuer die feinen Bass-Bins */
    ULONG  fplin[VIS_FINE];
    ULONG  plin[VIS_BINS];      /* geglaettete Leistung je Bin */
    LONG   floor;               /* mitlaufende Untergrenze, log2 * 256 */
    BOOL   floor_ok;
    BOOL   moving;              /* ist noch etwas zu sehen? */

#if VIS_MEASURE
    ULONG  m_frames, m_fft, m_render, m_wpa, m_start;
    ULONG  m_efreq;
#endif
};

struct MUI_CustomClass *vis_class(void)
{
    return g_mcc;
}

/* ------------------------------------------------------------------ */
/* Tabellen                                                            */
/* ------------------------------------------------------------------ */

/* sin(2*pi*i/1024) fuer i = 0..256, in Q15 - ein Viertel der Welle, der
 * Rest ergibt sich aus der Symmetrie. Erzeugt mit Python, nicht getippt. */
static WORD g_sin[257] VIS_DATA = {
    0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809,
    2009, 2210, 2410, 2611, 2811, 3012, 3212, 3412, 3612, 3811,
    4011, 4210, 4410, 4609, 4808, 5007, 5205, 5404, 5602, 5800,
    5998, 6195, 6393, 6590, 6786, 6983, 7179, 7375, 7571, 7767,
    7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319, 9512, 9704,
    9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12353, 12539, 12725, 12910, 13094, 13279, 13462,
    13645, 13828, 14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269,
    15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673, 16846, 17018,
    17189, 17360, 17530, 17700, 17869, 18037, 18204, 18371, 18537, 18703,
    18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000, 20159, 20317,
    20475, 20631, 20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311,
    23452, 23592, 23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680,
    24811, 24942, 25072, 25201, 25329, 25456, 25582, 25708, 25832, 25955,
    26077, 26198, 26319, 26438, 26556, 26674, 26790, 26905, 27019, 27133,
    27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001, 28105, 28208,
    28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177,
    29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037,
    30117, 30195, 30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783,
    30852, 30919, 30985, 31050, 31113, 31176, 31237, 31297, 31356, 31414,
    31470, 31526, 31580, 31633, 31685, 31736, 31785, 31833, 31880, 31926,
    31971, 32014, 32057, 32098, 32137, 32176, 32213, 32250, 32285, 32318,
    32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737,
    32745, 32752, 32757, 32761, 32765, 32766, 32767,
};

/* 256 * log2(1 + i/64) fuer i = 0..63 - der Nachkommateil des
 * Logarithmus, nachgeschlagen statt gerechnet. */
static WORD g_logf[64] VIS_DATA = {
    0, 6, 11, 17, 22, 28, 33, 38, 44, 49, 54, 59,
    63, 68, 73, 78, 82, 87, 92, 96, 100, 105, 109, 113,
    118, 122, 126, 130, 134, 138, 142, 146, 150, 154, 157, 161,
    165, 169, 172, 176, 179, 183, 186, 190, 193, 197, 200, 203,
    207, 210, 213, 216, 220, 223, 226, 229, 232, 235, 238, 241,
    244, 247, 250, 253,
};

static WORD  g_wr[VIS_N / 2], g_wi[VIS_N / 2];  /* Drehfaktoren */
static WORD  g_tilt[VIS_BINS];                  /* +3 dB je Oktave */

/* 256 * 2^(i/256) fuer i = 0..255 - fuer die logarithmische Achse, die
 * Umkehrung von g_logf. Erzeugt mit Python. */
static WORD  g_exp2[256] VIS_DATA = {
    256, 257, 257, 258, 259, 259, 260, 261, 262, 262, 263, 264,
    264, 265, 266, 267, 267, 268, 269, 270, 270, 271, 272, 272,
    273, 274, 275, 275, 276, 277, 278, 278, 279, 280, 281, 281,
    282, 283, 284, 285, 285, 286, 287, 288, 288, 289, 290, 291,
    292, 292, 293, 294, 295, 296, 296, 297, 298, 299, 300, 300,
    301, 302, 303, 304, 304, 305, 306, 307, 308, 309, 309, 310,
    311, 312, 313, 314, 314, 315, 316, 317, 318, 319, 320, 321,
    321, 322, 323, 324, 325, 326, 327, 328, 328, 329, 330, 331,
    332, 333, 334, 335, 336, 337, 337, 338, 339, 340, 341, 342,
    343, 344, 345, 346, 347, 348, 349, 350, 350, 351, 352, 353,
    354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365,
    366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377,
    378, 379, 380, 381, 382, 383, 384, 385, 386, 387, 388, 389,
    391, 392, 393, 394, 395, 396, 397, 398, 399, 400, 401, 402,
    403, 405, 406, 407, 408, 409, 410, 411, 412, 413, 415, 416,
    417, 418, 419, 420, 421, 422, 424, 425, 426, 427, 428, 429,
    431, 432, 433, 434, 435, 436, 438, 439, 440, 441, 442, 444,
    445, 446, 447, 448, 450, 451, 452, 453, 454, 456, 457, 458,
    459, 461, 462, 463, 464, 466, 467, 468, 470, 471, 472, 473,
    475, 476, 477, 478, 480, 481, 482, 484, 485, 486, 488, 489,
    490, 492, 493, 494, 496, 497, 498, 500, 501, 502, 504, 505,
    506, 508, 509, 511,
};
static WORD  g_hann[VIS_N];
static UWORD g_rev[VIS_N];                      /* Bitumkehr */
static WORD  g_mono[VIS_HIST];                  /* aelteste zuerst */
static WORD  g_dec[VIS_N];                      /* je 4 gemittelt */
static ULONG g_pw[VIS_BINS];                    /* Leistung je Bin */
static ULONG g_pwf[VIS_FINE];                   /* dasselbe, feine Bins */
static LONG  g_re[VIS_N], g_im[VIS_N];

static LONG qsin(LONG k)
{
    k &= VIS_N - 1;
    if (k <= 256) {
        return g_sin[k];
    }
    if (k <= 512) {
        return g_sin[512 - k];
    }
    if (k <= 768) {
        return -g_sin[k - 512];
    }
    return -g_sin[1024 - k];
}

static void tables_init(void)
{
    LONG i, j, b;

    for (i = 0; i < VIS_N / 2; i++) {
        g_wr[i] = (WORD)qsin(i + 256);          /* cos */
        g_wi[i] = (WORD)-qsin(i);               /* -sin: Hinrichtung */
    }
    /* Hann: (1 - cos) / 2 */
    for (i = 0; i < VIS_N; i++) {
        g_hann[i] = (WORD)((32767L - qsin(i + 256)) >> 1);
    }
    for (i = 0; i < VIS_N; i++) {
        j = 0;
        for (b = 0; b < VIS_LOG2N; b++) {
            if (i & (1L << b)) {
                j |= 1L << (VIS_LOG2N - 1 - b);
            }
        }
        g_rev[i] = (UWORD)j;
    }
}

static LONG log2_256(ULONG p);

/* Die Neigung: log2(k) * 256 ist genau eine Stufe (3 dB) je Oktave, mal
 * 3/4 also 2,25 dB. Mit linearer Achse brauchte es 3,75 dB, damit die
 * Hoehen ueberhaupt auftauchten; mit der logarithmischen druecken so
 * viele die Baesse zu flach. Am Mac verglichen, 22.9.2026.
 * Erst nach tables_init, log2_256 braucht g_logf nicht - aber es steht
 * weiter unten. */
static void tilt_init(void)
{
    LONG k;

    g_tilt[0] = 0;
    for (k = 1; k < VIS_BINS; k++) {
        g_tilt[k] = (WORD)(log2_256((ULONG)k) * 3 / 4);
    }

}

/* ------------------------------------------------------------------ */
/* Rechnen                                                             */
/* ------------------------------------------------------------------ */

/* 16 x 16 -> 32 Bit, Ergebnis wieder Q15. Die Umwandlung nach WORD
 * sorgt dafuer, dass gcc muls.w nimmt - nachgesehen im Assembler. */
#define MULQ15(a, b)  (((LONG)(WORD)(a) * (LONG)(WORD)(b)) >> 15)

/* Radix-2, Zerlegung im Zeitbereich, an Ort und Stelle. Jede Stufe
 * halbiert: so bleibt jeder Wert im 16-Bit-Bereich, was die muls.w
 * voraussetzen - ein Betrag waechst je Stufe hoechstens auf das
 * Doppelte, das Halbieren faengt es ab. Am Ende ist alles durch N
 * geteilt; fuer eine Anzeige ist das egal. */
static void fft(void)
{
    LONG len, half, step, i, j, a, b, tr, ti, wr, wi;

    for (len = 2; len <= VIS_N; len <<= 1) {
        half = len >> 1;
        step = VIS_N / len;
        for (j = 0; j < half; j++) {
            wr = g_wr[j * step];
            wi = g_wi[j * step];
            for (i = j; i < VIS_N; i += len) {
                a = i;
                b = i + half;
                tr = MULQ15(wr, g_re[b]) - MULQ15(wi, g_im[b]);
                ti = MULQ15(wr, g_im[b]) + MULQ15(wi, g_re[b]);
                g_re[b] = (g_re[a] - tr) >> 1;
                g_im[b] = (g_im[a] - ti) >> 1;
                g_re[a] = (g_re[a] + tr) >> 1;
                g_im[a] = (g_im[a] + ti) >> 1;
            }
        }
    }
}

/* log2(p) * 256, fuer p > 0. */
static LONG log2_256(ULONG p)
{
    LONG n = 31 - __builtin_clz(p);
    ULONG idx;

    if (n >= 6) {
        idx = (p >> (n - 6)) & 63;
    } else {
        idx = (p << (6 - n)) & 63;
    }
    return n * 256 + g_logf[idx];
}

/* Beide FFTs in EINEM Durchgang: fine (die gemittelten Bass-Werte) in
 * den Realteil, coarse (die juengsten 1024) in den Imaginaerteil. Weil
 * beide Eingaben reell sind, lassen sich die Spektren danach trennen:
 *
 *   A[k] = (Z[k] + konj(Z[N-k])) / 2        (fine)
 *   B[k] = (Z[k] - konj(Z[N-k])) / (2i)     (coarse)
 *
 * Halbe Rechenzeit fuer dasselbe (22.9.2026: zwei FFTs 3,3 ms je Bild).
 *
 * Beide Eingaben werden vorher halbiert: der Betrag eines komplexen
 * Werts darf sonst bis 32767 * Wurzel 2 wachsen, und die Anteile passen
 * nicht mehr in die 16 Bit der muls.w. Das kostet ein Bit - die
 * mitlaufende Untergrenze gleicht den Pegel aus. */
static void fft_two(const WORD *fine, const WORD *coarse)
{
    LONG i, k;

    for (i = 0; i < VIS_N; i++) {
        LONG r = g_rev[i];

        g_re[r] = MULQ15(fine[i], g_hann[i]) >> 1;
        g_im[r] = MULQ15(coarse[i], g_hann[i]) >> 1;
    }
    fft();

    for (k = 0; k < VIS_BINS; k++) {
        LONG j = (VIS_N - k) & (VIS_N - 1);
        LONG br = (g_im[k] + g_im[j]) >> 1;
        LONG bi = (g_re[j] - g_re[k]) >> 1;

        g_pw[k] = (ULONG)((LONG)(WORD)br * (WORD)br)
                + (ULONG)((LONG)(WORD)bi * (WORD)bi);
        if (k < VIS_FINE) {
            LONG ar = (g_re[k] + g_re[j]) >> 1;
            LONG ai = (g_im[k] - g_im[j]) >> 1;

            g_pwf[k] = (ULONG)((LONG)(WORD)ar * (WORD)ar)
                     + (ULONG)((LONG)(WORD)ai * (WORD)ai);
        }
    }
}

/* Aus dem Pegel (log2 * 256, Neigung schon drin) die angezeigte Hoehe,
 * geglaettet. Fuer grobe und feine Bins gleich. */
static WORD shape(LONG lgv, LONG lo, LONG old)
{
    LONG h = ((lgv - lo) * VIS_SCALE) >> 8;

    if (h < 0) {
        h = 0;
    }
    /* Weich begrenzen statt hart abschneiden: ueber drei Vierteln zaehlt
     * nur noch ein Viertel. Je nach Titel schlug die Linie sonst oben
     * an (der Anwender). */
    if (h > 3072) {
        h = 3072 + ((h - 3072) >> 2);
    }
    if (h > 4096) {
        h = 4096;
    }
    /* Glaetten, aber ohne Verzoegerung nach oben: steigt ein Bin, folgt
     * die Anzeige fast sofort (drei Viertel des Wegs), faellt er, sinkt
     * sie langsam (ein Achtel je Bild). So wirkt es ruhig ("zu
     * hektisch", der Anwender) und laeuft trotzdem nicht hinter dem
     * Schlag her. */
    if (h > old) {
        old += ((h - old) * 3) >> 2;
    } else {
        old -= (old - h) >> 3;
    }
    return (WORD)old;
}

/* Neues Spektrum aus g_mono (VIS_HIST Werte, der juengste zuletzt) in
 * d->level und d->flevel. */
static void spectrum(struct VisData *d)
{
    static LONG lg[VIS_BINS], flg[VIS_FINE];
    LONG i, k, sum = 0, lo;

    /* Fein: je 4 mitteln (ein einfacher Tiefpass - was dabei aus hohen
     * Frequenzen herunterklappt, stoert die Optik nicht). Dann beide
     * FFTs auf einmal. */
    for (i = 0; i < VIS_N; i++) {
        const WORD *m = g_mono + i * VIS_DEC;

        g_dec[i] = (WORD)(((LONG)m[0] + m[1] + m[2] + m[3]) >> 2);
    }
    fft_two(g_dec, g_mono + VIS_HIST - VIS_N);
    for (k = VIS_DEC; k < VIS_FINE; k++) {
        /* Glaetten auf der LEISTUNG, halb alt, halb neu. Neigung wie bei
         * den groben Bins: k fein ist k/4 grob, log2(4) = 2 Stufen. */
        d->fplin[k] = (d->fplin[k] >> 1) + (g_pwf[k] >> 1);
        flg[k] = (d->fplin[k] ? log2_256(d->fplin[k]) : 0)
               + (log2_256((ULONG)k) - 2 * 256) * 3 / 4;
    }

    /* Grob: die juengsten 1024 Werte, aus demselben Durchgang. */
    for (k = 1; k < VIS_BINS; k++) {
        /* Glaetten auf der LEISTUNG, halb alt, halb neu - das gibt
         * runde Huegel statt Zittern. Drei Viertel alt (wie im Browser)
         * lief spuerbar hinterher. Nur mit Schieben: keine Division. */
        d->plin[k] = (d->plin[k] >> 1) + (g_pw[k] >> 1);
        lg[k] = (d->plin[k] ? log2_256(d->plin[k]) : 0) + g_tilt[k];
        if (k >= VIS_MEAN0 && k < VIS_MEAN1) {
            sum += lg[k];
        }
    }

    /* Die Untergrenze folgt dem Mittelwert der groben Bins, aber traege
     * (ein Achtel je Bild) - sonst pumpt die ganze Linie mit jedem
     * Schlag. Sie gilt fuer beide. */
    sum /= (VIS_MEAN1 - VIS_MEAN0);
    if (!d->floor_ok) {
        d->floor = sum;
        d->floor_ok = TRUE;
    } else {
        d->floor += (sum - d->floor) >> 3;
    }
    lo = d->floor + VIS_OFF;

    for (k = 1; k < VIS_BINS; k++) {
        d->level[k] = shape(lg[k], lo, d->level[k]);
    }
    for (k = VIS_DEC; k < VIS_FINE; k++) {
        d->flevel[k] = shape(flg[k], lo, d->flevel[k]);
    }
    /* Bin 0 ist der Gleichanteil - keine Musik, aber hoch. Er stand als
     * senkrechter Strich am linken Rand (Foto des Anwenders). Fein
     * beginnt die Anzeige bei 43 Hz (fein 4), darunter wird nie
     * gelesen. */
    d->level[0] = d->level[1];
}

/* ------------------------------------------------------------------ */
/* Messen                                                              */
/* ------------------------------------------------------------------ */

static ULONG eclock(ULONG *efreq)
{
    struct EClockVal ev;
    ULONG f;

    if (!TimerBase) {
        return 0;
    }
    f = ReadEClock(&ev);
    if (efreq) {
        *efreq = f;
    }
    return ev.ev_lo;
}

#if VIS_MEASURE
/* Mittelwert in Hundertstel-Millisekunden. */
static ULONG avg_cms(ULONG sum, ULONG n, ULONG efreq)
{
    if (!n || efreq < 100) {
        return 0;
    }
    return (sum / n) * 1000UL / (efreq / 100UL);
}

static void measure_log(struct VisData *d, LONG w, LONG h)
{
    BPTR fh;
    ULONG now, el, fps100;

    /* Vergangene Zeit in Hundertstelsekunden, daraus Bilder je Sekunde
     * mal 100 - so bleibt es ganzzahlig und passt in 32 Bit. */
    now = eclock(NULL);
    el = (d->m_efreq >= 100) ? (now - d->m_start) / (d->m_efreq / 100UL) : 0;
    fps100 = el ? d->m_frames * 10000UL / el : 0;

    fh = Open((STRPTR)VIS_LOG, MODE_READWRITE);
    if (fh) {
        Seek(fh, 0, OFFSET_END);
        FPrintf(fh, "%ldx%ld: %ld Bilder, %ld.%02ld Bilder/s | FFT %ld.%02ld ms"
                " | Zeichnen %ld.%02ld ms | WritePixelArray %ld.%02ld ms\n",
                w, h, (LONG)d->m_frames,
                (LONG)(fps100 / 100), (LONG)(fps100 % 100),
                (LONG)(avg_cms(d->m_fft, d->m_frames, d->m_efreq) / 100),
                (LONG)(avg_cms(d->m_fft, d->m_frames, d->m_efreq) % 100),
                (LONG)(avg_cms(d->m_render, d->m_frames, d->m_efreq) / 100),
                (LONG)(avg_cms(d->m_render, d->m_frames, d->m_efreq) % 100),
                (LONG)(avg_cms(d->m_wpa, d->m_frames, d->m_efreq) / 100),
                (LONG)(avg_cms(d->m_wpa, d->m_frames, d->m_efreq) % 100));
        Close(fh);
    }
    d->m_frames = d->m_fft = d->m_render = d->m_wpa = 0;
    d->m_start = now;
}
#endif

/* ------------------------------------------------------------------ */
/* Takt                                                                */
/* ------------------------------------------------------------------ */

static void timer_send(struct VisData *d);

static ULONG vis_tick(struct IClass *cl, Object *obj)
{
    struct VisData *d = INST_DATA(cl, obj);
    ULONG t0;
    LONG k;

    /* Nur wenn unsere Anforderung wirklich zurueck ist - das Signal
     * kann auch uebrig sein. Dann SOFORT die naechste losschicken: der
     * Takt zaehlt ab jetzt, nicht ab dem Ende dieses Bildes. */
    if (!d->tpending || !CheckIO((struct IORequest *)d->treq)) {
        return 0;
    }
    WaitIO((struct IORequest *)d->treq);
    d->tpending = FALSE;
    if (d->ticking) {
        timer_send(d);
    }
    t0 = eclock(NULL);

    if (audio_vis_window(g_mono, VIS_HIST)) {
        spectrum(d);
        d->moving = TRUE;
    } else if (d->moving) {
        /* Nichts zu hoeren (Pause, Stop): die Linie sinkt ab, statt
         * stehen zu bleiben. */
        BOOL any = FALSE;

        for (k = 0; k < VIS_BINS; k++) {
            d->plin[k] -= d->plin[k] >> 2;
            d->level[k] = (WORD)((d->level[k] * 3L) >> 2);
            if (d->level[k] > 0) {
                any = TRUE;
            }
        }
        for (k = 0; k < VIS_FINE; k++) {
            d->fplin[k] -= d->fplin[k] >> 2;
            d->flevel[k] = (WORD)((d->flevel[k] * 3L) >> 2);
            if (d->flevel[k] > 0) {
                any = TRUE;
            }
        }
        d->moving = any;
    } else {
        return 0;                       /* alles flach - nichts zu tun */
    }
#if VIS_MEASURE
    d->m_fft += eclock(NULL) - t0;
#else
    (void)t0;
#endif
    MUI_Redraw(obj, MADF_DRAWUPDATE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static BOOL ensure_buf(struct VisData *d, LONG w, LONG h)
{
    ULONG px, bytes;

    if (d->buf && d->buf_w == w && d->buf_h == h) {
        return TRUE;
    }
    if (d->buf) {
        FreeVec(d->buf);
        d->buf = NULL;
    }
    /* AllocVec, nicht malloc: MUI kann jederzeit ein Neuzeichnen
     * verlangen, auch mitten in einem Netzauftrag (siehe netjob.h).
     * Ein Block fuer alles: Bild, Zeilenfarben, alte Spannen. */
    px = (ULONG)w * (ULONG)h;
    bytes = px * 4UL                    /* Bild */
          + (ULONG)h * 12UL             /* rowcol, linecol, refcol */
          + (ULONG)w * 8UL              /* old0, old1, in0, in1 */
          + (ULONG)(w + 1) * 4UL        /* colbin */
          + (ULONG)w * sizeof(struct VisCol)
          + (ULONG)h + 8UL;             /* rfade */
    d->buf = AllocVec(bytes, MEMF_ANY);
    if (!d->buf) {
        return FALSE;
    }
#if VIS_MEASURE
    /* Liegt der Puffer im Chip-RAM, geht auf der PiStorm jeder Zugriff
     * ueber den echten A500-Bus - das waere der groesste Hebel. */
    {
        BPTR fh = Open((STRPTR)VIS_LOG, MODE_READWRITE);

        if (fh) {
            ULONG m = TypeOfMem(d->buf), n = TypeOfMem(g_re);

            Seek(fh, 0, OFFSET_END);
            FPrintf(fh, "Puffer %ld KB: %s, FFT-Felder: %s\n",
                    (LONG)(bytes >> 10),
                    (LONG)(ULONG)((m & MEMF_CHIP) ? "CHIP" :
                                  (m & MEMF_FAST) ? "FAST" : "?"),
                    (LONG)(ULONG)((n & MEMF_CHIP) ? "CHIP" :
                                  (n & MEMF_FAST) ? "FAST" : "?"));
            Close(fh);
        }
    }
#endif
    d->rowcol = d->buf + px;
    d->linecol = d->rowcol + h;
    d->refcol = d->linecol + h;
    d->colbin = (LONG *)(d->refcol + h);
    d->cols = (struct VisCol *)(d->colbin + w + 1);
    d->old0 = (WORD *)(d->cols + w);
    d->old1 = d->old0 + w;
    d->in0 = d->old1 + w;
    d->in1 = d->in0 + w;
    d->rfade = (UBYTE *)(d->in1 + w);
    d->buf_w = w;
    d->buf_h = h;
    d->buf_ok = FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Farben                                                              */
/* ------------------------------------------------------------------ */

/* Farbton in 1536 Stufen (6 * 256, je Sechstel des Farbkreises 256),
 * Saettigung und Helligkeit 0..255. Ganzzahlig; die Divisionen durch 255
 * laufen nur beim Aufbau der Tabellen, nicht je Bild. */
static ULONG hsv_rgb(LONG h, LONG s, LONG v)
{
    LONG i, f, p, q, t, r, g, b;

    h %= 1536;
    if (h < 0) {
        h += 1536;
    }
    i = h >> 8;
    f = h & 255;
    p = v * (255 - s) / 255;
    q = v * (255 - s * f / 255) / 255;
    t = v * (255 - s * (255 - f) / 255) / 255;
    switch (i) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
}

static LONG rgb_hue(ULONG rgb)
{
    LONG r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    LONG mx = r, mn = r, dd, h;

    if (g > mx) { mx = g; }
    if (b > mx) { mx = b; }
    if (g < mn) { mn = g; }
    if (b < mn) { mn = b; }
    dd = mx - mn;
    if (dd == 0) {
        return 1024;            /* grau: blau, wie bei Feishin unten */
    }
    if (mx == r) {
        h = (g - b) * 256 / dd;
    } else if (mx == g) {
        h = 512 + (b - r) * 256 / dd;
    } else {
        h = 1024 + (r - g) * 256 / dd;
    }
    return (h < 0) ? h + 1536 : h;
}

static ULONG blend(ULONG bg, ULONG fg, LONG a);

/* Hintergrund einmal ganz: Verlauf wie in der Liste daneben. Dazu die
 * Farben je Zeile. Danach steht alles im Puffer, und je Bild wird nur
 * noch die Linie ausgetauscht. */
static void render_bg(struct VisData *d, LONG w, LONG h)
{
    LONG r0 = (d->colour >> 16) & 0xff;
    LONG g0 = (d->colour >>  8) & 0xff;
    LONG b0 =  d->colour        & 0xff;
    LONG n = (h > 1) ? (h - 1) : 1;
    LONG x, y, hue, span, diff, refl;
    ULONG *p = d->buf;

    /* Grundlinie etwas ueber der Mitte, darunter die Spiegelung - wie im
     * Bild von Feishin. */
    d->base = h * 56 / 100;
    d->amp = d->base * 9 / 10;
    refl = h - 1 - d->base;
    if (refl > d->amp) {
        refl = d->amp;
    }

    /* Farbe nach Hoehe: vom Farbton des Covers aus bis zu 120 Grad in
     * Richtung Gelb (256), wie bei Feishin von Blau ueber Gruen nach
     * Gelb. Liegt das Cover schon nah bei Gelb (braun, orange), faengt
     * die Linie unten 60 Grad davor an und endet oben in Gelb - sonst
     * bliebe sie fast einfarbig. Der erste Anlauf drehte in dem Fall
     * weg von Gelb, und ein braunes Cover bekam pinke Spitzen. */
    hue = rgb_hue(d->colour);
    diff = 256 - hue;
    if (diff > 768) {
        diff -= 1536;
    }
    if (diff < -768) {
        diff += 1536;
    }
    if (diff > 512) {
        span = 512;
    } else if (diff < -512) {
        span = -512;
    } else if (diff >= 256 || diff <= -256) {
        span = diff;
    } else {
        span = (diff >= 0) ? 256 : -256;
        hue = 256 - span;
    }

    for (y = 0; y < h; y++) {
        LONG f = (h > 1) ? (h - 1 - y) : 1;
        ULONG v = ((ULONG)(r0 * f / n) << 16)
                | ((ULONG)(g0 * f / n) <<  8)
                |  (ULONG)(b0 * f / n);
        LONG lev;

        d->rowcol[y] = v;
        for (x = 0; x < w; x++) {
            *p++ = v;
        }

        /* Linienfarbe: ueber der Grundlinie nach der Hoehe, darunter die
         * der gespiegelten Zeile. */
        lev = (y <= d->base) ? (d->base - y) : (y - d->base);
        lev = d->amp ? lev * 4096 / d->amp : 0;
        if (lev > 4096) {
            lev = 4096;
        }
        d->linecol[y] = hsv_rgb(hue + span * lev / 4096, 190, 245);

        /* Spiegelung: gut ein Drittel Deckkraft an der Grundlinie, nach
         * unten auf null. */
        if (y > d->base && refl > 0 && y - d->base < refl) {
            d->rfade[y] = (UBYTE)(6 * (refl - (y - d->base)) / refl);
        } else {
            d->rfade[y] = 0;
        }
    }

    /* Welcher Bin an welcher Spalte, in 1/256 Bin, logarithmisch: die
     * Spalte x liegt bei log2(KMIN) + x/w * log2(KMAX/KMIN), und 2 hoch
     * das kommt aus g_exp2 (Nachkomma) und einem Schieben (Ganzzahl).
     * Eine Spalte mehr als die Breite: die rechte Grenze der letzten. */
    {
        LONG l0 = log2_256(VIS_KMIN), l1 = log2_256(VIS_KMAX);

        for (x = 0; x <= w; x++) {
            LONG lg2 = l0 + (w > 0 ? (l1 - l0) * x / w : 0);

            d->colbin[x] = (LONG)g_exp2[lg2 & 255] << (lg2 >> 8);
        }
    }
    for (x = 0; x < w; x++) {
        LONG fb = d->colbin[x], fe = d->colbin[x + 1];
        LONG taper = (w / 10 > 1) ? w / 10 : 1;
        struct VisCol *c = &d->cols[x];

        if (fb < (VIS_FINE / VIS_DEC) * 256) {
            /* Bass: aus den feinen Bins. Stelle in 1/256 grob mal 4 ist
             * die Stelle in 1/256 fein. Die Spalten sind hier fast immer
             * schmaler als ein feiner Bin - weich uebergehen. */
            LONG ff = fb * VIS_DEC, fk = ff >> 8, fr = ff & 255;

            if (fk < VIS_DEC) {
                fk = VIS_DEC;
                fr = 0;
            }
            c->mode = VC_FINE;
            c->k = (WORD)fk;
            c->mw = (fk + 1 < VIS_FINE)
                  ? (WORD)((fr * fr * (768 - 2 * fr)) >> 16) : 0;
        } else if (fe - fb >= 256) {
            /* Rechts: mehrere Bins in dieser Spalte - der lauteste
             * zaehlt, sonst verschwinden die Spitzen. */
            LONG ke = fe >> 8;

            if (ke >= VIS_BINS) {
                ke = VIS_BINS - 1;
            }
            c->mode = VC_MAX;
            c->k = (WORD)(fb >> 8);
            c->ke = (WORD)ke;
        } else {
            /* Mitten: eine Spalte ist schmaler als ein Bin - weich
             * zwischen zwei Bins uebergehen (smoothstep, f*f*(3-2f)). */
            LONG k = fb >> 8, f = fb & 255;

            c->mode = VC_MIX;
            c->k = (WORD)k;
            c->mw = (k + 1 < VIS_BINS)
                  ? (WORD)((f * f * (768 - 2 * f)) >> 16) : 0;
        }

        /* Links weich von der Grundlinie aus einblenden. Die tiefsten
         * Bins (43 bis 86 Hz) sind fast immer laut - Bass laeuft dauernd,
         * und das Fenster streut etwas Gleichanteil hinein. Ohne das
         * stand die Linie am linken Rand hoch und stieg mit einem
         * senkrechten Strich aus der Grundlinie (Fotos des Anwenders,
         * 22.9.2026); bei Feishin beginnt sie unten. */
        if (x < taper) {
            LONG t = x * 256 / taper;

            c->tap = (WORD)((t * t * (768 - 2 * t)) >> 16);  /* smoothstep */
        } else {
            c->tap = 256;
        }

        d->old0[x] = 1;
        d->old1[x] = 0;
        d->in0[x] = 1;
        d->in1[x] = 0;
    }
    /* Spiegelung bei voller Deckung: je Zeile einmal gemischt statt je
     * Bildpunkt und Bild. */
    for (y = 0; y < h; y++) {
        d->refcol[y] = d->rfade[y] ? blend(d->rowcol[y], d->linecol[y],
                                           d->rfade[y] * 16)
                                   : d->rowcol[y];
    }
    d->buf_col = d->colour;
    d->buf_ok = TRUE;
}

/* fg ueber bg mit Deckkraft a (0..256). */
static ULONG blend(ULONG bg, ULONG fg, LONG a)
{
    LONG br = (bg >> 16) & 0xff, bgg = (bg >> 8) & 0xff, bb = bg & 0xff;
    LONG fr = (fg >> 16) & 0xff, fgg = (fg >> 8) & 0xff, fb = fg & 0xff;

    br  += ((fr - br) * a) >> 8;
    bgg += ((fgg - bgg) * a) >> 8;
    bb  += ((fb - bb) * a) >> 8;
    return ((ULONG)br << 16) | ((ULONG)bgg << 8) | (ULONG)bb;
}

/* Eine Zeile als geaendert vermerken - fuer den Streifen der Spalte. */
#define MARK(sl, y) do { if ((y) < d->slo[sl]) d->slo[sl] = (y); \
                          if ((y) > d->shi[sl]) d->shi[sl] = (y); } while (0)

/* Die Linie austauschen. Danach steht in d->slo/shi je Streifen, welche
 * Zeilen sich geaendert haben - nur die gehen zur Grafikkarte.
 *
 * Jede Spalte zeichnet nur ihre eigenen Bildpunkte: die senkrechte
 * Spanne zwischen der Hoehe der vorigen Spalte und ihrer eigenen, in
 * Sechzehnteln eines Bildpunkts, um eine dreiviertel Zeile verbreitert.
 * Die Randzeilen bekommen nur ihren Anteil - das ist die Kantenglaettung.
 *
 * Und nur, was sich AENDERT (22.9.2026: 6,5 ms je Bild fuer das
 * Zeichnen, mehr als die FFT): zwischen zwei Bildern ueberlappen sich
 * die Spannen fast immer, und innen steht beide Male dieselbe Farbe -
 * sie haengt nur an der Zeile. Also: wiederherstellen, was herausfaellt;
 * innen nur schreiben, was vorher nicht innen war; mischen nur an den
 * hoechstens zwei Randzeilen. Das Ergebnis ist Bildpunkt fuer Bildpunkt
 * dasselbe wie vorher (am Mac nachgeprueft). */
static void render_line(struct VisData *d, LONG w, LONG h)
{
    LONG base16 = d->base * 16;
    LONG x, y, prev16 = base16;
    LONG b2 = 2 * d->base;
    LONG sl = 0, snext = w / 8;

    for (x = 0; x < 8; x++) {
        d->slo[x] = h;
        d->shi[x] = -1;
    }

    for (x = 0; x < w; x++) {
        const struct VisCol *c = &d->cols[x];
        LONG lev, y16, s0, s1;
        LONG ya, yb, fa, fz, o0, o1, i0, i1, lo, hi, my;
        ULONG *col = d->buf + x;

        if (x >= snext && sl < 7) {
            sl++;
            snext = (sl + 1) * w / 8;
        }

        /* Pegel der Spalte, nach der Tabelle aus render_bg(). */
        if (c->mode == VC_MAX) {
            const WORD *lv = d->level + c->k;
            LONG n = c->ke - c->k;

            lev = 0;
            do {
                if (*lv > lev) {
                    lev = *lv;
                }
                lv++;
            } while (--n >= 0);
        } else {
            const WORD *lv = (c->mode == VC_FINE) ? d->flevel + c->k
                                                  : d->level + c->k;

            lev = (c->mw) ? (lv[0] * (256 - c->mw) + lv[1] * c->mw) >> 8
                          : lv[0];
        }
        lev = (lev * c->tap) >> 8;

        /* Hoehe in Sechzehnteln: lev * amp * 16 / 4096, als Schieben -
         * fuer lev >= 0 genau dasselbe, ohne Division. */
        y16 = base16 - ((lev * d->amp) >> 8);
        s0 = ((y16 < prev16) ? y16 : prev16) - 12;
        s1 = ((y16 < prev16) ? prev16 : y16) + 12;
        prev16 = y16;

        /* Beruehrte Zeilen ya..yb, davon voll gedeckt fa..fz. Unter der
         * Grundlinie zeichnet nur die Spiegelung. */
        ya = s0 >> 4;
        yb = (s1 - 1) >> 4;
        fa = (s0 + 15) >> 4;
        fz = (s1 >> 4) - 1;
        if (ya < 0) { ya = 0; }
        if (fa < 0) { fa = 0; }
        if (yb > d->base) { yb = d->base; }
        if (fz > d->base) { fz = d->base; }

        o0 = d->old0[x];
        o1 = d->old1[x];
        i0 = d->in0[x];
        i1 = d->in1[x];
        lo = h;
        hi = -1;

        /* 1. Was aus der Spanne herausfaellt: Hintergrund. */
        for (y = o0; y <= o1; y++) {
            if (y >= ya && y <= yb) {
                y = yb;                 /* den Rest der neuen ueberspringen */
                continue;
            }
            col[y * w] = d->rowcol[y];
            my = b2 - y;
            if (my > d->base && my < h) {
                col[my * w] = d->rowcol[my];
            }
            if (y < lo) { lo = y; }
            if (y > hi) { hi = y; }
        }

        /* 2. Innen: nur, was vorher nicht innen war. */
        for (y = fa; y <= fz; y++) {
            if (y >= i0 && y <= i1) {
                y = i1;
                continue;
            }
            col[y * w] = d->linecol[y];
            my = b2 - y;
            if (my > d->base && my < h) {
                col[my * w] = d->refcol[my];
            }
            if (y < lo) { lo = y; }
            if (y > hi) { hi = y; }
        }

        /* 3. Die Randzeilen: nur ihren Anteil, gemischt. */
        for (y = ya; y <= yb; y++) {
            LONG c0, c1, cov;

            if (y == fa && fa <= fz) {
                y = fz;                 /* innen ist schon erledigt */
                continue;
            }
            c0 = y * 16;
            c1 = c0 + 16;
            if (c0 < s0) { c0 = s0; }
            if (c1 > s1) { c1 = s1; }
            cov = c1 - c0;
            if (cov <= 0) {
                continue;
            }
            col[y * w] = blend(d->rowcol[y], d->linecol[y], cov * 16);
            my = b2 - y;
            if (my > d->base && my < h && d->rfade[my]) {
                col[my * w] = blend(d->rowcol[my], d->linecol[my],
                                    cov * d->rfade[my]);
            }
            if (y < lo) { lo = y; }
            if (y > hi) { hi = y; }
        }

        d->old0[x] = (WORD)ya;
        d->old1[x] = (WORD)yb;
        d->in0[x] = (WORD)fa;
        d->in1[x] = (WORD)fz;

        /* Streifen: oben lo, unten die Spiegelung von lo. */
        if (hi >= lo) {
            MARK(sl, lo);
            MARK(sl, hi);
            my = b2 - lo;
            if (my >= h) {
                my = h - 1;
            }
            if (my > d->base) {
                MARK(sl, my);
            }
        }
    }
}

static ULONG vis_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct VisData *d = INST_DATA(cl, obj);
    LONG l, t, w, h;
    ULONG t0, t1;

    DoSuperMethodA(cl, obj, (Msg)msg);

    if (!(msg->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))) {
        return 0;
    }
    l = _mleft(obj);
    t = _mtop(obj);
    w = _mwidth(obj);
    h = _mheight(obj);
    if (w <= 0 || h <= 0 || !CyberGfxBase) {
        return 0;
    }

    if (!ensure_buf(d, w, h)) {
        FillPixelArray(_rp(obj), (UWORD)l, (UWORD)t, (UWORD)w, (UWORD)h,
                       d->colour);
        return 0;
    }

    t0 = eclock(NULL);
    if (!d->buf_ok || d->buf_col != d->colour ||
        (msg->flags & MADF_DRAWOBJECT)) {
        /* Ganz: erstes Bild, neuer Farbton, oder MUI will alles (Fenster
         * aufgedeckt, Seite gewechselt). */
        render_bg(d, w, h);
        render_line(d, w, h);
        t1 = eclock(NULL);
        WritePixelArray(d->buf, 0, 0, (UWORD)(w * 4), _rp(obj),
                        (UWORD)l, (UWORD)t, (UWORD)w, (UWORD)h,
                        RECTFMT_ARGB);
    } else {
        /* Nur was sich bewegt hat, in acht senkrechten Streifen: eine
         * einzelne hohe Spitze macht so nur ihren Streifen hoch, nicht
         * die ganze Breite. */
        LONG sl;

        render_line(d, w, h);
        t1 = eclock(NULL);
        for (sl = 0; sl < 8; sl++) {
            LONG x0 = sl * w / 8, x1 = (sl + 1) * w / 8;

            if (d->shi[sl] >= d->slo[sl] && x1 > x0) {
                WritePixelArray(d->buf, (UWORD)x0, (UWORD)d->slo[sl],
                                (UWORD)(w * 4), _rp(obj),
                                (UWORD)(l + x0), (UWORD)(t + d->slo[sl]),
                                (UWORD)(x1 - x0),
                                (UWORD)(d->shi[sl] - d->slo[sl] + 1),
                                RECTFMT_ARGB);
            }
        }
    }
#if VIS_MEASURE
    if (msg->flags & MADF_DRAWUPDATE) {
        ULONG t2 = eclock(&d->m_efreq);

        d->m_render += t1 - t0;
        d->m_wpa += t2 - t1;
        if (d->m_start == 0) {
            d->m_start = t0;
        }
        if (++d->m_frames >= 125) {     /* etwa alle 5 s */
            measure_log(d, w, h);
        }
    }
#else
    (void)t0;
    (void)t1;
#endif
    return 0;
}

/* ------------------------------------------------------------------ */
/* Klasse                                                              */
/* ------------------------------------------------------------------ */

static ULONG vis_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct VisData *d;

    obj = (Object *)DoSuperMethodA(cl, obj, (Msg)msg);
    if (!obj) {
        return 0;
    }
    d = INST_DATA(cl, obj);
    /* Die Klasse faerbt ihre Flaeche vollstaendig selbst - siehe tabs.c
     * zu MUIA_FillArea. */
    set(obj, MUIA_FillArea, FALSE);
    d->colour = GetTagData(MUIA_Vis_Colour, 0x00281640, msg->ops_AttrList);
    d->fps = (LONG)GetTagData(MUIA_Vis_Fps, 30, msg->ops_AttrList);

    /* Fehlt der Zeitgeber, bleibt der Visualizer eben stehen - das
     * Programm laeuft sonst normal. */
    d->tport = CreateMsgPort();
    if (d->tport) {
        d->treq = (struct timerequest *)
                  CreateIORequest(d->tport, sizeof(struct timerequest));
        if (d->treq && OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
                                  (struct IORequest *)d->treq, 0) != 0) {
            DeleteIORequest((struct IORequest *)d->treq);
            d->treq = NULL;
        }
    }
    return (ULONG)obj;
}

static void timer_stop(struct VisData *d);

static ULONG vis_dispose(struct IClass *cl, Object *obj, Msg msg)
{
    struct VisData *d = INST_DATA(cl, obj);

    if (d->buf) {
        FreeVec(d->buf);
        d->buf = NULL;
    }
    if (d->treq) {
        timer_stop(d);
        CloseDevice((struct IORequest *)d->treq);
        DeleteIORequest((struct IORequest *)d->treq);
        d->treq = NULL;
    }
    if (d->tport) {
        DeleteMsgPort(d->tport);
        d->tport = NULL;
    }
    return DoSuperMethodA(cl, obj, msg);
}

static void ticker(struct VisData *d, Object *obj);

static ULONG vis_set(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct VisData *d = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *ti;

    while ((ti = NextTagItem(&tags)) != NULL) {
        if (ti->ti_Tag == MUIA_Vis_Colour && d->colour != ti->ti_Data) {
            d->colour = ti->ti_Data;
            MUI_Redraw(obj, MADF_DRAWOBJECT);
        }
        if (ti->ti_Tag == MUIA_Vis_Fps && d->fps != (LONG)ti->ti_Data) {
            d->fps = (LONG)ti->ti_Data;
            if (d->fps == 0) {
                /* Aus: die Linie faellt sofort flach, statt mitten im
                 * Ausschlag stehen zu bleiben. */
                memset(d->level, 0, sizeof(d->level));
                memset(d->flevel, 0, sizeof(d->flevel));
                memset(d->fplin, 0, sizeof(d->fplin));
                memset(d->plin, 0, sizeof(d->plin));
                d->moving = FALSE;
                if (d->shown) {
                    MUI_Redraw(obj, MADF_DRAWOBJECT);
                }
            }
            if (d->shown) {
                ticker(d, obj);
            }
        }
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static ULONG vis_askminmax(struct IClass *cl, Object *obj,
                           struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);
    msg->MinMaxInfo->MinWidth  += 80;
    msg->MinMaxInfo->DefWidth  += 400;
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MinHeight += 40;
    msg->MinMaxInfo->DefHeight += 300;
    msg->MinMaxInfo->MaxHeight += MUI_MAXMAX;
    return 0;
}

static void timer_send(struct VisData *d)
{
    ULONG us = fps_micros(d->fps);

    d->treq->tr_node.io_Command = TR_ADDREQUEST;
    d->treq->tr_time.tv_secs    = us / 1000000UL;
    d->treq->tr_time.tv_micro   = us % 1000000UL;
    SendIO((struct IORequest *)d->treq);
    d->tpending = TRUE;
}

static void timer_stop(struct VisData *d)
{
    if (d->tpending) {
        AbortIO((struct IORequest *)d->treq);
        WaitIO((struct IORequest *)d->treq);
        d->tpending = FALSE;
    }
    SetSignal(0, 1UL << d->tport->mp_SigBit);
}

/* Den Zeitgeber ein- oder aushaengen. Er laeuft NUR, wenn der
 * Visualizer zu sehen ist (zwischen Show und Hide) UND nicht auf "Off"
 * steht. Nie aus MUIM_HandleEvent heraus rufen - die Liste darf nicht
 * umgebaut werden, waehrend MUI sie durchlaeuft (AGENTS.md, 6).
 *
 * Eine neue Rate bei laufendem Zeitgeber braucht nichts: die naechste
 * Anforderung liest d->fps ohnehin neu. */
static void ticker(struct VisData *d, Object *obj)
{
    BOOL want = d->shown && d->fps > 0 && d->treq;

    if (d->ticking && !want) {
        DoMethod(_app(obj), MUIM_Application_RemInputHandler,
                 (ULONG)&d->ihn);
        timer_stop(d);
        d->ticking = FALSE;
    }
    if (want && !d->ticking) {
        memset(&d->ihn, 0, sizeof(d->ihn));
        d->ihn.ihn_Object  = obj;
        d->ihn.ihn_Signals = 1UL << d->tport->mp_SigBit;
        d->ihn.ihn_Flags   = 0;             /* Signal, nicht MUI-Timer */
        d->ihn.ihn_Method  = MUIM_Vis_Tick;
        DoMethod(_app(obj), MUIM_Application_AddInputHandler,
                 (ULONG)&d->ihn);
        timer_send(d);
        d->ticking = TRUE;
    }
}

#if VIS_MEASURE
static void measure_note(const char *what)
{
    BPTR fh = Open((STRPTR)VIS_LOG, MODE_READWRITE);

    if (fh) {
        Seek(fh, 0, OFFSET_END);
        FPrintf(fh, "%s\n", (LONG)(ULONG)what);
        Close(fh);
    }
}
#endif

/* Der Zeitgeber haengt an Show/Hide, nicht an Setup/Cleanup: er soll
 * nur laufen, solange man den Visualizer SIEHT. Hinter UP NEXT oder auf
 * der Bildwand kostet er so nichts - dieselbe Regel wie bei den
 * Ereignissen (AGENTS.md, Abschnitt 6). */
static ULONG vis_show(struct IClass *cl, Object *obj, Msg msg)
{
    struct VisData *d = INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, msg)) {
        return FALSE;
    }
    d->shown = TRUE;
    ticker(d, obj);
#if VIS_MEASURE
    d->m_frames = d->m_fft = d->m_render = d->m_wpa = 0;
    d->m_start = 0;
    measure_note(d->ticking ? "eingeblendet, Zeitgeber laeuft"
                            : "eingeblendet, Zeitgeber aus (Off)");
#endif
    return TRUE;
}

static ULONG vis_hide(struct IClass *cl, Object *obj, Msg msg)
{
    struct VisData *d = INST_DATA(cl, obj);

    d->shown = FALSE;
    ticker(d, obj);
#if VIS_MEASURE
    measure_note("ausgeblendet, Zeitgeber aus");
#endif
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG vis_dispatch(struct IClass *cl  __asm("a0"),
                          Object       *obj __asm("a2"),
                          Msg           msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW:          return vis_new(cl, obj, (struct opSet *)msg);
    case OM_DISPOSE:      return vis_dispose(cl, obj, msg);
    case OM_SET:          return vis_set(cl, obj, (struct opSet *)msg);
    case MUIM_AskMinMax:  return vis_askminmax(cl, obj,
                                  (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:       return vis_draw(cl, obj, (struct MUIP_Draw *)msg);
    case MUIM_Show:       return vis_show(cl, obj, msg);
    case MUIM_Hide:       return vis_hide(cl, obj, msg);
    case MUIM_Vis_Tick:   return vis_tick(cl, obj);
    }
    return DoSuperMethodA(cl, obj, msg);
}

/* Der Klassenname als Feld im Datenbereich statt als Zeichenkette im
 * Code: dort stand "Area.mui" direkt vor vis_init, objdump kam beim
 * Zerlegen aus dem Tritt und meldete das folgende lea als FPU-Befehl
 * (fboltl, 22.9.2026) - dieselbe Falle wie bei den Tabellen. */
static char g_area[] VIS_DATA = MUIC_Area;

BOOL vis_init(void)
{
    tables_init();
    tilt_init();
    g_mcc = MUI_CreateCustomClass(NULL, g_area, NULL,
                                  sizeof(struct VisData),
                                  (APTR)vis_dispatch);
    return g_mcc != NULL;
}

void vis_cleanup(void)
{
    if (g_mcc) {
        MUI_DeleteCustomClass(g_mcc);
        g_mcc = NULL;
    }
}
