/* AmiSubsonic - Bildpunkte schreiben, auf RTG und auf ECS.
 *
 * Die Klassen zeichnen ihre Flaechen ueber FillPixelArray und
 * WritePixelArray aus cybergraphics.library. Auf einem ECS-Amiga gibt es
 * die Bibliothek nicht - dort lenkt dieser Kopf beide Aufrufe auf
 * ecsgfx.c um, das die Farben auf die 8 Stifte des eigenen Bildschirms
 * abbildet. So bleibt der Zeichencode in beiden Fassungen derselbe.
 *
 * PIXOK ersetzt die Abfrage "ist cybergraphics offen?": auf ECS ist die
 * Umlenkung immer da. */

#ifndef PIX_H
#define PIX_H

#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

#ifdef AMI_ECS

#include "ecsgfx.h"

#undef FillPixelArray
#undef WritePixelArray
#define FillPixelArray(rp, x, y, w, h, rgb) \
    ecs_fill((rp), (x), (y), (w), (h), (rgb))
#define WritePixelArray(src, sx, sy, mod, rp, x, y, w, h, fmt) \
    ecs_write((src), (sx), (sy), (mod), (rp), (x), (y), (w), (h), (fmt))
#define PIXOK 1

/* Textfarben: MUIs Stifte waeren auf 8 Farben irgendein naechstliegender
 * Ton (das Grau fuer Nebentext landete fast schwarz). Die Palette hat
 * eigene Stifte dafuer. */
#define MPEN(obj, i) ecs_mpen(_pens(obj), (i))

/* Cover gerastert schreiben (siehe ecs_write_mode). */
#define WritePixelArrayMode(src, sx, sy, mod, rp, x, y, w, h, fmt, mode) \
    ecs_write_mode((src), (sx), (sy), (mod), (rp), (x), (y), (w), (h), \
                   (fmt), (mode))
/* Cover kommen auf ECS schon fertig gerastert aus der .ecs-Ablage (ein
 * Byte je Punkt, siehe cover.h) und werden nur noch kopiert. */
#define PIX_GRAY  ECS_W_PENS
#define PIX_COLOR ECS_W_PENS
#define PIXBPP    1

/* Eine Farbe fuer RTG, eine fuer ECS. Fuer ECS eine Farbe der Palette
 * angeben - die Tabelle findet dann genau ihren Stift. */
#define PIXRGB(rtg, ecs) (ecs)

#else

#define MPEN(obj, i) (_pens(obj)[(i)])

/* Auf RTG gibt es nichts zu rastern. */
#define WritePixelArrayMode(src, sx, sy, mod, rp, x, y, w, h, fmt, mode) \
    WritePixelArray((src), (sx), (sy), (mod), (rp), (x), (y), (w), (h), (fmt))
#define PIX_GRAY  1
#define PIX_COLOR 2
#define PIXBPP    3
#define PIXRGB(rtg, ecs) (rtg)

extern struct Library *CyberGfxBase;
#define PIXOK (CyberGfxBase != NULL)

#endif

#endif
