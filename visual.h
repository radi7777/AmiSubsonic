/* AmiSubsonic - der Visualizer: Spektrum des Gehoerten, wie bei Feishin.
 *
 * Eine selbst gezeichnete Area-Klasse wie die anderen. Sie tickt nur,
 * solange sie SICHTBAR ist (MUI-Zeitgeber, eingehaengt bei MUIM_Show) -
 * auf einer anderen Seite oder hinter einem anderen Reiter kostet sie
 * nichts. */

#ifndef VISUAL_H
#define VISUAL_H

#include <exec/types.h>
#include <libraries/mui.h>

#define MUIA_Vis_Colour  (TAG_USER | 0x41565301)  /* Farbton, 0x00RRGGBB */
#define MUIA_Vis_Fps     (TAG_USER | 0x41565302)  /* 0 = aus, 15, 20, 30, 60 */

/* Interne Methode, die der Zeitgeber aufruft. */
#define MUIM_Vis_Tick    (TAG_USER | 0x41565310)

BOOL vis_init(void);
void vis_cleanup(void);
struct MUI_CustomClass *vis_class(void);

#define VisualObject  NewObject(vis_class()->mcc_Class, NULL

#endif
