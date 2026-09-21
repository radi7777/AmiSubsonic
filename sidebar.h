/* AmiSubsonic - die schwarze Seitenleiste links.
 *
 * Wieder ein selbstzeichnendes Area-BLATT und keine Gruppe aus
 * MUI-Knoepfen. Zwei Gruende:
 *
 *   - Die Leiste ist durchgehend schwarz, auch zwischen den Eintraegen.
 *     Eine Gruppe aus Knoepfen haette MUIs Rahmen und Zwischenraeume in
 *     der Grundfarbe dazwischen - genau der graue Streifen, der bei
 *     panel.c schon einmal Zeit gekostet hat.
 *   - Ueberschriften ("Navidrome", "Folder") und noch nicht
 *     benutzbare Eintraege sollen anders aussehen als die aktiven. Mit
 *     Knoepfen waere das ein Sonderfall je Eintrag.
 *
 * Welche Eintraege es gibt, steht in der Klasse selbst - die Leiste ist
 * die feste Gliederung des Programms, keine Datenliste.
 */

#ifndef SIDEBAR_H
#define SIDEBAR_H

#include <exec/types.h>
#include <libraries/mui.h>

/* Die waehlbaren Eintraege. Die Nummern gehen an gui.c, deshalb hier
 * und nicht in der .c-Datei.
 *
 * Es sind genau diese sechs, und alle sechs sind waehlbar - nichts ist
 * mehr nur hingezeichnet. Die Beschriftung ist englisch, wie im Vorbild
 * Feishin; die Meldungen in der Statuszeile bleiben deutsch. */
#define SB_HOME       0
#define SB_ALBUMS     1
#define SB_TRACKS     2
#define SB_FAVORITES  3
#define SB_RADIO      4
/* Der eigene Bestand ist genauso zweigeteilt wie der vom Server:
 * eine Bildwand mit Alben und eine Liste aller Titel. */
#define SB_FOLDER_ALBUMS  5
#define SB_FOLDER_TRACKS  6
#define SB_SCAN           7
#define SB_SETTINGS       8

/* Kein Eintrag markiert. Die Playeransicht ist kein Punkt der
 * Gliederung - waehrend sie offen ist, soll die Leiste nicht behaupten,
 * man stuende woanders. */
#define SB_NONE   (-1)

#define MUIA_Sb_Active   (TAG_USER | 0x41534201)  /* LONG, SB_... */
#define MUIA_Sb_Pressed  (TAG_USER | 0x41534202)  /* LONG, zum Benachrichtigen */

BOOL sb_init(void);
void sb_cleanup(void);
struct MUI_CustomClass *sb_class(void);

#define SidebarObject  NewObject(sb_class()->mcc_Class, NULL

#endif
