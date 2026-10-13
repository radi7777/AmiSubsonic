/* AmiSubsonic - die ECS-Fassung: eigener Bildschirm mit 8 Farben.
 *
 * Wunsch des Anwenders (13.10.2026): Amiga 500 mit 68030 und ECS,
 * 640x512, kein Farbverlauf, "eine feste Farbe aus dem Album, die
 * passt". Die Palette:
 *
 *   0 Schwarz      Seitenleiste, Bedienleiste, Bildwand
 *   1 Weiss        Text
 *   2 Hellblau     Ueberschriften der Seitenleiste
 *   3 Hellgrau     Nebentext
 *   4 Albumfarbe   die Flaeche der Playeransicht und der Listen
 *   5 dunkel       Schatten, Zwischenton fuer die Cover
 *   6 hell         laufender Titel, Linie unter dem Reiter
 *   7 Dunkelgrau   Auswahl, Balken, Rollleiste
 *
 * Nur 4 bis 6 aendern sich, und zwar ueber SetRGB32: die Flaeche faerbt
 * sich um, ohne neu gezeichnet zu werden. Die Stifte sind EXKLUSIV
 * belegt, damit MUI sie nicht fuer eigene Farben nimmt - sonst faerbte
 * sich beim Albumwechsel auch das Einstellfenster mit. */

#ifndef ECSGFX_H
#define ECSGFX_H

#include <exec/types.h>
#include <graphics/rastport.h>
#include <intuition/screens.h>

#define EPEN_BLACK   0
#define EPEN_WHITE   1
#define EPEN_BLUE    2
#define EPEN_LGREY   3
#define EPEN_ALB     4
#define EPEN_ALBDK   5
#define EPEN_ALBLT   6
#define EPEN_DGREY   7

/* Den Bildschirm oeffnen (640 breit, Hoehe nach Monitor, 3 Ebenen) und
 * oeffentlich machen. NULL, wenn es nicht geht. */
struct Screen *ecs_screen_open(ULONG displayid, const char *pubname);
void ecs_screen_close(void);

/* Die drei Albumfarben (Flaeche, dunkel, hell) zu einer Hauptfarbe. */
void ecs_album_colours(ULONG rgb, ULONG out[3]);

/* Rastern beim Umwandeln, ohne gemeinsamen Zustand (Netzprozess):
 * Graustufe 0..3 je Punkt, bzw. Stifte der Palette dieses Albums. */
void  ecs_dither_levels(const UBYTE *rgb, LONG w, LONG h, UBYTE *lev);
UBYTE ecs_level_pen(UBYTE lev);
void  ecs_dither_colour(const UBYTE *rgb, LONG w, LONG h, ULONG dominant,
                        UBYTE *pens);

/* Albumfarbe setzen: rechnet die Stifte 4 bis 6 aus und setzt sie. */
void ecs_set_album(ULONG rgb);

/* Naechster Stift zu einer RGB-Farbe (0x00RRGGBB). */
UBYTE ecs_pen(ULONG rgb);

/* MUI-Stift -> Stift der Palette (siehe MPEN in pix.h). */
ULONG ecs_mpen(const UWORD *pens, LONG i);

/* Ersatz fuer FillPixelArray und WritePixelArray (siehe pix.h). */
void ecs_fill(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h, ULONG rgb);
void ecs_fill_pen(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h,
                  UBYTE pen);
ULONG ecs_write(APTR src, LONG sx, LONG sy, LONG mod, struct RastPort *rp,
                LONG x, LONG y, LONG w, LONG h, LONG fmt);

/* Wie ecs_write, aber gerastert:
 *   ECS_W_GRAY  vier Graustufen (Bildwand, Listen, Bedienleiste)
 *   ECS_W_COLOR alle Farben ausser Hellblau (das grosse Cover, in der
 *               Albumfarbe) */
#define ECS_W_NEAR  0
#define ECS_W_GRAY  1
#define ECS_W_COLOR 2
#define ECS_W_PENS  3       /* schon fertige Stifte, ein Byte je Punkt */
ULONG ecs_write_mode(APTR src, LONG sx, LONG sy, LONG mod,
                     struct RastPort *rp, LONG x, LONG y, LONG w, LONG h,
                     LONG fmt, LONG mode);

#endif
