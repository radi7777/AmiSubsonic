/* AmiSubsonic - die Bedienleiste unten, selbst gezeichnet.
 *
 * Vorbild ist der Player von Feishin: links ein Miniatur-Cover mit Titel,
 * Interpret und Album darunter, mittig die Transportknoepfe mit einem
 * gefuellten Kreis fuer Play/Pause, darunter Position, Fortschrittsbalken
 * und Dauer, rechts die Lautstaerke. Alles auf Schwarz.
 *
 * Warum wieder eine eigene Klasse und keine MUI-Knoepfe: MUI zeichnet
 * seine Knoepfe im Systemstil mit Rahmen und Fuellmuster. Ein flaches
 * Symbol auf schwarzem Grund, ein gefuellter Kreis und ein
 * Fortschrittsbalken in der Albumfarbe sind damit nicht zu machen.
 * Dieselbe Entscheidung wie bei panel.c und tracklist.c, aus demselben
 * Grund.
 */

#ifndef PLAYER_H
#define PLAYER_H

#include <exec/types.h>
#include <libraries/mui.h>

/* Angaben zum laufenden Titel. Alle Zeichenketten werden KOPIERT. */
#define MUIA_Pl_Title     (TAG_USER | 0x41504c01)
#define MUIA_Pl_Artist    (TAG_USER | 0x41504c02)
#define MUIA_Pl_Album     (TAG_USER | 0x41504c03)

/* Dateiname des Covers fuer die Miniatur. Wird kopiert. */
#define MUIA_Pl_Cover     (TAG_USER | 0x41504c04)

/* Farbe des Fortschrittsbalkens - die Hauptfarbe des Albums, damit die
 * Leiste zum Rest passt, ohne selbst bunt zu werden. */
#define MUIA_Pl_Colour    (TAG_USER | 0x41504c05)

#define MUIA_Pl_Pos       (TAG_USER | 0x41504c06)  /* Sekunden */
#define MUIA_Pl_Total     (TAG_USER | 0x41504c07)  /* Sekunden */
#define MUIA_Pl_Playing   (TAG_USER | 0x41504c08)  /* BOOL */
#define MUIA_Pl_Volume    (TAG_USER | 0x41504c09)  /* 0..26, wie AmigaAMP */

/* Wird auf einen der PLB_-Werte gesetzt, wenn der Anwender drueckt.
 * Darauf haengt die Oberflaeche ihre Benachrichtigung. */
#define MUIA_Pl_Pressed   (TAG_USER | 0x41504c0a)

/* Beim Ziehen im Fortschrittsbalken bzw. am Lautstaerkeregler: der neue
 * Wert steht in MUIA_Pl_SeekTo bzw. MUIA_Pl_Volume, bevor MUIA_Pl_Pressed
 * gemeldet wird. */
#define MUIA_Pl_SeekTo    (TAG_USER | 0x41504c0b)  /* Sekunden */

/* Die Leiste zeigt drei Knoepfe: PREV, PLAY, NEXT. Dazu kommen der
 * Fortschrittsbalken (SEEK) und der Lautstaerkeregler (VOLUME).
 *
 * STOP, SHUFFLE und REPEAT werden NICHT MEHR GEZEICHNET - Zufall und
 * Wiederholen taten nie etwas Sinnvolles, und Stop ist neben einer
 * Pausetaste eine Verdopplung. Die Nummern bleiben stehen, damit sich
 * die uebrigen nicht verschieben; ihre Trefferflaechen setzt player.c
 * bei jedem Zeichnen ausdruecklich auf Null. */
#define PLB_NONE     0
#define PLB_STOP     1      /* nicht gezeichnet */
#define PLB_SHUFFLE  2      /* nicht gezeichnet */
#define PLB_PREV     3
#define PLB_PLAY     4
#define PLB_NEXT     5
#define PLB_REPEAT   6      /* nicht gezeichnet */
#define PLB_SEEK     7
#define PLB_VOLUME   8
#define PLB_COVER    9      /* Miniatur links: dorthin, wo es herkommt */

BOOL pl_init(void);
void pl_cleanup(void);
struct MUI_CustomClass *pl_class(void);

#define PlayerObject  NewObject(pl_class()->mcc_Class, NULL

#endif
