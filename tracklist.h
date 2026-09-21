/* AmiSubsonic - selbst gezeichnete Liste mit Farbverlauf-Hintergrund.
 *
 * WARUM NICHT List.mui
 *
 * Der Hintergrund soll derselbe Farbverlauf sein wie auf der Coverseite.
 * Mit MUIs Bordmitteln geht das nicht:
 *
 *   - MUIA_Background kennt als Vollton nur "2:<r>,<g>,<b>" - keinen
 *     Verlauf.
 *   - Ein externes Bild ("5:<datei>") KACHELT MUI, es zieht es nicht auf
 *     die Flaeche; und schlimmer: einen Wechsel zur LAUFZEIT uebernimmt
 *     MUI gar nicht. Nachgewiesen daran, dass sich die beim Programmstart
 *     geladene Datei spaeter nicht loeschen liess ("object is in use"),
 *     die beim Albumwechsel geschriebene dagegen schon - MUI hielt also
 *     weiter die alte.
 *
 * Also dasselbe Vorgehen wie bei panel.c: ein Area-Blatt, das alles
 * selbst zeichnet. Nebenbei faellt damit eine weitere MUI-Grenze weg -
 * echte Gadgets lassen sich in List.mui-Zeilen ohnehin nicht setzen, und
 * Bilder nur ueber Escape-Sequenzen. Fuer die kleinen Cover je Zeile aus
 * der Feishin-Vorlage ist das hier der einzige gangbare Weg.
 */

#ifndef TRACKLIST_H
#define TRACKLIST_H

#include <exec/types.h>
#include <libraries/mui.h>

#include "amisub.h"
#include "cover.h"

/* Was die Liste zeigt. */
#define TLK_SONGS   0       /* struct Song  - Nr, Titel, Dauer */
#define TLK_ALBUMS  1       /* struct Album - Interpret, Album, Jahr */
#define TLK_TEXT    2       /* Zeilen aus einem Textpuffer */
#define TLK_TRACKS  3       /* struct Song  - Cover, Titel, Interpret,
                             * Album, Dauer. Die Ansicht "Tracks". */
#define TLK_RADIO   4       /* struct Radio - Sendersymbol, Name,
                             * Homepage. Die Ansicht "Radio". */

#define MUIA_TL_Kind        (TAG_USER | 0x41544c01)  /* TLK_..., nur beim Anlegen */
#define MUIA_TL_Colour      (TAG_USER | 0x41544c02)  /* Verlauf oben, 0x00RRGGBB */
#define MUIA_TL_Active      (TAG_USER | 0x41544c03)  /* LONG, gewaehlte Zeile */
#define MUIA_TL_Playing     (TAG_USER | 0x41544c04)  /* LONG, laufender Titel */
#define MUIA_TL_DoubleClick (TAG_USER | 0x41544c05)  /* BOOL, zum Benachrichtigen */

/* Ueberschrift ueber der Liste ("Tracks", "Favorites", "Radio"). Wie bei
 * der Bildwand ein fester Streifen oben, der NICHT mitrollt - und aus
 * demselben Grund in der Klasse und nicht als MUI-Text darueber: der
 * Verlauf laeuft dann ohne Naht durch. Leer heisst: kein Streifen. */
#define MUIA_TL_Title       (TAG_USER | 0x41544c06)  /* STRPTR */

/* Kantenlaenge der kleinen Cover in TLK_TRACKS. Oeffentlich, weil der
 * Auftraggeber die Miniaturen genau auf dieses Mass herunterrechnet. */
#define TL_THUMB_SIZE 24

/* Die Liste zeigt auf fremde Daten und kopiert sie NICHT. Der Aufrufer
 * muss sie am Leben halten, solange sie gesetzt sind - und die Liste
 * leeren, bevor er sie freigibt. */
#define MUIM_TL_SetList     (TAG_USER | 0x41544c10)
struct MUIP_TL_SetList { ULONG MethodID; struct SubList *list; };

/* Fuer TLK_TRACKS: je Zeile ein ZEIGER auf die Miniatur ihres Albums.
 * Zeiger und kein Feld von Miniaturen, weil sich viele Titel dasselbe
 * Cover teilen - die Miniatur liegt genau einmal beim Auftraggeber, und
 * beim Freigeben kann nichts doppelt drankommen. Das Feld selbst gehoert
 * ebenfalls dem Auftraggeber und muss so lang sein wie die Liste. */
#define MUIM_TL_SetThumbs   (TAG_USER | 0x41544c12)
struct MUIP_TL_SetThumbs { ULONG MethodID; struct AlbumThumb **thumbs; };

/* Fuer TLK_TEXT: der Puffer wird ebenfalls nur referenziert. */
#define MUIM_TL_SetText     (TAG_USER | 0x41544c11)
struct MUIP_TL_SetText { ULONG MethodID; char *text; };

BOOL tl_init(void);
void tl_cleanup(void);
struct MUI_CustomClass *tl_class(void);

#define TrackListObject  NewObject(tl_class()->mcc_Class, NULL

#endif
