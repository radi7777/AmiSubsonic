/* AmiSubsonic - die Reiterleiste ueber der Liste (UP NEXT / LYRICS).
 *
 * WARUM NICHT Register.mui
 *
 * MUIs Register bringt seine eigenen Laschen im Systemlook mit, und die
 * lassen sich nicht einfaerben. Der einzige Hebel waere MUIA_Background
 * am Register selbst - dann wird aber auch die AKTIVE Lasche dunkel und
 * ihre Beschriftung unlesbar. Genau daran ist der erste Anlauf schon
 * gescheitert; im Quelltext von gui.c stand deshalb lange der Hinweis,
 * den Inhalt zu faerben und das Register in Ruhe zu lassen.
 *
 * Diese Klasse ist nur die LEISTE, nicht der Seitenwechsel. Die Seiten
 * bleiben eine gewoehnliche MUI-Gruppe mit MUIA_Group_PageMode - die
 * Mechanik funktioniert, es ging immer nur um das Aussehen der Laschen.
 */

#ifndef TABS_H
#define TABS_H

#include <exec/types.h>
#include <libraries/mui.h>

/* Die Beschriftungen. Ein Feld von Zeigern, mit NULL abgeschlossen, wie
 * bei MUIA_Register_Titles. Wird NICHT kopiert - es muss leben, solange
 * das Objekt lebt. Nur beim Anlegen. */
#define MUIA_Tb_Titles   (TAG_USER | 0x41544201)

#define MUIA_Tb_Colour   (TAG_USER | 0x41544202)  /* Grundfarbe, 0x00RRGGBB */
#define MUIA_Tb_Active   (TAG_USER | 0x41544203)  /* LONG, gewaehlter Reiter */
#define MUIA_Tb_Pressed  (TAG_USER | 0x41544204)  /* LONG, zum Benachrichtigen */

BOOL tb_init(void);
void tb_cleanup(void);
struct MUI_CustomClass *tb_class(void);

#define TabsObject  NewObject(tb_class()->mcc_Class, NULL

#endif
