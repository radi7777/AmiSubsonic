/* AmiSubsonic - Albumcover laden und seine Hauptfarbe bestimmen.
 *
 * Das Bild wird EINMAL als Rohpixel geholt. Daraus kommt beides: die
 * Hauptfarbe fuer den Farbverlauf und die Bildpunkte zum Zeichnen.
 *
 * Der erste Anlauf ging ueber die fertige BitMap des datatype-Objekts
 * (PDTA_BitMap) und blittete die. Das sah gerastert aus: picture.datatype
 * rechnet dafuer auf den Bildschirm um, und dabei meldete PDTA_NumColors
 * sechzehn - es dithert also auf wenige Farben herunter, obwohl der
 * Schirm Truecolor ist. Ueber PDTM_READPIXELARRAY kommen die Bildpunkte
 * dagegen unveraendert als RGB heraus, und da die Anzeigeflaeche ohnehin
 * selbst zeichnet, brauchen wir gar keine BitMap.
 */

#ifndef COVER_H
#define COVER_H

#include <exec/types.h>

/* Ein geladenes Cover als Rohpixel, drei Bytes je Bildpunkt. */
struct CoverImage {
    UBYTE *rgb;                 /* w * h * 3 Bytes, oder NULL */
    LONG   w, h;
};

/* Eine fertig verkleinerte Miniatur. Steht hier und nicht bei der
 * Bildwand, weil sie inzwischen an zwei Stellen gebraucht wird: im
 * Alben-Raster und in der Titelliste. rgb == NULL heisst "noch nicht
 * da" - der Zeichner setzt dann einen Platzhalter. */
struct AlbumThumb {
    UBYTE *rgb;                 /* w * h * 3 Bytes, oder NULL */
    LONG   w, h;
};

BOOL cover_load(const char *path, struct CoverImage *img);
void cover_unload(struct CoverImage *img);

/* Verkleinert ein geladenes Bild auf ein Quadrat der Kantenlaenge box,
 * Seitenverhaeltnis bleibt. Der Puffer kommt aus malloc() und gehoert
 * dem Aufrufer.
 *
 * Naechster Nachbar, kein Mitteln: auf 68k ist das der Unterschied
 * zwischen "faellt nicht auf" und "der Albumwechsel ruckelt" - gemittelt
 * braeuchte jeder Zielpunkt vier Quellpunkte und eine Division. Beim
 * VERKLEINERN sieht man den Unterschied ohnehin kaum.
 *
 * grow entscheidet, was bei einem zu KLEINEN Bild passiert. FALSE laesst
 * es in seiner Groesse - so will es die Bildwand, dort saehe ein
 * hochgezogenes Cover in einer Zelle voller scharfer Nachbarn schlecht
 * aus. TRUE zieht es auf die Kantenlaenge hoch; das braucht die grosse
 * Anzeigeflaeche, die sich der Fenstergroesse anpassen soll. Die Quelle
 * ist 300 Punkte gross, ueber etwa das Anderthalbfache hinaus wird es
 * also sichtbar weich. */
BOOL cover_scale_rgb(const struct CoverImage *src, LONG box, BOOL grow,
                     UBYTE **out, LONG *outw, LONG *outh);

/* Hauptfarbe aus einem bereits geladenen Bild, als 0x00RRGGBB. FALSE,
 * wenn keine brauchbare Farbe herauskommt. */
BOOL cover_dominant_img(const struct CoverImage *img, ULONG *rgb);

/* Bequemlichkeit fuer das Shell-Werkzeug: laden, rechnen, freigeben. */
BOOL cover_dominant(const char *path, ULONG *rgb);

/* Warum das letzte cover_load()/cover_dominant() gescheitert ist. */
const char *cover_last_error(void);

/* Zielhelligkeit fuer den oberen Rand des Verlaufs, 0..255. Abgelesen
 * am Feishin-Schirmbild: dessen Violett 0x512D7E hat Helligkeit 65. */
#define GRAD_TOP_LUM 68

/* Alle Kanaele mit einem Prozentsatz skalieren, ganzzahlig, mit
 * Begrenzung auf 255. */
ULONG cover_scale(ULONG rgb, int percent);

/* Die Hauptfarbe auf GRAD_TOP_LUM bringen - hellt dunkle Farben auf UND
 * dunkelt helle ab, der Farbton bleibt. */
ULONG cover_gradient_top(ULONG rgb);

#endif
