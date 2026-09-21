/* AmiSubsonic - die "laeuft gerade"-Flaeche als EINE selbstzeichnende
 * MUI-Klasse.
 *
 * WARUM EIN EINZIGES OBJEKT UND KEINE GRUPPE MIT KINDERN
 *
 * Der erste Anlauf war eine Group-Subklasse, die den Farbverlauf malt,
 * mit Text- und Bildobjekten von MUI darin. Das ging schief, und zwar
 * hartnaeckig:
 *
 *   - MUIs eigene Auffrischung (Fenster nach vorn holen, verdeckte
 *     Flaeche freilegen) ruft MUIM_Draw bei einer Gruppe nicht so, dass
 *     der Verlauf wieder erscheint - nach einem Klick aufs Tiefengadget
 *     war die ganze Flaeche grau.
 *   - Kinder mit MUIA_FillArea, TRUE malen ihren eigenen Hintergrund in
 *     MUIs Grundfarbe darueber.
 *   - Kinder mit MUIA_FillArea, FALSE malen ihn nicht, wischen dafuer
 *     aber ihren alten Inhalt nicht weg: es blieben graue Balken in
 *     Texthoehe stehen.
 *
 * Als reines Area-BLATT hat dieselbe Zeichenroutine von Anfang an
 * tadellos funktioniert. Genau das ist diese Klasse: ein Blatt, das
 * Verlauf, Cover und Textzeilen selbst zeichnet. Damit gibt es in der
 * Flaeche keine MUI-Kinder mehr, mit denen man sich um den Hintergrund
 * streiten koennte.
 */

#ifndef PANEL_H
#define PANEL_H

#include <exec/types.h>
#include <libraries/mui.h>

/* Hauptfarbe oben, 0x00RRGGBB. Nach unten laeuft es nach Schwarz. */
#define MUIA_Panel_Colour   (TAG_USER | 0x41535001)

/* Dateiname des Covers. Wird KOPIERT - anders als bei Dtpic muss der
 * Aufrufer den Puffer also nicht am Leben halten. */
#define MUIA_Panel_Cover    (TAG_USER | 0x41535002)

/* Die vier Zeilen unter dem Cover. Alle werden kopiert. */
#define MUIA_Panel_Title    (TAG_USER | 0x41535003)
#define MUIA_Panel_Artist   (TAG_USER | 0x41535004)
#define MUIA_Panel_Album    (TAG_USER | 0x41535005)
#define MUIA_Panel_Meta     (TAG_USER | 0x41535006)

BOOL panel_init(void);
void panel_cleanup(void);
struct MUI_CustomClass *panel_class(void);

#define PanelObject  NewObject(panel_class()->mcc_Class, NULL

#endif
