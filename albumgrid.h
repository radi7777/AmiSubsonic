/* AmiSubsonic - die Alben als Bildwand.
 *
 * WARUM WIEDER EINE EIGENE KLASSE
 *
 * Dieselbe Begruendung wie bei tracklist.c, nur schaerfer: hier stehen
 * BILDER im Raster, und Bilder bekommt man in MUIs List.mui ueberhaupt
 * nicht unter - dort gehen nur Text und Escape-Sequenzen. Ausserdem soll
 * der Farbverlauf durchlaufen, und den kann MUIA_Background nicht.
 *
 * Also wie ueberall in diesem Programm: ein Area-BLATT, das alles selbst
 * zeichnet. Keine MUI-Kinder, mit denen man sich um den Hintergrund
 * streiten koennte.
 *
 * WEM GEHOEREN DIE DATEN
 *
 * Weder die Albenliste noch die Miniaturen werden kopiert - die Klasse
 * zeigt nur darauf. Das ist Absicht: die Miniaturen entstehen nach und
 * nach, waehrend die Cover eintrudeln, und der Auftraggeber (gui.c) muss
 * sie ohnehin verwalten, weil nur er weiss, wann der Netzprozess gerade
 * nichts anfasst. Er muss die Liste also am Leben halten, solange sie
 * gesetzt ist, und sie ABHAENGEN, bevor er sie freigibt.
 */

#ifndef ALBUMGRID_H
#define ALBUMGRID_H

#include <exec/types.h>
#include <libraries/mui.h>

#include "amisub.h"
#include "cover.h"

/* Kantenlaenge des Bildfeldes je Zelle. Oeffentlich, weil der
 * Auftraggeber die Miniaturen genau auf dieses Mass herunterrechnet. */
#define AG_THUMB_SIZE 100

/* struct AlbumThumb steht in cover.h - die Titelliste braucht sie auch. */

/* Ueberschrift ueber der Wand ("Most Played", "Albums"). Sie gehoert in
 * die Klasse und nicht als eigenes Textobjekt darueber: der Verlauf
 * laeuft dann ohne Naht durch, und ein MUI-Text auf einem Verlauf ist in
 * diesem Programm schon einmal ein halber Tag gewesen (AGENTS.md 6).
 * Die Zeichenkette wird KOPIERT, anders als Liste und Miniaturen. */
#define MUIA_AG_Title   (TAG_USER | 0x41414704)  /* STRPTR */

#define MUIA_AG_Colour  (TAG_USER | 0x41414701)  /* Verlauf oben, 0x00RRGGBB */
#define MUIA_AG_Active  (TAG_USER | 0x41414702)  /* LONG, gewaehltes Album */
#define MUIA_AG_Click   (TAG_USER | 0x41414703)  /* BOOL, zum Benachrichtigen */

/* Liste und Miniaturfeld setzen. Beides wird nur referenziert; das Feld
 * muss so viele Eintraege haben wie die Liste. NULL haengt beides ab. */
#define MUIM_AG_SetList (TAG_USER | 0x41414710)
struct MUIP_AG_SetList {
    ULONG MethodID;
    struct SubList   *list;
    struct AlbumThumb *thumbs;
};

/* "Eine Miniatur ist dazugekommen" - neu zeichnen. Ein eigener Aufruf
 * statt eines Attributs, weil sich am Zustand der Klasse nichts aendert;
 * die Daten liegen ja beim Auftraggeber. */
#define MUIM_AG_Refresh (TAG_USER | 0x41414711)

/* Nur zum Suchen: schreibt bei jedem Klick eine Zeile nach
 * RAM:AmiSubsonic/click.log - Mausposition, Rollstand, Spalten und der
 * daraus errechnete Index. Kostet nichts, solange nicht geklickt wird. */
#define AG_DEBUG_CLICK 0

BOOL ag_init(void);
void ag_cleanup(void);
struct MUI_CustomClass *ag_class(void);

#define AlbumGridObject  NewObject(ag_class()->mcc_Class, NULL

#endif
