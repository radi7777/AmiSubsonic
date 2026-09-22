/* AmiSubsonic - Oberflaeche.
 *
 * Aufbau nach dem Vorbild von Feishin (Schirmbilder in Screenshots/new):
 * EIN Fenster, darin links die schwarze Seitenleiste und rechts zwei
 * Seiten, zwischen denen umgeschaltet wird -
 *
 *   Seite 0 "Start": die Alben als Bildwand
 *   Seite 1 "Player": grosses Cover mit Angaben, daneben die Reiter
 *                     UP NEXT / LYRICS
 *
 * Unten quer ueber beides die Bedienleiste und die Statuszeile.
 *
 * WARUM KEIN ZWEITES FENSTER MEHR: die Albenliste stand frueher in einem
 * eigenen Fenster, das sich nach der Auswahl nicht zuverlaessig schloss
 * und hinter dem Hauptfenster liegen blieb. Mit MUIA_Group_PageMode
 * stellt sich die Frage nicht mehr - es gibt nur noch ein Fenster.
 *
 * Was hier ABSICHTLICH schlicht bleibt: die Netzabfragen laufen im Task
 * der Oberflaeche und blockieren sie kurz. Eine Anfrage an den
 * Navidrome-Server dauert gemessen rund 1,4 s (TLS-Handschlag), ein
 * Albumwechsel also etwa drei davon. Das ist spuerbar, aber es passiert
 * nur auf Knopfdruck - anders als bei AmiHomeassist, wo im Sekundentakt
 * abgefragt wurde und ein toter Server das ganze Programm 76 s einfror.
 * Der Sekundentakt hier fragt nur AmigaAMP ueber ARexx, und das ist
 * oertlich und schnell.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <clib/alib_protos.h>
#include <devices/timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amisub.h"
#include "audio.h"
#include "ring.h"
#include "cover.h"
#include "panel.h"
#include "tracklist.h"
#include "albumgrid.h"
#include "sidebar.h"
#include "tabs.h"
#include "visual.h"
#include "player.h"
#include "netjob.h"
#include "local.h"

/* MUIA_Dtpic_Name steht nicht in den MUI-3.8-Headern - die Klasse kam
 * erst mit MUI 4. Das hier installierte muimaster.library 19.35 bringt
 * sie trotzdem mit (geprueft: MUI_NewObject("Dtpic.mui", ...) liefert ein
 * Objekt), also wird der Wert aus der Autodoc uebernommen.
 *
 * ACHTUNG laut derselben Autodoc: Dtpic KOPIERT den Dateinamen NICHT und
 * merkt sich nur den Zeiger. Zum Bildwechsel muss ein ANDERER Zeiger
 * kommen - denselben Pfad noch einmal zu setzen bewirkt nichts. Fuer die
 * eigenen Klassen (panel, player) gilt das nicht, die kopieren den Pfad;
 * und seit die Cover im Cache unter ihrer ID liegen, hat ohnehin jedes
 * Album einen eigenen Dateinamen. */
#define MUIA_Dtpic_Name  0x80423d72

struct Library *MUIMasterBase = NULL;

/* ------------------------------------------------------------------ */
/* Zustand                                                             */
/* ------------------------------------------------------------------ */

static struct Prefs   g_prefs;
static struct SubList g_albums;         /* struct Album */
static struct SubList g_songs;          /* struct Song - die Warteschlange */
static int  g_playing = -1;             /* Index in g_songs, -1 = nichts */
static BOOL g_have_prefs = FALSE;

/* Der Pfad des zuletzt angezeigten Covers. Ein einziger Puffer reicht
 * jetzt: seit die Cover im Cache unter ihrer Cover-ID liegen, hat jedes
 * Album ohnehin einen eigenen Dateinamen. Die frueheren zwei
 * abwechselnden Puffer waren nur noetig, weil alle Cover dieselbe Datei
 * in T: ueberschrieben. */
static char g_coverpath[256];

/* Die Miniaturen der Bildwand, eine je Album, in derselben Reihenfolge
 * wie g_albums. Sie gehoeren HIER und nicht der Bildwand: sie entstehen
 * nach und nach, waehrend die Cover eintrudeln, und nur diese Datei
 * weiss, wann der Netzprozess gerade nichts anfasst.
 *
 * Genau darum geht es bei der Regel aus netjob.h: solange ein Auftrag
 * laeuft, wird hier weder malloc noch free gerufen. */
static struct AlbumThumb *g_thumbs = NULL;
static int   g_thumb_count = 0;
static BOOL  g_thumb_job   = FALSE; /* laeuft gerade ein Miniatur-Auftrag? */

/* Der Zustand des Fuellens.
 *
 * WARUM DAS SO UMSTAENDLICH IST - gemessen am 5.9.2026:
 *
 * Die erste Fassung holte die Cover so schnell nacheinander, wie die
 * Maschine konnte. Nach FUENF Abrufen in drei Sekunden war der Amiga
 * komplett vom Netz - kein Ping mehr, ueber sechs Minuten lang. Danach
 * kam er von selbst zurueck, ohne Neustart: das Programm lief weiter,
 * die Prozessliste war unveraendert. Dasselbe war vorher schon einmal
 * passiert, ebenfalls nach fuenf Abrufen kurz hintereinander.
 *
 * Zwei Lehren stecken darin, und beide stehen unten im Code:
 *
 *   1. Netzabrufe werden GEDROSSELT - hoechstens einer je Sekunde,
 *      angestossen vom Zeitgeber. Cover aus dem Cache brauchen kein
 *      Netz und laufen weiter mit voller Geschwindigkeit.
 *
 *   2. Bei einem Aussetzer wird NICHT weiter durch die Liste gerannt.
 *      Genau das ist beim ersten Versuch passiert: 44 uebrige Alben mal
 *      5 s Verbindungsgrenze - vier Minuten Fehlversuche, danach war die
 *      Wand fertig und blieb luecken haft, ohne dass je etwas erneut
 *      versucht wurde. Jetzt bricht der Durchgang nach drei
 *      Fehlschlaegen in Folge ab und beginnt nach einer Pause von vorn.
 */
#define FILL_PASSES   3     /* so viele Anlaeufe, dann ist Schluss */
#define FILL_HOLD    60     /* Sekunden Pause nach einem Aussetzer */
#define FILL_MAXFAIL  3     /* Fehlschlaege in Folge = Netz ist weg */

static int  g_fill_idx   = -1;      /* Stelle im Durchgang, -1 = keiner */
static int  g_fill_pass  = 0;
static int  g_fill_hold  = 0;       /* Restpause in Sekunden */
static int  g_fill_fails = 0;       /* Fehlschlaege in Folge */
static BOOL g_fill_gap   = FALSE;   /* in diesem Durchgang blieb etwas offen */
static BOOL g_fill_token = FALSE;   /* Erlaubnis fuer EINE Netzanfrage */

/* ZWEI ANSICHTEN AUF DIE BILDWAND.
 *
 * Home zeigt die meistgespielten Alben, Albums die ganze Bibliothek.
 * Beide bleiben im Speicher, damit das Umschalten sofort dasteht: die
 * Miniaturen neu aufzubauen kostet gemessene 0,2 s je Cover, bei 51
 * Alben also gut zehn Sekunden Bilderaufbau bei jedem Wechsel.
 *
 * Die SICHTBARE Ansicht liegt in den Arbeitsvariablen oben (g_albums,
 * g_thumbs, g_fill_...), die andere hier auf Halde. Beim Umschalten
 * werden sie getauscht. Das ist mit Absicht so herum: der ganze
 * Cover-Nachschub arbeitet unveraendert auf den Arbeitsvariablen
 * weiter, und der Tausch ist eine Stelle statt fuenfzig. */
#define VIEW_HOME    0
#define VIEW_ALBUMS  1
#define VIEW_COUNT   2

struct AlbumSet {
    struct SubList     list;
    struct AlbumThumb *thumbs;
    int   count;
    int   fill_idx, fill_pass, fill_hold, fill_fails;
    BOOL  fill_gap;
    BOOL  loaded;               /* schon einmal vom Server geholt */
};

static struct AlbumSet g_sets[VIEW_COUNT];
static int  g_set_cur  = VIEW_HOME;   /* welche Ansicht sichtbar ist */
static int  g_want_view = -1;         /* Wechsel, der auf das Netz wartet */

/* Zu welcher Ansicht der laufende Cover-Auftrag gehoert. Ohne das
 * landete ein Cover, das waehrend eines Wechsels unterwegs war, im
 * falschen Miniaturfeld. */
static int  g_thumb_job_view = -1;

/* Laeuft gerade eine Albenlisten-Abfrage? Siehe albums_request(). */
static BOOL g_list_job = FALSE;

/* DIE ANSICHT "TRACKS".
 *
 * Alle Titel der Bibliothek, nach Titelnamen von A bis Z. Der Server
 * kennt keine Liste "alle Titel" - search3 mit LEERER Suchanfrage
 * liefert sie aber, und songOffset blaettert darin.
 *
 * Geholt wird in Seiten zu 100. Die Groesse ist zweimal nachgemessen:
 * 500 auf einmal war dem Amiga zu viel - Abruf, Zerlegen der Antwort und
 * das Sortieren danach haengen alle an derselben Sekunde. 20 war zu
 * kleinteilig: 66 Abrufe, und nach jedem wird die ganze Liste neu
 * sortiert, zusammen rund 40 s. Mit 100 sind es 13 Abrufe.
 *
 * Nach jeder Seite wird neu sortiert - die sichtbaren Zeilen ruecken
 * dabei. Das ist der Preis fuer "sofort etwas sehen" und A-Z zugleich.
 *
 * Die kleinen Cover teilen sich viele Zeilen: ein Album hat ein Cover,
 * aber zwanzig Titel. Deshalb liegen die Miniaturen EINMAL in
 * g_cslots (nach Cover-ID), und die Liste bekommt je Zeile nur einen
 * ZEIGER darauf. So kann beim Freigeben nichts doppelt drankommen. */
#define TRACK_PAGE   100

static struct SubList        g_tracks;

/* Die eigenen Dateien von der Platte. Dieselbe Struktur wie beim Server
 * - ein Titel ist ein Titel, egal woher er kommt. Nur die Quelle steht
 * in Song.path statt in Song.id. */
static struct SubList        g_local;    /* eigene Titel */
static struct SubList        g_lalbums;  /* daraus gebildete Alben */
static struct AlbumThumb    *g_lthumbs = NULL;
static BOOL                  g_local_seen = FALSE;
static BOOL                  g_want_falbums = FALSE;
static int                   g_lfill_idx = -1;   /* naechste Miniatur */

/* Die kleinen Cover fuer Folder/Tracks: eines je ALBUM, und die Zeilen
 * zeigen nur darauf. Genau wie bei Tracks vom Server - ein Album hat ein
 * Cover, aber zwanzig Titel. */
static struct AlbumThumb    *g_lsmall = NULL;   /* je Album, 24 Punkte */
static struct AlbumThumb   **g_lrows  = NULL;   /* je Zeile ein Zeiger */
static struct AlbumThumb   **g_track_rows = NULL;  /* je Zeile ein Zeiger */
static int   g_track_rowcount = 0;
static int   g_track_offset   = 0;      /* naechste Seite */
static BOOL  g_track_more     = TRUE;   /* kommt noch etwas? */
static BOOL  g_track_job      = FALSE;  /* Seitenabruf laeuft */
static BOOL  g_track_seen     = FALSE;  /* Ansicht schon einmal geoeffnet */

struct CoverSlot {
    char  id[SUB_ID_LEN];
    ULONG hash;                 /* Streuwert der ID, siehe cslot_for() */
    struct AlbumThumb th;
};

/* EIN Symbol fuer alle Zeilen der Ansicht "Tracks".
 *
 * Der Grund ist eine Messung, die eine Annahme umgeworfen hat: es war
 * gedacht, dass sich alle Titel eines Albums ein Cover teilen. Navidrome
 * vergibt aber JE TITEL eine eigene Cover-ID ("mf-..."), und die Ansicht
 * holte daraufhin nicht 51 Cover, sondern 1306 - eines je Sekunde,
 * gemessen 39 MB im Cache. Fuer eine Liste, in der ohnehin das Album
 * danebensteht, ist das ein sehr hoher Preis fuer sehr kleine Bilder.
 *
 * Also ein festes Symbol aus PROGDIR:. Fehlt die Datei, bleibt der Platz
 * leer - das Programm laeuft trotzdem. */
static struct AlbumThumb g_track_icon = { NULL, 0, 0 };

static struct CoverSlot *g_cslots     = NULL;
static int   g_cslot_count = 0;
static int   g_cslot_max   = 0;
static int   g_cslot_fill  = 0;         /* naechster zu holender Slot */
static BOOL  g_cslot_job   = FALSE;     /* Cover-Auftrag fuer eine Zeile */

/* Die Ansicht "Favorites": die in Navidrome markierten Titel, in der
 * Reihenfolge des Servers. Eigene Liste, aber DIESELBEN Cover-Plaetze
 * wie Tracks - sie haengen an der Cover-ID, nicht an der Ansicht. */
static struct SubList      g_favs;
static struct AlbumThumb **g_fav_rows = NULL;
static int   g_fav_rowcount = 0;
static BOOL  g_fav_job      = FALSE;
static BOOL  g_fav_seen     = FALSE;

/* Die Ansicht "Radio". Eine Station ist kein Titel: die Adresse zeigt
 * irgendwohin ins Netz, und es gibt keine Dauer. Sie laeuft deshalb
 * NICHT ueber die Warteschlange - siehe radio_play(). */
static struct SubList g_radios;
static BOOL  g_radio_job  = FALSE;
static BOOL  g_radio_seen = FALSE;

/* Die Sendercover fuer die Liste. EIGENE Miniaturen, index-gleich zu
 * g_radios - NICHT in g_cslots. Dort teilen sich Tracks und Favorites
 * die Plaetze, und das Feld waechst per realloc: jeder neue Platz kann
 * es umziehen lassen, und die Zeigerreihen der anderen Listen zeigten
 * danach ins Leere (das "gruene Rauschen", AGENTS.md 5a). Acht Sender
 * sind die Verwaltung nicht wert, die das vermeiden wuerde.
 *
 * Angelegt wird in radios_fill(), also wenn kein Netzauftrag laeuft -
 * sonst waere malloc verboten (netjob.h). */
static struct AlbumThumb  *g_rthumbs     = NULL;
static struct AlbumThumb **g_radio_rows  = NULL;
static int   g_rthumb_count = 0;
static int   g_rfill        = 0;        /* naechster zu holender Sender */
static BOOL  g_rcover_job   = FALSE;    /* Cover-Auftrag fuer einen Sender */

static const char *view_type(int v)
{
    /* "frequent" ist bei Subsonic die Liste nach Abspielhaeufigkeit. */
    return (v == VIEW_HOME) ? "frequent" : "alphabeticalByName";
}

static const char *view_title(int v)
{
    return (v == VIEW_HOME) ? "Most Played" : "Albums";
}

static int view_size(int v)
{
    return (v == VIEW_HOME) ? 10 : 200;
}

/* WAS GERADE LAEUFT - und zwar unabhaengig davon, was man sich ansieht.
 *
 * Die Bedienleiste unten zeigt den laufenden Titel. Vorher zeigte sie den
 * zuletzt ANGEWAEHLTEN: bei jedem Albumwechsel sprangen Titel, Interpret
 * und Miniatur auf das neue Album, obwohl weiter das alte lief.
 *
 * Deshalb wird der laufende Titel hier als KOPIE gehalten, nicht als
 * Index in g_songs - die Liste wird beim naechsten Album ausgetauscht,
 * ein Index zeigte danach auf einen fremden Titel. Der Cover-Pfad kommt
 * mit, damit die Miniatur bleibt, auch wenn der Cache laengst ein
 * anderes Bild anzeigt. */
static struct Song g_now;
static BOOL        g_now_ok = FALSE;
static char        g_now_cover[256];
static char        g_now_album[SUB_ID_LEN];   /* Album, aus dem er stammt */
static char        g_cur_album[SUB_ID_LEN];   /* Album, das gerade zu sehen ist */

/* DIE WARTESCHLANGE - die Titel, die gerade abgespielt werden.
 *
 * Sie ist eine KOPIE der Liste, aus der gestartet wurde, in einem festen
 * Feld. Zwei Gruende, und beide sind wichtig:
 *
 *   - Die sichtbare Liste g_songs wird beim naechsten Album ausgetauscht.
 *     Wer aus ihr weiterspielt, springt beim Blaettern in ein fremdes
 *     Album - vor, zurueck und das automatische Weiterschalten waeren
 *     davon abhaengig, was man sich gerade ansieht.
 *   - Ein FESTES Feld statt einer wachsenden Liste, weil das Kopieren
 *     sonst malloc braeuchte. Die Oberflaeche darf nichts anlegen,
 *     solange ein Netzauftrag laeuft (siehe netjob.h), und gestartet
 *     wird auch mal waehrend die Bildwand im Hintergrund Cover holt.
 *
 * 200 Titel reichen: das laengste Album hier hat 144. */
#define QUEUE_MAX 200
static struct Song g_queue[QUEUE_MAX];
static int  g_qcount = 0;
static int  g_qpos   = -1;
static char g_queue_cover[256];
static char g_queue_album[SUB_ID_LEN];

/* Der Strom, aus dem gerade gespielt wird.
 *
 * Fruehere Fassungen schoben die ganze Warteschlange in AmigaAMPs
 * Playlist und ueberliessen ihm das Weiterschalten. Seit der eigene
 * Abspieler da ist, laeuft immer GENAU EIN Titel - weitergeschaltet wird
 * hier, wenn der Audioprozess meldet, dass er durch ist. */
/* ZWEI Ringe im Wechsel. Beim Titelwechsel bekommt der Arbeiter den
 * anderen; er darf seine angefangene Scheibe in Ruhe zu Ende schreiben,
 * ohne dass jemand darauf wartet oder in einen Puffer schreibt, den der
 * Abspieler gerade liest. 2 x 1 MB sind auf dieser Maschine nichts. */
static struct Ring    g_rings[2];
static int            g_ring_cur = 0;
static BOOL           g_ring_ok = FALSE;
/* ZWEI Auftragsstrukturen im Wechsel. Eine Exec-Nachricht, die noch
 * nicht beantwortet ist, haengt in der Warteschlange des Empfaengers -
 * wer sie in diesem Zustand neu befuellt und noch einmal verschickt,
 * zerstoert die Verkettung. Beim Springen kommen zwei Auftraege
 * unmittelbar hintereinander, genau dann passiert das. */
static struct NetJob  g_sjobs[2];
static int            g_sjob_cur = 0;
static long           g_stream_bytes = 0;   /* Groesse der laufenden Datei */
static BOOL           g_said_outage = FALSE; /* "Netz weg" steht gerade da */
static LONG           g_seek_base_ms = 0;   /* Stelle, an der der Ring anfaengt */

/* DER LAUFENDE SENDER, falls gerade Radio laeuft.
 *
 * Adresse und Name als KOPIE, aus demselben Grund wie g_now: der Scan-
 * Knopf oder ein Serverwechsel wirft g_radios weg, waehrend der Sender
 * weiterspielt, und ein Weiterhoeren nach Pause braucht die Adresse
 * dann immer noch. Die ID dient nur dazu, den Sender in einer frisch
 * geholten Liste wiederzufinden. */
static BOOL g_radio_on = FALSE;
static char g_radio_url[512];
static char g_radio_id[SUB_ID_LEN];
static char g_radio_name[SUB_NAME_LEN];
static char g_radio_home[256];
static ULONG g_icy_seen = 0;        /* zuletzt angezeigte ICY-Meldung */

/* Mitschrift der Wiedergabe - siehe tick(). */
static BOOL g_expect_play = FALSE;
static LONG g_last_pos    = -1;

/* Ein Albumklick, der warten musste, weil gerade eine Miniatur unterwegs
 * war. Ohne das bekaeme der Anwender "still busy with another request" zu
 * sehen, nur weil im Hintergrund Cover nachgeladen werden. */
static int   g_want_album = -1;
static char  g_want_album_id[SUB_ID_LEN];   /* dasselbe, per Kennung */
static int   g_want_lalbum = -1;            /* dasselbe, eigenes Album */

static char g_lyrics[16384];

/* Kein "Verlauf neu zeichnen"-Merker mehr noetig: die Flaeche ist ein
 * einziges Objekt, das sich bei jeder Aenderung selbst neu zeichnet. */

/* Der gerade laufende Netzauftrag. Es ist immer hoechstens einer
 * unterwegs - siehe die Regel im Kopf von netjob.h. */
static struct NetJob g_job;
static struct Album  g_pending_album;   /* Kopfzeile des geholten Albums */
static char          g_pending_cover[SUB_ID_LEN];

/* Zeitgeber fuer die Laufzeitanzeige. */
static struct MsgPort     *g_tport = NULL;
static struct timerequest *g_treq  = NULL;
static ULONG               g_tsig  = 0;
static BOOL                g_twait = FALSE;

static Object *app, *win;
static Object *lst_tracks;      /* die Ansicht "Tracks" */
static Object *lst_favs;        /* die Ansicht "Favorites" */
static Object *lst_radio;       /* die Ansicht "Radio" */
static Object *lst_folder;      /* Folder/Tracks: eigene Dateien */
static Object *grid_folder;     /* Folder/Albums: eigene Alben */

/* Das Einstellfenster. Als einziges Fenster dieses Programms besteht es
 * aus MUI-BORDMITTELN - Stringfelder, Ankreuzfeld, Knoepfe. Das ist
 * Absicht: Einstellungen sollen aussehen wie ueberall auf dem System,
 * und der Aufwand einer eigenen Zeichnerei waere hier verschwendet. */
static Object *prefswin;
static Object *str_host, *str_user, *str_pass;
static Object *chk_screen, *txt_mode;
static Object *cyc_ahi;
static Object *str_folder, *btn_folder;

/* Fuer das Zykelfeld. MUSS ein Feld von Zeigern sein, das bis zum Ende
 * des Programms lebt - MUI kopiert die Liste nicht. */
static const char *g_ahi_units[] = { "0", "1", "2", "3", NULL };
static Object *cyc_vis;
static const char *g_vis_rates[] = { "Off", "15 fps", "20 fps", "30 fps",
                                     "60 fps", NULL };
static const int   g_vis_fps[]   = { 0, 15, 20, 30, 60 };
static Object *btn_mode, *btn_save, *btn_cancel;
static Object *panel;

/* MUI merkt sich bei MUIA_Background nur den ZEIGER auf die
 * Spezifikation, es kopiert sie nicht. Die Zeichenketten muessen also
 * leben, solange das Objekt lebt - deshalb feste Puffer und keine
 * Ortsvariablen. */
/* Der aktuelle Farbton fuer die Registerseiten. Gemerkt, weil er beim
 * Umschalten auf eine bis dahin unsichtbare Seite noch einmal gesetzt
 * werden muss. */
static ULONG g_tint = 0x00281640;

/* Die Verlaufsbild-Bastelei ist ersatzlos entfallen: die Listen
 * zeichnen ihren Verlauf jetzt selbst (tracklist.c). Damit fallen auch
 * die BMP-Dateien in T:, das Nachfaerben beim Umschalten und die Frage
 * weg, ob MUI ein Hintergrundbild kachelt oder neu laedt. */
static Object *lst_queue, *ft_lyrics;
static Object *player;
static Object *grid, *sidebar, *pages, *txt_status;
static Object *tabbar, *tabpages;
static Object *visual;           /* dritter Reiter: VISUALIZER */

/* Die beiden Seiten der Gruppe mit MUIA_Group_PageMode. */
#define PAGE_HOME    0
#define PAGE_PLAYER  1
#define PAGE_TRACKS  2
#define PAGE_FAVS    3
#define PAGE_RADIO   4
#define PAGE_FOLDER  5          /* eigene Titel, Liste */
#define PAGE_FALBUMS 6          /* eigene Alben, Bildwand */

#define ID_QUIT        1
#define ID_SIDEBAR     2
#define ID_ALBUM_PICK  3
#define ID_SONG_PICK   4
#define ID_TRACK_PICK  5
#define ID_FAV_PICK    6
#define ID_RADIO_PICK  7
#define ID_SET_SAVE    8
#define ID_SET_CANCEL 11
#define ID_SET_MODE   12
#define ID_SET_AMP    13
#define ID_SET_FOLDER 14
#define ID_FALBUM_PICK 15
#define ID_FOLDER_PICK 16
#define ID_TAB         9
#define ID_PLAYER     10

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

/* Die Statuszeile blendet jede Meldung nach 3 s wieder aus - der
 * Anwender wollte keine Fehlermeldung, die minutenlang stehen bleibt
 * ("lrclib.net does not know this track" zum laengst vergangenen
 * Titel). Gerechnet wird in DateStamp-Ticks statt in Sekundentakten:
 * der erste Takt kann sofort nach der Meldung kommen, ein Zaehler
 * "noch 3 Takte" haette sie also schon nach gut 2 s geloescht.
 * Geprueft wird in tick(), also auf eine Sekunde genau: 3 bis 4 s.
 *
 * Laufende Meldungen ("network down - retrying for 12 s", "Cover
 * 7/51") werden je Sekunde neu gesetzt und bleiben damit stehen,
 * solange der Zustand andauert. */
#define SAY_TICKS (3 * TICKS_PER_SECOND)
static LONG g_say_at  = 0;          /* Zeitpunkt der Meldung, in Ticks */
static BOOL g_say_on  = FALSE;

static LONG say_now(void)
{
    struct DateStamp ds;

    /* Nur innerhalb des Tages: mit ds_Days liefe ein LONG ueber (gut
     * 17000 Tage seit 1978 mal 4,32 Mio. Ticks). Den Sprung um
     * Mitternacht faengt say_expire() ab. */
    DateStamp(&ds);
    return ds.ds_Minute * (60L * TICKS_PER_SECOND) + ds.ds_Tick;
}

static void say(const char *s)
{
    set(txt_status, MUIA_Text_Contents, (char *)s);
    g_say_at = say_now();
    g_say_on = (s && s[0]);
}

/* Aus tick(): abgelaufene Meldung wegnehmen. */
static void say_expire(void)
{
    LONG d;

    if (!g_say_on) {
        return;
    }
    d = say_now() - g_say_at;
    if (d < 0) {
        d += 24L * 60L * 60L * TICKS_PER_SECOND;    /* ueber Mitternacht */
    }
    if (d >= SAY_TICKS) {
        set(txt_status, MUIA_Text_Contents, (ULONG)"");
        g_say_on = FALSE;
    }
}



/* Der Helfer fuer MUI-Knoepfe ist ersatzlos entfallen: mit dem
 * Zurueck-Knopf ist der letzte verschwunden. Die ganze Oberflaeche
 * besteht jetzt aus eigenen, selbst gezeichneten Klassen - kein
 * MUI-Bordmittel zeichnet mehr mit. */

/* ------------------------------------------------------------------ */
/* Zeitgeber                                                           */
/* ------------------------------------------------------------------ */

static BOOL timer_open(void)
{
    g_tport = CreateMsgPort();
    if (!g_tport) {
        return FALSE;
    }
    g_treq = (struct timerequest *)
                 CreateIORequest(g_tport, sizeof(struct timerequest));
    if (!g_treq) {
        DeleteMsgPort(g_tport);
        g_tport = NULL;
        return FALSE;
    }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)g_treq, 0) != 0) {
        DeleteIORequest((struct IORequest *)g_treq);
        DeleteMsgPort(g_tport);
        g_treq = NULL;
        g_tport = NULL;
        return FALSE;
    }
    g_tsig = 1UL << g_tport->mp_SigBit;
    return TRUE;
}

static void timer_start(void)
{
    if (!g_treq || g_twait) {
        return;
    }
    g_treq->tr_node.io_Command = TR_ADDREQUEST;
    g_treq->tr_time.tv_secs    = 1;
    g_treq->tr_time.tv_micro   = 0;
    SendIO((struct IORequest *)g_treq);
    g_twait = TRUE;
}

static void timer_close(void)
{
    if (g_treq && g_twait) {
        AbortIO((struct IORequest *)g_treq);
        WaitIO((struct IORequest *)g_treq);
        g_twait = FALSE;
    }
    if (g_treq) {
        CloseDevice((struct IORequest *)g_treq);
        DeleteIORequest((struct IORequest *)g_treq);
        g_treq = NULL;
    }
    if (g_tport) {
        DeleteMsgPort(g_tport);
        g_tport = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Cover und Farbe                                                     */
/* ------------------------------------------------------------------ */

/* Das Cover ist geholt - anzeigen und die Verlaufsfarbe daraus ziehen.
 *
 * Laeuft in der Oberflaeche, nicht im Arbeitsprozess: cover_dominant()
 * geht nicht ins Netz, sondern nur ueber die datatypes, und die wollen
 * einen Bildschirm (PDTA_Screen). */
/* Alle drei Listen auf denselben Farbton bringen. Sie zeichnen den
 * Verlauf selbst, es ist also ein einziges Setzen je Liste - kein
 * Erzeugen von Bildern, kein Nachfaerben beim Umschalten. */
static void tint_all(void)
{
    set(lst_queue, MUIA_TL_Colour, g_tint);
    set(ft_lyrics, MUIA_TL_Colour, g_tint);
    set(grid,      MUIA_AG_Colour, g_tint);
    set(tabbar,    MUIA_Tb_Colour, g_tint);
    set(visual,    MUIA_Vis_Colour, g_tint);
}

/* Zwischen Bildwand und Playeransicht umschalten.
 *
 * Der Farbton wird danach noch einmal gesetzt: eine Seite, die bis eben
 * unsichtbar war, hat ihn beim letzten Coverwechsel zwar bekommen, aber
 * MUI zeichnet unsichtbare Objekte nicht - dieselbe Falle wie frueher
 * beim Register, wo die zweite Lasche noch im Startviolett stand,
 * waehrend die Coverseite laengst braun war. */
/* Der zuletzt in der Seitenleiste gewaehlte Punkt der Bibliothek. Die
 * Markierung folgt der ANSICHT: kommt man aus dem Player zurueck, steht
 * sie wieder dort, wo man vorher war. */
static LONG g_sb_home = SB_HOME;

/* Welche Seite gerade zu sehen ist. Der Cover-Nachschub der Titelliste
 * fragt danach - er soll nur laufen, wenn man sie auch sieht. */
static int  g_page = PAGE_HOME;

static void rows_repoint(int page);

static void show_page(int page)
{
    g_page = page;

    /* VOR dem Umschalten, denn das zeichnet sofort. Tracks und
     * Favorites teilen sich g_cslots; hat die jeweils andere Liste
     * seitdem Plaetze angelegt, kann das Feld per realloc umgezogen
     * sein, und die Zeigerreihe dieser Seite zeigte in freigegebenen
     * Speicher. */
    rows_repoint(page);

    set(pages, MUIA_Group_ActivePage, page);

    /* Die Seitenleiste zeigt, was zu sehen ist. Fuer die Playeransicht
     * gibt es keinen Eintrag, also ist keiner markiert. */
    set(sidebar, MUIA_Sb_Active,
        (page == PAGE_PLAYER) ? SB_NONE : g_sb_home);

    tint_all();
}

static void cover_ready(const char *path)
{
    ULONG rgb = 0;

    /* Erst die Farbe, dann das Bild: beides loest ein Neuzeichnen aus,
     * und so ist es genau eines statt zwei. */
    if (cover_dominant(path, &rgb)) {
        ULONG top = cover_gradient_top(rgb);

        set(panel, MUIA_Panel_Colour, top);

        /* Die Registerseite bekommt dieselbe Farbe, nur halb so hell -
         * so gehoert sie sichtbar zur Coverseite, ohne mit ihr um
         * Aufmerksamkeit zu streiten.
         *
         * Gefaerbt wird der INHALT, nicht das Register selbst: setzt man
         * den Hintergrund am Register, wird auch die aktive Lasche dunkel
         * und ihre Beschriftung unlesbar. */
        g_tint = cover_scale(top, 50);
        tint_all();
    }
    /* NUR die Anzeigeflaeche. Die Bedienleiste behaelt das Cover des
     * laufenden Titels - sie bekommt ihres in now_show(). */
    set(panel, MUIA_Panel_Cover, (ULONG)path);
}

/* ------------------------------------------------------------------ */
/* Liedtext                                                            */
/* ------------------------------------------------------------------ */

/* Wirft die Zeitmarken "[00:12.34] " aus einer synchronen Fassung
 * heraus. Die Marken selbst werden erst gebraucht, wenn die Zeile zur
 * Abspielposition hervorgehoben werden soll - dann liest sie diese
 * Funktion nicht mehr weg, sondern gibt sie weiter. */
static void strip_timestamps(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '[' && r[1] >= '0' && r[1] <= '9') {
            char *close = strchr(r, ']');
            if (close) {
                r = close + 1;
                while (*r == ' ') {
                    r++;
                }
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void lyrics_request(struct Song *s)
{
    if (!s || net_busy()) {
        return;
    }

    g_lyrics[0] = '\0';
    DoMethod(ft_lyrics, MUIM_TL_SetText, (ULONG)g_lyrics);

    memset(&g_job, 0, sizeof(g_job));
    g_job.op = NJ_LYRICS;
    strncpy(g_job.id,     s->id,     sizeof(g_job.id) - 1);
    strncpy(g_job.artist, s->artist, sizeof(g_job.artist) - 1);
    strncpy(g_job.title,  s->title,  sizeof(g_job.title) - 1);
    strncpy(g_job.album,  s->album,  sizeof(g_job.album) - 1);
    g_job.duration     = s->duration;
    g_job.out_text     = g_lyrics;
    g_job.out_textsize = sizeof(g_lyrics);

    net_submit(&g_job);
}

/* ------------------------------------------------------------------ */
/* Abspielen                                                           */
/* ------------------------------------------------------------------ */

/* Die grosse Anzeigeflaeche - sie zeigt, was man sich ANSIEHT. */
static void show_song(int idx)
{
    struct Song *s = (struct Song *)list_get(&g_songs, idx);
    char meta[64];
    char total[16];

    if (!s) {
        return;
    }

    set(panel, MUIA_Panel_Title,  (ULONG)s->title);
    set(panel, MUIA_Panel_Artist, (ULONG)s->artist);
    set(panel, MUIA_Panel_Album,  (ULONG)s->album);

    duration_text(s->duration, total, sizeof(total));
    if (s->year > 0) {
        sprintf(meta, "%s  \xb7  %d  \xb7  %s",
                s->suffix[0] ? s->suffix : "?", s->year, total);
    } else {
        sprintf(meta, "%s  \xb7  %s", s->suffix[0] ? s->suffix : "?", total);
    }
    set(panel, MUIA_Panel_Meta, (ULONG)meta);
}

/* Die Bedienleiste unten - sie zeigt, was LAEUFT. Gespeist wird sie
 * ausschliesslich aus g_now, nie aus der gerade sichtbaren Liste. */
static void now_show(void)
{
    if (!g_now_ok) {
        return;
    }
    set(player, MUIA_Pl_Title,  (ULONG)g_now.title);
    set(player, MUIA_Pl_Artist, (ULONG)g_now.artist);
    set(player, MUIA_Pl_Album,  (ULONG)g_now.album);
    set(player, MUIA_Pl_Total,  g_now.duration);
    set(player, MUIA_Pl_Pos,    0);
    /* AUCH wenn der Pfad leer ist: sonst bliebe das Cover des vorigen
     * Titels stehen. Gemeldet vom Anwender am 21.9.2026 fuer Titel aus
     * Tracks - dort fand sich kein Cover, und die Leiste zeigte weiter
     * das alte. */
    set(player, MUIA_Pl_Cover, (ULONG)g_now_cover);
}

/* Die sichtbare Liste zur Warteschlange machen. Strukturkopien in ein
 * festes Feld - kein malloc, siehe die Begruendung oben bei g_queue. */
static void queue_from_songs(void)
{
    int i, n = g_songs.count;

    if (n > QUEUE_MAX) {
        n = QUEUE_MAX;
    }
    for (i = 0; i < n; i++) {
        struct Song *s = (struct Song *)list_get(&g_songs, i);

        if (s) {
            g_queue[i] = *s;
        }
    }
    g_qcount = n;

    /* Alle Titel einer Warteschlange stammen aus demselben Album, also
     * gilt ein Cover fuer alle - beim Weiterschalten muss es nicht neu
     * gesucht werden. */
    strncpy(g_queue_cover, g_coverpath, sizeof(g_queue_cover) - 1);
    g_queue_cover[sizeof(g_queue_cover) - 1] = '\0';
    strncpy(g_queue_album, g_cur_album, sizeof(g_queue_album) - 1);
    g_queue_album[sizeof(g_queue_album) - 1] = '\0';

    /* Was in AmigaAMPs Playlist steht, gehoert zur ALTEN Warteschlange
     * und ist damit hinfaellig. play_queue() baut sie neu auf. */

}

/* Aus der Titelliste heraus starten: ab dem angeklickten Titel wird die
 * Warteschlange gefuellt, hoechstens QUEUE_MAX Stueck. Anders als bei
 * einem Album kommen die Titel hier aus VERSCHIEDENEN Alben - das Cover
 * gilt also nicht mehr fuer die ganze Schlange, es wird je Titel aus dem
 * Cache geholt (siehe now_from_queue). */
static void queue_from_list(struct SubList *src, int start)
{
    int i, n = src->count - start;

    if (start < 0 || start >= src->count) {
        return;
    }
    if (n > QUEUE_MAX) {
        n = QUEUE_MAX;
    }
    for (i = 0; i < n; i++) {
        struct Song *sg = (struct Song *)list_get(src, start + i);

        if (sg) {
            g_queue[i] = *sg;
        }
    }
    g_qcount = n;

    /* Kein gemeinsames Cover und kein gemeinsames Album. */
    g_queue_cover[0] = '\0';
    g_queue_album[0] = '\0';


}

/* Den laufenden Titel in der SICHTBAREN Liste markieren - oder die
 * Markierung wegnehmen, wenn man sich gerade ein anderes Album ansieht.
 * Gesucht wird ueber die ID, nicht ueber den Index: die sichtbare Liste
 * ist frisch vom Server und muss nicht dieselbe Reihenfolge haben. */
static void mark_playing(void)
{
    int i;

    g_playing = -1;
    if (!g_now_ok) {
        set(lst_queue, MUIA_TL_Playing, -1);
        return;
    }
    for (i = 0; i < g_songs.count; i++) {
        struct Song *s = (struct Song *)list_get(&g_songs, i);

        /* Eigene Dateien haben keine Kennung, ihre id ist leer - ein
         * Vergleich der ids traefe dann immer die erste Zeile. Dort
         * zaehlt der Pfad. */
        if (!s) {
            continue;
        }
        if (g_now.path[0] ? strcmp(s->path, g_now.path) == 0
                          : strcmp(s->id, g_now.id) == 0) {
            g_playing = i;
            break;
        }
    }
    set(lst_queue, MUIA_TL_Playing, g_playing);
}

/* Die Anzeige auf den Titel qidx der Warteschlange umstellen. Ruehrt
 * AmigaAMP NICHT an - sie wird von zwei Seiten gerufen: nach einem
 * eigenen Startbefehl, und wenn der Sekundentakt merkt, dass AmigaAMP
 * von sich aus weitergeschaltet hat. */
static void now_from_queue(int qidx)
{
    g_qpos   = qidx;
    g_now    = g_queue[qidx];
    g_now_ok = TRUE;

    /* Das Cover kommt zuerst aus dem Cache, ueber die Cover-ID des
     * Titels - nur so stimmt es auch bei einer Warteschlange aus
     * verschiedenen Alben (Ansicht "Tracks"). Liegt es nicht im Cache,
     * gilt das Cover des Albums, aus dem gestartet wurde. */
    /* Zuerst das ALBUMcover. Navidrome vergibt je Titel eine eigene
     * Kennung ("mf-..."), der Cache haelt aber eine Datei je Album
     * ("al-<albumid>", AGENTS.md 5a) - ueber g_now.coverart fand sich
     * deshalb bei Titeln aus Tracks nie etwas. */
    g_now_cover[0] = '\0';
    {
        char cov[SUB_ID_LEN];

        sub_album_cover(&g_now, cov, sizeof(cov));
        if (cov[0] && cache_have(&g_prefs, cov)) {
            cache_path(&g_prefs, cov, g_now_cover, sizeof(g_now_cover));
        }
    }
    if (!g_now_cover[0] && g_now.coverart[0]
            && cache_have(&g_prefs, g_now.coverart)) {
        cache_path(&g_prefs, g_now.coverart,
                   g_now_cover, sizeof(g_now_cover));
    }
    if (!g_now_cover[0]) {
        strncpy(g_now_cover, g_queue_cover, sizeof(g_now_cover) - 1);
        g_now_cover[sizeof(g_now_cover) - 1] = '\0';
    }
    strncpy(g_now_album, g_queue_album, sizeof(g_now_album) - 1);
    g_now_album[sizeof(g_now_album) - 1] = '\0';

    now_show();
    mark_playing();
    if (g_playing >= 0) {
        set(lst_queue, MUIA_TL_Active, g_playing);
        show_song(g_playing);
    }

    /* Fuer die Mitschrift im Sekundentakt neu anfangen. -1 heisst: der
     * naechste Takt zaehlt noch keinen Uebergang mit, sonst saehe der
     * Ruecksprung auf 0 wie ein Titelwechsel aus. */
    g_expect_play = TRUE;
    g_last_pos    = -1;

    /* Der Liedtext wird NACH dem Startbefehl angefordert. Andersherum
     * stuende die Musik still, waehrend die Netzabfrage laeuft. */
    lyrics_request(&g_now);
}

/* Ein Cover ist gerade angekommen. Fehlte es dem laufenden Titel, kommt
 * es jetzt in die Bedienleiste - sonst bliebe sie bis zum naechsten
 * Titel leer, obwohl das Bild laengst da ist. */
static void now_cover_late(const char *id, const char *path)
{
    char cov[SUB_ID_LEN];

    if (!g_now_ok || g_radio_on || g_now_cover[0] || !id || !id[0]) {
        return;
    }
    sub_album_cover(&g_now, cov, sizeof(cov));
    if (strcmp(cov, id) != 0) {
        return;
    }
    strncpy(g_now_cover, path, sizeof(g_now_cover) - 1);
    g_now_cover[sizeof(g_now_cover) - 1] = '\0';
    set(player, MUIA_Pl_Cover, (ULONG)g_now_cover);
}

/* Den Strom fuer einen Titel anfangen lassen.
 *
 * start_ms > 0 heisst: mitten im Titel einsteigen. Die Stelle wird in
 * ein Byte umgerechnet - der Server nimmt Range an (gemessen 20.9.2026,
 * HTTP 206). Bei wechselnder Bitrate ist das eine Schaetzung; genauer
 * ginge es nur, indem man die Rahmen zaehlt, und dafuer muesste man den
 * Titel erst einmal ganz holen. */
static int stream_start(int qidx, LONG start_ms)
{
    long off = 0;

    if (!g_ring_ok) {
        say("no audio buffer - is the audio process running?");
        return 0;
    }

    audio_halt();                   /* was noch laeuft, beenden */
    net_stream_end();

    /* Stelle in Bytes, ganzzahlig gerechnet: erst durch die Dauer, dann
     * mal die Sekunden - andersherum liefe ein LONG bei einer 10-MB-
     * Datei schon nach wenigen Minuten ueber. */
    if (start_ms > 0 && g_queue[qidx].duration > 0) {
        if (g_queue[qidx].path[0] && g_queue[qidx].bitrate > 0) {
            /* Eigene Datei mit bekannter Bitrate: die Stelle laesst sich
             * unmittelbar ausrechnen, kbit/s mal 125 sind Bytes je
             * Sekunde. Genauer als die Schaetzung ueber die Dateigroesse,
             * und ohne sie zu kennen. */
            off = (long)g_queue[qidx].bitrate * 125L * (start_ms / 1000);
        } else if (g_stream_bytes > 0) {
            off = (g_stream_bytes / g_queue[qidx].duration)
                * (start_ms / 1000);
        }
    }

    g_ring_cur ^= 1;                /* der andere Ring ist frei */
    ring_reset(&g_rings[g_ring_cur]);
    g_seek_base_ms = start_ms;

    net_stream_reap();              /* beantwortete Auftraege abraeumen */
    g_sjob_cur ^= 1;
    memset(&g_sjobs[g_sjob_cur], 0, sizeof(g_sjobs[0]));
    g_sjobs[g_sjob_cur].op = NJ_STREAM;
    strncpy(g_sjobs[g_sjob_cur].id, g_queue[qidx].id, SUB_ID_LEN - 1);
    /* Eigene Datei: der Pfad entscheidet, der Rest bleibt gleich. */
    strncpy(g_sjobs[g_sjob_cur].path, g_queue[qidx].path,
            sizeof(g_sjobs[0].path) - 1);
    g_sjobs[g_sjob_cur].offset = (int)off;
    g_sjobs[g_sjob_cur].out_ring = &g_rings[g_ring_cur];
    if (!net_submit_stream(&g_sjobs[g_sjob_cur])) {
        say("cannot start stream");
        return 0;
    }

    audio_play(&g_rings[g_ring_cur], start_ms);
    return 1;
}

/* Einen Titel der Warteschlange abspielen. Der einzige Weg, auf dem in
 * diesem Programm Musik anfaengt. */
static void play_queue(int qidx)
{
    if (qidx < 0 || qidx >= g_qcount) {
        return;
    }
    if (!stream_start(qidx, 0)) {
        return;
    }
    g_radio_on = FALSE;             /* ab jetzt wieder Warteschlange */
    set(lst_radio, MUIA_TL_Playing, -1);
    now_from_queue(qidx);
}

/* Aus der sichtbaren Liste heraus starten: sie wird zur Warteschlange. */
static void play_index(int idx)
{
    queue_from_songs();
    play_queue(idx);
}

/* ------------------------------------------------------------------ */
/* Miniaturen fuer die Bildwand                                        */
/* ------------------------------------------------------------------ */

/* Dasselbe fuer die Miniaturen der eigenen Alben. Eine eigene Funktion,
 * weil die andere an den Zaehlern der Serverbildwand haengt. */
static void lthumbs_free(struct AlbumThumb *t, int count)
{
    int i;

    if (!t) {
        return;
    }
    for (i = 0; i < count; i++) {
        if (t[i].rgb) {
            free(t[i].rgb);
        }
    }
    free(t);
}

/* Alle Miniaturen freigeben. Erst ABHAENGEN, dann freigeben - die
 * Bildwand zeigt auf diese Puffer und kopiert sie nicht. Andersherum
 * zeichnete sie in freigegebenen Speicher. */
static void thumbs_free(void)
{
    int i;

    DoMethod(grid, MUIM_AG_SetList, (ULONG)NULL, (ULONG)NULL);

    if (g_thumbs) {
        for (i = 0; i < g_thumb_count; i++) {
            if (g_thumbs[i].rgb) {
                free(g_thumbs[i].rgb);
            }
        }
        free(g_thumbs);
        g_thumbs = NULL;
    }
    g_thumb_count = 0;
    g_thumb_job   = FALSE;

    g_fill_idx   = -1;
    g_fill_pass  = 0;
    g_fill_hold  = 0;
    g_fill_fails = 0;
    g_fill_gap   = FALSE;
}

/* Die sichtbare Ansicht auf die Halde legen. Es wird NICHTS freigegeben
 * und nichts kopiert - nur die Zeiger wandern. */
static void view_save(int v)
{
    struct AlbumSet *set = &g_sets[v];

    set->list       = g_albums;
    set->thumbs     = g_thumbs;
    set->count      = g_thumb_count;
    set->fill_idx   = g_fill_idx;
    set->fill_pass  = g_fill_pass;
    set->fill_hold  = g_fill_hold;
    set->fill_fails = g_fill_fails;
    set->fill_gap   = g_fill_gap;
}

/* Eine Ansicht von der Halde in die Arbeitsvariablen holen und die Wand
 * darauf zeigen lassen. */
static void view_load(int v)
{
    struct AlbumSet *set = &g_sets[v];

    g_albums      = set->list;
    g_thumbs      = set->thumbs;
    g_thumb_count = set->count;
    g_fill_idx    = set->fill_idx;
    g_fill_pass   = set->fill_pass;
    g_fill_hold   = set->fill_hold;
    g_fill_fails  = set->fill_fails;
    g_fill_gap    = set->fill_gap;

    g_set_cur = v;

    set(grid, MUIA_AG_Title, (ULONG)view_title(v));
    DoMethod(grid, MUIM_AG_SetList,
             (ULONG)(g_thumbs ? &g_albums : NULL), (ULONG)g_thumbs);
}

/* Einen Durchgang von vorn beginnen. */
static void fill_start(void)
{
    g_fill_idx   = (g_thumb_count > 0) ? 0 : -1;
    g_fill_fails = 0;
    g_fill_gap   = FALSE;
}

/* Der Durchgang ist durch. Blieb etwas offen, gibt es nach einer Pause
 * einen weiteren Anlauf - Cover, die schon liegen, ueberspringt er
 * ueber cache_have() im Vorbeigehen. */
static void fill_end(void)
{
    char msg[64];

    g_fill_idx = -1;

    if (g_fill_gap && g_fill_pass + 1 < FILL_PASSES) {
        /* Nur die Pause setzen - hochgezaehlt wird der Durchgang an
         * EINER Stelle, naemlich im Sekundentakt, wenn die Pause
         * abgelaufen ist. Zweimal zaehlen hiesse, Anlaeufe zu
         * verschenken. */
        g_fill_hold = FILL_HOLD;
        sprintf(msg, "%d albums - covers incomplete, retrying",
                g_albums.count);
    } else {
        sprintf(msg, "%d albums", g_albums.count);
    }
    say(msg);
}

/* Eine Cover-Datei zur Miniatur machen.
 *
 * Laeuft in der Oberflaeche, nicht im Arbeitsprozess: die datatypes
 * wollen einen Bildschirm (PDTA_Screen), und der Arbeiter hat keinen.
 * Gemessen kostet das rund 0,2 s je Cover - deshalb kommt pro
 * Schleifendurchlauf genau eine dran und nicht alle auf einmal. */
static BOOL thumb_load(int idx, const char *path)
{
    struct CoverImage img;
    UBYTE *rgb = NULL;
    LONG w = 0, h = 0;

    if (idx < 0 || idx >= g_thumb_count || !g_thumbs) {
        return FALSE;
    }
    if (!cover_load(path, &img)) {
        return FALSE;
    }
    if (cover_scale_rgb(&img, AG_THUMB_SIZE, FALSE, &rgb, &w, &h)) {
        if (g_thumbs[idx].rgb) {
            free(g_thumbs[idx].rgb);
        }
        g_thumbs[idx].rgb = rgb;
        g_thumbs[idx].w   = w;
        g_thumbs[idx].h   = h;
    }
    cover_unload(&img);
    return rgb != NULL;
}

/* Ein Schritt beim Fuellen der Bildwand. TRUE heisst "es gab etwas zu
 * tun" - dann laesst die Hauptschleife das Wait() aus und kommt sofort
 * wieder her, statt auf ein Ereignis zu warten.
 *
 * Zwei Wege, je nachdem ob das Cover schon im Cache liegt:
 *   - liegt es da: sofort laden, kein Netz, ein Bild je Durchlauf
 *   - fehlt es:    hoechstens einmal je Sekunde einen Auftrag
 *                  abschicken; weiter geht es in job_done()
 */
static BOOL thumbs_step(void)
{
    struct Album *a;
    char path[256];
    char cov[SUB_ID_LEN];

    if (g_fill_idx < 0) {
        return FALSE;
    }
    if (g_fill_idx >= g_thumb_count) {
        fill_end();
        return FALSE;
    }
    /* Der Anwender hat Vorrang: wartet ein Albumklick, wird jetzt keine
     * neue Miniatur angefangen. */
    if (g_want_album >= 0 || net_busy() || g_fill_hold > 0) {
        return FALSE;
    }

    a = (struct Album *)list_get(&g_albums, g_fill_idx);
    if (!a) {
        fill_end();
        return FALSE;
    }

    if (!a->coverart[0] || g_thumbs[g_fill_idx].rgb) {
        g_fill_idx++;
        return TRUE;
    }

    /* NICHT a->coverart, sondern die Kennung ohne Hash: dieselbe Datei
     * bedient dann auch die Titelliste. a->coverart bleibt trotzdem die
     * Probe darauf, OB es ueberhaupt ein Cover gibt. */
    sub_album_cover_id(a->id, cov, sizeof(cov));

    if (cache_have(&g_prefs, cov)) {
        cache_path(&g_prefs, cov, path, sizeof(path));
        if (!thumb_load(g_fill_idx, path)) {
            g_fill_gap = TRUE;
        }
        DoMethod(grid, MUIM_AG_Refresh);
        g_fill_idx++;
        return TRUE;
    }

    /* Ab hier geht es ins Netz - und dafuer braucht es die Marke, die
     * der Sekundentakt ausgibt. Ohne diese Bremse faellt der Rechner vom
     * Netz, siehe die Messung oben. */
    if (!g_fill_token) {
        return FALSE;
    }
    g_fill_token = FALSE;

    memset(&g_job, 0, sizeof(g_job));
    g_job.op   = NJ_COVER;
    g_job.size = 300;
    strncpy(g_job.id, cov, sizeof(g_job.id) - 1);
    if (!net_submit(&g_job)) {
        return FALSE;
    }
    g_thumb_job      = TRUE;
    g_thumb_job_view = g_set_cur;

    {
        char msg[64];
        sprintf(msg, "Cover %d/%d ...", g_fill_idx + 1, g_thumb_count);
        say(msg);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Listen fuellen                                                      */
/* ------------------------------------------------------------------ */

/* Albenliste anfordern. Die Liste wird erst gefuellt, wenn die Antwort
 * da ist - siehe job_done(). */
static void albums_request(void)
{
    if (net_busy()) {
        say("still busy with another request");
        return;
    }

    say("loading albums ...");
    /* Die Bildwand zeigt auf g_albums und kopiert nichts - also erst
     * abhaengen, dann freigeben. Andersherum zeichnete sie in
     * freigegebenen Speicher. thumbs_free() haengt beides ab. */
    thumbs_free();
    list_free(&g_albums);
    list_init(&g_albums, sizeof(struct Album));

    memset(&g_job, 0, sizeof(g_job));
    g_job.op       = NJ_ALBUMS;
    g_job.size     = view_size(g_set_cur);
    strncpy(g_job.listtype, view_type(g_set_cur),
            sizeof(g_job.listtype) - 1);
    g_job.out_list = &g_albums;
    if (net_submit(&g_job)) {
        /* Solange diese Abfrage laeuft, darf die Ansicht NICHT
         * gewechselt werden: der Arbeiter schreibt in &g_albums, und
         * unter dieser Anschrift laege nach einem Wechsel die Liste der
         * anderen Ansicht. */
        g_list_job = TRUE;
    }
}

static void albums_fill(void)
{
    char msg[64];

    /* Ein Feld fuer die Miniaturen, so lang wie die Liste. calloc, damit
     * alle Zeiger NULL sind - die Bildwand zeichnet dafuer Platzhalter,
     * bis das jeweilige Cover da ist. */
    g_thumb_count = g_albums.count;
    g_thumbs = (struct AlbumThumb *)
                   calloc((size_t)(g_thumb_count > 0 ? g_thumb_count : 1),
                          sizeof(struct AlbumThumb));
    if (!g_thumbs) {
        g_thumb_count = 0;
    }

    g_sets[g_set_cur].loaded = TRUE;

    set(grid, MUIA_AG_Title, (ULONG)view_title(g_set_cur));
    DoMethod(grid, MUIM_AG_SetList, (ULONG)&g_albums, (ULONG)g_thumbs);

    /* Einmal anlegen, bevor die erste Miniatur geholt wird - danach
     * fragt sub_get_cover_cached() nicht mehr danach. */
    cache_ensure(&g_prefs);

    g_fill_pass = 0;
    fill_start();

    sprintf(msg, "%d albums - loading covers ...", g_albums.count);
    say(msg);
}

/* ------------------------------------------------------------------ */
/* Die Ansicht "Tracks"                                                */
/* ------------------------------------------------------------------ */

/* Streuwert einer Cover-ID. Die uebliche Formel von Bernstein - sie
 * braucht je Zeichen eine Verschiebung und eine Addition, mehr nicht. */
static ULONG cslot_hash(const char *s)
{
    ULONG h = 5381;

    while (*s) {
        h = (h << 5) + h + (ULONG)(UBYTE)*s++;
    }
    return h;
}

/* Den Platz fuer die Miniatur eines Covers finden oder anlegen. Gesucht
 * wird ueber die Cover-ID, damit sich alle Titel eines Albums denselben
 * Platz teilen.
 *
 * Die Suche ist linear - bei rund 50 Alben ist jede Verwaltung drumherum
 * teurer als das Durchgehen. Verglichen werden aber ZUERST die
 * Streuwerte: diese Funktion laeuft je Titel und je Nachladung, bei 1311
 * Titeln und 66 Nachladungen also millionenfach, und ein Vergleich
 * zweier Langworte ist auf dem 68020 ein Bruchteil eines strcmp ueber
 * zwei 30-Zeichen-Kennungen. Der strcmp bleibt als Gegenprobe stehen,
 * damit zwei verschiedene IDs mit gleichem Streuwert nicht verwechselt
 * werden. */
static struct CoverSlot *cslot_for(const char *coverart)
{
    ULONG h;
    int i;

    if (!coverart || !coverart[0]) {
        return NULL;
    }
    h = cslot_hash(coverart);
    for (i = 0; i < g_cslot_count; i++) {
        if (g_cslots[i].hash == h
                && strcmp(g_cslots[i].id, coverart) == 0) {
            return &g_cslots[i];
        }
    }

    if (g_cslot_count >= g_cslot_max) {
        int nmax = g_cslot_max ? g_cslot_max * 2 : 64;
        struct CoverSlot *nn = (struct CoverSlot *)
            realloc(g_cslots, (size_t)nmax * sizeof(struct CoverSlot));

        if (!nn) {
            return NULL;
        }
        g_cslots   = nn;
        g_cslot_max = nmax;
    }

    memset(&g_cslots[g_cslot_count], 0, sizeof(struct CoverSlot));
    strncpy(g_cslots[g_cslot_count].id, coverart, SUB_ID_LEN - 1);
    g_cslots[g_cslot_count].hash = h;
    return &g_cslots[g_cslot_count++];
}

/* Vergleich fuer die Sortierung: Titel A-Z, ohne Ruecksicht auf
 * Gross- und Kleinschreibung. */
static int song_cmp(const void *a, const void *b)
{
    const struct Song *x = (const struct Song *)a;
    const struct Song *y = (const struct Song *)b;

    return stricmp(x->title, y->title);
}

/* Innerhalb eines Albums gilt die Titelnummer, nicht das Alphabet. */
static int track_cmp(const void *a, const void *b)
{
    const struct Song *x = (const struct Song *)a;
    const struct Song *y = (const struct Song *)b;

    if (x->track != y->track) {
        return x->track - y->track;
    }
    return stricmp(x->title, y->title);
}

/* Die Zeigerreihe fuer die Zeilen einer Liste neu aufbauen. Muss nach
 * jeder Sortierung passieren - die Zeilen haben sich ja verschoben - und
 * nach jedem Nachschub, weil das Cover-Feld dabei umgezogen sein kann.
 * Dieselbe Rechnung dient Tracks und Favorites. */
static BOOL rows_rebuild(struct SubList *list, struct AlbumThumb ***rows,
                         int *rowcount, BOOL album)
{
    int i;

    if (list->count > *rowcount) {
        struct AlbumThumb **nn = (struct AlbumThumb **)
            realloc(*rows,
                    (size_t)list->count * sizeof(struct AlbumThumb *));

        if (!nn) {
            return FALSE;
        }
        *rows     = nn;
        *rowcount = list->count;
    }

    /* ZWEI DURCHGAENGE, und der Grund ist ein Fehler, der genau einmal
     * gemacht wurde: g_cslots waechst per realloc, und dabei ZIEHT DAS
     * GANZE FELD UM. Wer im selben Durchgang erst einen Platz anlegt und
     * dann seine Anschrift wegschreibt, hat nach dem naechsten Umzug
     * lauter Zeiger ins Leere - auf dem Schirm sah das aus wie gruenes
     * Rauschen ueber dem ganzen Fenster.
     *
     * Also erst ALLE Plaetze anlegen, und danach die Anschriften holen:
     * dann steht das Feld fest, solange die Zeiger gebraucht werden. */
    /* album == TRUE: die Kennung des ALBUMS statt der des Titels. Damit
     * werden aus 1311 Bildern rund 50, und sie liegen alle schon im
     * Cache - das Alben-Raster hat sie geholt. */
    for (i = 0; i < list->count; i++) {
        struct Song *sg = (struct Song *)list_get(list, i);
        char cov[SUB_ID_LEN];

        if (!sg) {
            continue;
        }
        if (album) {
            sub_album_cover(sg, cov, sizeof(cov));
            cslot_for(cov);
        } else {
            cslot_for(sg->coverart);
        }
    }
    for (i = 0; i < list->count; i++) {
        struct Song *sg = (struct Song *)list_get(list, i);
        struct CoverSlot *cs = NULL;
        char cov[SUB_ID_LEN];

        if (sg) {
            if (album) {
                sub_album_cover(sg, cov, sizeof(cov));
                cs = cslot_for(cov);
            } else {
                cs = cslot_for(sg->coverart);
            }
        }

        /* Solange das Cover noch nicht da ist, steht das feste Symbol
         * da - sonst waere die Spalte beim Aufbau leer. */
        if (cs && cs->th.rgb) {
            (*rows)[i] = &cs->th;
        } else if (g_track_icon.rgb) {
            (*rows)[i] = &g_track_icon;
        } else {
            (*rows)[i] = cs ? &cs->th : NULL;
        }
    }
    return TRUE;
}

/* Eine Seite anfordern. */
static void tracks_request(void)
{
    if (g_track_job || !g_have_prefs || net_busy()) {
        return;
    }

    memset(&g_job, 0, sizeof(g_job));
    g_job.op       = NJ_TRACKS;
    g_job.size     = TRACK_PAGE;
    g_job.offset   = g_track_offset;
    g_job.out_list = &g_tracks;
    if (net_submit(&g_job)) {
        g_track_job = TRUE;
        if (g_track_offset == 0) {
            say("loading tracks ...");
        }
    }
}

/* Eine Seite ist da: sortieren, Zeilen neu verzeigern, anzeigen. */
static void tracks_fill(void)
{
    char msg[64];
    int  got = g_tracks.count - g_track_offset;

    g_track_job = FALSE;

    /* Weniger als eine volle Seite heisst: das war der Rest. */
    g_track_more   = (got >= TRACK_PAGE);
    g_track_offset = g_tracks.count;

    if (g_tracks.count > 0 && g_tracks.items) {
        qsort(g_tracks.items, (size_t)g_tracks.count,
              (size_t)g_tracks.itemsize, song_cmp);
    }
    rows_rebuild(&g_tracks, &g_track_rows, &g_track_rowcount, TRUE);

    DoMethod(lst_tracks, MUIM_TL_SetList,   (ULONG)&g_tracks);
    DoMethod(lst_tracks, MUIM_TL_SetThumbs, (ULONG)g_track_rows);

    sprintf(msg, g_track_more ? "%d tracks - more to come"
                              : "%d tracks", g_tracks.count);
    say(msg);
}

/* Welche der beiden Titellisten gerade zu sehen ist. Der Cover-Nachschub
 * arbeitet fuer beide - die Plaetze haengen ja an der Cover-ID, nicht an
 * der Ansicht - und muss nur die sichtbare neu zeichnen lassen. */
static Object *visible_list(void)
{
    return (g_page == PAGE_FAVS) ? lst_favs : lst_tracks;
}


/* Das feste Symbol fuer die Titelliste laden. Mehrere Endungen, damit
 * es egal ist, womit das Bild gezeichnet wurde - picture.datatype liest
 * ohnehin alles, wofuer ein Datatype da ist. */
static BOOL track_icon_try(const char *path)
{
    struct CoverImage img;
    UBYTE *rgb = NULL;
    LONG w = 0, h = 0;

    if (!cover_load(path, &img)) {
        return FALSE;
    }
    /* grow = TRUE: ein kleiner gezeichnetes Symbol wird auf das
     * Zeilenmass hochgezogen, statt verloren klein dazustehen. */
    if (cover_scale_rgb(&img, TL_THUMB_SIZE, TRUE, &rgb, &w, &h)) {
        g_track_icon.rgb = rgb;
        g_track_icon.w   = w;
        g_track_icon.h   = h;
    }
    cover_unload(&img);
    return rgb != NULL;
}

/* Die Endungen einzeln abgefragt statt ueber ein Feld von Zeigern: so
 * ein Feld landet bei gcc im Codebereich, und objdump zerlegt es dann
 * als Befehle - make check-fpu meldete daraufhin eine FPU-Instruktion,
 * die gar keine war (dieselbe Falle wie bei den Sprungtabellen, siehe
 * checkfpu.py). */
static void track_icon_load(void)
{
    if (track_icon_try("PROGDIR:track.png")) {
        return;
    }
    if (track_icon_try("PROGDIR:track.iff")) {
        return;
    }
    track_icon_try("PROGDIR:track.jpg");
}

/* Ein Cover laden und auf Zeilenmass verkleinern - fuer die Titel-
 * listen wie fuer die Radioliste. */
static BOOL thumb_small_load(struct AlbumThumb *th, const char *path)
{
    struct CoverImage img;
    UBYTE *rgb = NULL;
    LONG w = 0, h = 0;

    if (!th || !cover_load(path, &img)) {
        return FALSE;
    }
    if (cover_scale_rgb(&img, TL_THUMB_SIZE, FALSE, &rgb, &w, &h)) {
        if (th->rgb) {
            free(th->rgb);
        }
        th->rgb = rgb;
        th->w   = w;
        th->h   = h;
    }
    cover_unload(&img);
    return rgb != NULL;
}

static BOOL cslot_load(struct CoverSlot *cs, const char *path)
{
    return cs ? thumb_small_load(&cs->th, path) : FALSE;
}

/* Ein frisch geladenes Cover in die Zeilen einhaengen und neu zeichnen.
 * Noetig, weil eine Zeile ohne Bild vorlaeufig auf das Platzhalter-
 * symbol zeigt - der Zeiger muss also umgesetzt werden, nicht nur die
 * Flaeche aufgefrischt. */
static void rows_refresh(void)
{
    if (g_page == PAGE_FAVS) {
        rows_rebuild(&g_favs, &g_fav_rows, &g_fav_rowcount, FALSE);
        DoMethod(lst_favs, MUIM_TL_SetThumbs, (ULONG)g_fav_rows);
    } else {
        rows_rebuild(&g_tracks, &g_track_rows, &g_track_rowcount, TRUE);
        DoMethod(lst_tracks, MUIM_TL_SetThumbs, (ULONG)g_track_rows);
    }
    MUI_Redraw(visible_list(), MADF_DRAWOBJECT);
}

/* Die Zeigerreihe einer Titelliste frisch verzeigern, ohne zu zeichnen.
 * Legt KEINE neuen Plaetze an und belegt nichts: die Liste hat sich seit
 * ihrem letzten Aufbau nicht veraendert, also findet cslot_for() jede
 * Kennung schon vor, und die Reihe ist bereits lang genug. Damit ist
 * der Aufruf auch waehrend eines Netzauftrags erlaubt. */
static void rows_repoint(int page)
{
    if (page == PAGE_FAVS && g_fav_rows) {
        rows_rebuild(&g_favs, &g_fav_rows, &g_fav_rowcount, FALSE);
        DoMethod(lst_favs, MUIM_TL_SetThumbs, (ULONG)g_fav_rows);
    } else if (page == PAGE_TRACKS && g_track_rows) {
        rows_rebuild(&g_tracks, &g_track_rows, &g_track_rowcount, TRUE);
        DoMethod(lst_tracks, MUIM_TL_SetThumbs, (ULONG)g_track_rows);
    }
}

/* Ein Schritt beim Fuellen der kleinen Cover - dieselbe Mechanik wie bei
 * der Bildwand, nur je ALBUM statt je Zeile. Laeuft nur, solange die
 * Titelliste zu sehen ist: sonst nimmt sie der Bildwand das Netz weg. */
static BOOL track_covers_step(void)
{
    struct CoverSlot *cs;
    char path[256];

    if ((g_page != PAGE_TRACKS && g_page != PAGE_FAVS)
            || g_cslot_fill >= g_cslot_count) {
        return FALSE;
    }
    if (g_want_album >= 0 || net_busy()) {
        return FALSE;
    }

    cs = &g_cslots[g_cslot_fill];
    if (!cs->id[0] || cs->th.rgb) {
        g_cslot_fill++;
        return TRUE;
    }

    if (cache_have(&g_prefs, cs->id)) {
        cache_path(&g_prefs, cs->id, path, sizeof(path));
        cslot_load(cs, path);
        rows_refresh();
        g_cslot_fill++;
        return TRUE;
    }

    /* Ab hier ins Netz - und dafuer braucht es die Marke aus dem
     * Sekundentakt, dieselbe Bremse wie bei der Bildwand. */
    if (!g_fill_token) {
        return FALSE;
    }
    g_fill_token = FALSE;

    memset(&g_job, 0, sizeof(g_job));
    g_job.op   = NJ_COVER;
    g_job.size = 300;
    strncpy(g_job.id, cs->id, sizeof(g_job.id) - 1);
    if (!net_submit(&g_job)) {
        return FALSE;
    }
    g_cslot_job = TRUE;
    return TRUE;
}

/* Die Favoriten anfordern. Kein Blaettern: getStarred2 liefert alles
 * auf einmal, und eine Favoritenliste ist um Groessenordnungen kuerzer
 * als die Bibliothek. */
static void favs_request(void)
{
    if (g_fav_job || !g_have_prefs || net_busy()) {
        return;
    }

    list_free(&g_favs);
    list_init(&g_favs, sizeof(struct Song));

    memset(&g_job, 0, sizeof(g_job));
    g_job.op       = NJ_FAVORITES;
    g_job.out_list = &g_favs;
    if (net_submit(&g_job)) {
        g_fav_job = TRUE;
        say("loading favorites ...");
    }
}

static void favs_fill(void)
{
    char msg[64];

    g_fav_job = FALSE;

    /* NICHT sortiert: der Server liefert sie in seiner eigenen Ordnung,
     * ueblicherweise zuletzt markiert zuerst - genau die Reihenfolge,
     * die man aus der Weboberflaeche kennt. */
    rows_rebuild(&g_favs, &g_fav_rows, &g_fav_rowcount, FALSE);

    DoMethod(lst_favs, MUIM_TL_SetList,   (ULONG)&g_favs);
    DoMethod(lst_favs, MUIM_TL_SetThumbs, (ULONG)g_fav_rows);

    if (g_favs.count > 0) {
        sprintf(msg, "%d favorites", g_favs.count);
    } else {
        strcpy(msg, "no favorites - star tracks in Navidrome");
    }
    say(msg);
}

static void favs_show(void)
{
    show_page(PAGE_FAVS);

    if (!g_fav_seen) {
        g_fav_seen = TRUE;
        favs_request();
    }
}

/* Die Cover-Kennung eines Senders. Wie beim Album OHNE den Hash, den
 * der Server in coverArt mitschickt ("ra-<id>_6a268ca8"): gemessen am
 * 21.9.2026 liefert "ra-<id>" dasselbe Bild, Byte fuer Byte gleich
 * gross, und eine erfundene Kennung wird abgelehnt statt mit einem
 * Ersatzbild beantwortet. Ohne Hash passt die Kennung auch in
 * SUB_ID_LEN: "ra-" und 36 Zeichen sind genau 39. */
static void radio_cover_id(const char *radioid, char *out)
{
    sprintf(out, "ra-%.36s", radioid);
}

static void radio_thumbs_free(void)
{
    int i;

    for (i = 0; i < g_rthumb_count; i++) {
        if (g_rthumbs[i].rgb) {
            free(g_rthumbs[i].rgb);
        }
    }
    if (g_rthumbs) {
        free(g_rthumbs);
    }
    if (g_radio_rows) {
        free(g_radio_rows);
    }
    g_rthumbs      = NULL;
    g_radio_rows   = NULL;
    g_rthumb_count = 0;
    g_rfill        = 0;
    g_rcover_job   = FALSE;
}

/* Zu jedem Sender ein leerer Platz, die Zeile zeigt darauf. Solange ein
 * Platz leer ist, zeichnet die Liste die Antenne. */
static void radio_thumbs_build(void)
{
    int i, n = g_radios.count;

    radio_thumbs_free();
    if (n <= 0) {
        return;
    }
    g_rthumbs = (struct AlbumThumb *)calloc((size_t)n,
                                            sizeof(struct AlbumThumb));
    g_radio_rows = (struct AlbumThumb **)calloc((size_t)n,
                                                sizeof(struct AlbumThumb *));
    if (!g_rthumbs || !g_radio_rows) {
        radio_thumbs_free();
        return;
    }
    for (i = 0; i < n; i++) {
        g_radio_rows[i] = &g_rthumbs[i];
    }
    g_rthumb_count = n;
}

/* Cover und Farbe des LAUFENDEN Senders in Bedienleiste und Radioliste.
 * Liegt das Cover (noch) nicht im Cache, wird die Miniatur geleert -
 * sonst stuende dort weiter das Cover des zuletzt gehoerten Albums. Es
 * kommt nach, sobald der Nachschub der Liste es geholt hat. */
static void radio_cover_show(void)
{
    char cid[SUB_ID_LEN + 8];
    ULONG rgb = 0;

    radio_cover_id(g_radio_id, cid);
    g_now_cover[0] = '\0';
    if (cache_have(&g_prefs, cid)) {
        cache_path(&g_prefs, cid, g_now_cover, sizeof(g_now_cover));
    }
    set(player, MUIA_Pl_Cover, (ULONG)g_now_cover);

    /* Der Farbton der Radioliste folgt dem laufenden Sender - genauso
     * gerechnet wie bei einem Album in cover_ready(). */
    if (g_now_cover[0] && cover_dominant(g_now_cover, &rgb)) {
        set(lst_radio, MUIA_TL_Colour,
            cover_scale(cover_gradient_top(rgb), 50));
    }
}

/* Ein Schritt beim Holen der Sendercover. Dieselbe Mechanik wie bei den
 * Titellisten: aus dem Cache sofort, aus dem Netz hoechstens eines je
 * Sekunde (g_fill_token). Nur, solange die Radioliste zu sehen ist. */
static BOOL radio_covers_step(void)
{
    struct Radio *r;
    char cid[SUB_ID_LEN + 8];
    char path[256];

    if (g_page != PAGE_RADIO || g_rcover_job
            || g_rfill >= g_rthumb_count || net_busy()) {
        return FALSE;
    }
    r = (struct Radio *)list_get(&g_radios, g_rfill);
    if (!r || g_rthumbs[g_rfill].rgb) {
        g_rfill++;
        return TRUE;
    }

    radio_cover_id(r->id, cid);
    if (cache_have(&g_prefs, cid)) {
        cache_path(&g_prefs, cid, path, sizeof(path));
        thumb_small_load(&g_rthumbs[g_rfill], path);
        MUI_Redraw(lst_radio, MADF_DRAWOBJECT);
        g_rfill++;
        return TRUE;
    }

    if (!g_fill_token) {
        return FALSE;
    }
    g_fill_token = FALSE;

    memset(&g_job, 0, sizeof(g_job));
    g_job.op   = NJ_COVER;
    g_job.size = 300;
    strncpy(g_job.id, cid, sizeof(g_job.id) - 1);
    if (!net_submit(&g_job)) {
        return FALSE;
    }
    g_rcover_job = TRUE;
    return TRUE;
}

/* Den laufenden Titel eines Senders in der Bedienleiste zeigen.
 *
 * Die Sender schicken "Interpret - Titel" (gemessen 21.9.2026, vier
 * Sender, ausnahmslos so). Daraus werden die ersten beiden Zeilen, in
 * der dritten steht der Sender. Ohne " - " steht der ganze Text oben -
 * so kommt auch der erste Block an, der nur den Programmnamen traegt
 * ("80s80s Digital Web"). Leer heisst: noch nichts gemeldet, dann wie
 * vorher Sendername, "Radio" und Homepage. */
static void radio_show_title(const char *t)
{
    const char *sep = t ? strstr(t, " - ") : NULL;

    memset(g_now.title, 0, sizeof(g_now.title));
    memset(g_now.artist, 0, sizeof(g_now.artist));
    memset(g_now.album, 0, sizeof(g_now.album));

    if (!t || !t[0]) {
        strncpy(g_now.title, g_radio_name, sizeof(g_now.title) - 1);
        strcpy(g_now.artist, "Radio");
        strncpy(g_now.album, g_radio_home, sizeof(g_now.album) - 1);
    } else if (sep) {
        int an = (int)(sep - t);

        if (an >= (int)sizeof(g_now.artist)) {
            an = sizeof(g_now.artist) - 1;
        }
        memcpy(g_now.artist, t, an);
        strncpy(g_now.title, sep + 3, sizeof(g_now.title) - 1);
        strncpy(g_now.album, g_radio_name, sizeof(g_now.album) - 1);
    } else {
        strncpy(g_now.title, t, sizeof(g_now.title) - 1);
        strncpy(g_now.artist, g_radio_name, sizeof(g_now.artist) - 1);
    }

    set(player, MUIA_Pl_Title,  (ULONG)g_now.title);
    set(player, MUIA_Pl_Artist, (ULONG)g_now.artist);
    set(player, MUIA_Pl_Album,  (ULONG)g_now.album);
}

/* Den Strom des laufenden Senders (neu) anfangen lassen - beim Start
 * UND nach einer Pause. Eine Pause heisst beim Radio Anhalten: an Ort
 * und Stelle weiterzuhoeren ginge nur mit einem Ring, der waehrend der
 * Pause weiter vollaeuft, und bei 192 kbps waere 1 MB nach gut 40 s
 * voll. Also wird neu verbunden, und es geht live weiter.
 *
 * Dieselbe Reihenfolge wie in stream_start(): erst anhalten, dann den
 * ANDEREN Ring und die ANDERE Auftragsstruktur nehmen. */
static int radio_start(void)
{
    struct NetJob *j;

    if (!g_ring_ok) {
        say("no audio buffer - is the audio process running?");
        return 0;
    }

    audio_halt();
    net_stream_end();

    g_ring_cur ^= 1;
    ring_reset(&g_rings[g_ring_cur]);
    g_seek_base_ms = 0;

    net_stream_reap();
    g_sjob_cur ^= 1;
    j = &g_sjobs[g_sjob_cur];
    memset(j, 0, sizeof(*j));
    j->op = NJ_STREAM;
    strncpy(j->url, g_radio_url, sizeof(j->url) - 1);
    j->out_ring = &g_rings[g_ring_cur];
    if (!net_submit_stream(j)) {
        say("cannot start stream");
        return 0;
    }

    audio_play(&g_rings[g_ring_cur], 0);
    return 1;
}

/* Einen Sender als nicht abspielbar markieren, sobald der Content-Type
 * es gezeigt hat (weder MP3 noch AAC). Gesucht wird ueber die ID - die
 * Liste kann seit dem Klick neu geholt worden sein. */
static void radio_mark_unplayable(void)
{
    int i;

    for (i = 0; i < g_radios.count; i++) {
        struct Radio *r = (struct Radio *)list_get(&g_radios, i);

        if (r && strcmp(r->id, g_radio_id) == 0) {
            r->unplayable = TRUE;
            MUI_Redraw(lst_radio, MADF_DRAWOBJECT);
            break;
        }
    }
}

/* Eine Radiostation abspielen.
 *
 * Sie geht NICHT durch die Warteschlange: die Adresse zeigt irgendwohin
 * ins Netz statt auf den eigenen Server, es gibt keine Dauer und nichts,
 * worauf weitergeschaltet werden koennte. Die Warteschlange bleibt
 * dabei stehen, wie sie war - ein Druck auf VOR oder ZURUECK kehrt zu
 * ihr zurueck. */
static void radio_play(int idx)
{
    struct Radio *r = (struct Radio *)list_get(&g_radios, idx);

    if (!r || !r->url[0]) {
        say("this station has no address");
        return;
    }
    /* Schon einmal am Content-Type gescheitert. Gar nicht erst
     * verbinden - die Zeile steht ohnehin grau da. */
    if (r->unplayable) {
        say("this station's format is not supported");
        return;
    }

    strncpy(g_radio_url, r->url, sizeof(g_radio_url) - 1);
    g_radio_url[sizeof(g_radio_url) - 1] = '\0';
    strncpy(g_radio_id, r->id, sizeof(g_radio_id) - 1);
    g_radio_id[sizeof(g_radio_id) - 1] = '\0';
    strncpy(g_radio_name, r->name, sizeof(g_radio_name) - 1);
    g_radio_name[sizeof(g_radio_name) - 1] = '\0';
    strncpy(g_radio_home, r->home, sizeof(g_radio_home) - 1);
    g_radio_home[sizeof(g_radio_home) - 1] = '\0';

    if (!radio_start()) {
        return;
    }
    g_radio_on = TRUE;

    /* Fuer die Anzeige wird die Station als Titel ausgegeben. Dauer 0:
     * ein Sender hat kein Ende, der Balken zeigt nur die Laufzeit, und
     * die Bedienleiste laesst ohne Dauer keinen Sprung zu. */
    memset(&g_now, 0, sizeof(g_now));
    strncpy(g_now.title, r->name, sizeof(g_now.title) - 1);
    strcpy(g_now.artist, "Radio");
    strncpy(g_now.album, r->home, sizeof(g_now.album) - 1);
    g_now_ok       = TRUE;
    g_now_cover[0] = '\0';
    g_now_album[0] = '\0';

    now_show();
    radio_cover_show();
    mark_playing();
    set(lst_radio, MUIA_TL_Playing, idx);

    g_expect_play = TRUE;
    g_last_pos    = -1;

    say(r->name);
}

/* ------------------------------------------------------------------ */
/* Eigener Bildschirm                                                  */
/* ------------------------------------------------------------------ */

static struct Screen *g_screen  = NULL;
static const char    *g_pubname = NULL;

/* Den eigenen Bildschirm oeffnen, wenn er eingestellt ist.
 *
 * Er wird OEFFENTLICH gemacht (PubScreenStatus), und die Fenster gehen
 * ueber MUIA_Window_PublicScreen darauf. Der Umweg ueber den Namen statt
 * ueber den Zeiger hat einen praktischen Grund: ohne eigenen Bildschirm
 * ist der Name NULL, und das heisst bei MUI schlicht "der oeffentliche
 * Standardbildschirm". So braucht die Fensterbeschreibung keine
 * Fallunterscheidung. */
static void screen_open(void)
{
    ULONG pens[] = { ~0UL };    /* Farbwahl wie die Workbench */

    if (!g_prefs.ownscreen) {
        return;
    }

    if (g_prefs.screenid != 0 && g_prefs.screenw > 0) {
        g_screen = OpenScreenTags(NULL,
            SA_DisplayID, g_prefs.screenid,
            SA_Width,     (ULONG)g_prefs.screenw,
            SA_Height,    (ULONG)g_prefs.screenh,
            SA_Depth,     (ULONG)g_prefs.screend,
            SA_Title,     (ULONG)"AmiSubsonic",
            SA_Pens,      (ULONG)pens,
            SA_SysFont,   1,
            SA_Type,      PUBLICSCREEN,
            SA_PubName,   (ULONG)"AMISUBSONIC",
            TAG_DONE);
    }
    if (!g_screen) {
        /* Kein Modus gewaehlt, oder der gewaehlte ging nicht auf. Dann
         * einer wie die Workbench - besser als gar kein Programm. */
        g_screen = OpenScreenTags(NULL,
            SA_LikeWorkbench, TRUE,
            SA_Title,         (ULONG)"AmiSubsonic",
            SA_Pens,          (ULONG)pens,
            SA_Type,          PUBLICSCREEN,
            SA_PubName,       (ULONG)"AMISUBSONIC",
            TAG_DONE);
    }

    if (g_screen) {
        PubScreenStatus(g_screen, 0);
        g_pubname = "AMISUBSONIC";
    }
}

static void screen_close(void)
{
    if (g_screen) {
        CloseScreen(g_screen);
        g_screen = NULL;
    }
    g_pubname = NULL;
}

/* ------------------------------------------------------------------ */
/* Einstellungen                                                       */
/* ------------------------------------------------------------------ */

/* Die Beschriftung des Modus-Knopfes auffrischen. */
static void mode_text(void)
{
    static char buf[64];

    if (g_prefs.screenid == 0) {
        strcpy(buf, "same as Workbench");
    } else {
        sprintf(buf, "%ldx%ld, %ld bitplanes",
                (LONG)g_prefs.screenw, (LONG)g_prefs.screenh,
                (LONG)g_prefs.screend);
    }
    set(txt_mode, MUIA_Text_Contents, buf);
}

/* Den Bildschirmmodus waehlen lassen. asl.library wird nur fuer diesen
 * einen Knopf gebraucht und deshalb erst hier geoeffnet und gleich
 * wieder zugemacht - im Normalbetrieb soll sie nicht im Speicher
 * haengen. */
static void mode_request(void)
{
    struct ScreenModeRequester *req;

    /* MUIs eigene ASL-Huelle statt asl.library von Hand: sie oeffnet die
     * Bibliothek selbst und legt die Anwendung waehrenddessen schlafen -
     * ohne das liefe der Sekundentakt hinter dem Requester weiter. */
    req = (struct ScreenModeRequester *)
              MUI_AllocAslRequest(ASL_ScreenModeRequest, NULL);
    if (!req) {
        say("screen mode requester not available");
        return;
    }

    if (MUI_AslRequestTags(req,
            ASLSM_TitleText,        (ULONG)"Screen mode for AmiSubsonic",
            ASLSM_InitialDisplayID, g_prefs.screenid,
            ASLSM_DoWidth,          TRUE,
            ASLSM_DoHeight,         TRUE,
            ASLSM_DoDepth,          TRUE,
            TAG_DONE)) {
        g_prefs.screenid = req->sm_DisplayID;
        g_prefs.screenw  = req->sm_DisplayWidth;
        g_prefs.screenh  = req->sm_DisplayHeight;
        g_prefs.screend  = req->sm_DisplayDepth;
        mode_text();
    }
    MUI_FreeAslRequest(req);
}


/* Das Verzeichnis mit den eigenen MP3-Dateien aussuchen lassen.
 *
 * Ein VERZEICHNIS-Requester, kein Dateirequester: ASLFR_DrawersOnly.
 * Geraten wird nichts - wo die eigene Sammlung liegt, weiss nur der
 * Anwender, und ein falsch geratener Pfad waere schlimmer als eine
 * klare Frage. */
static void folder_request_path(void)
{
    struct FileRequester *req;
    STRPTR alt = NULL;

    get(str_folder, MUIA_String_Contents, &alt);

    req = (struct FileRequester *)
              MUI_AllocAslRequest(ASL_FileRequest, NULL);
    if (!req) {
        say("folder requester not available");
        return;
    }

    if (MUI_AslRequestTags(req,
            ASLFR_TitleText,   (ULONG)"Folder with MP3 files",
            ASLFR_DrawersOnly, TRUE,
            ASLFR_InitialDrawer, (ULONG)((alt && *alt) ? alt : (STRPTR)""),
            TAG_DONE)) {
        char full[192];

        /* Bei DrawersOnly steht das Ergebnis im Pfad, der Dateiname
         * bleibt leer - beides zusammenzusetzen waere hier falsch. */
        strncpy(full, (const char *)req->fr_Drawer, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
        set(str_folder, MUIA_String_Contents, (ULONG)full);
    }
    MUI_FreeAslRequest(req);
}

/* Das Einstellfenster mit dem aktuellen Stand fuellen und oeffnen. */
static void prefs_open(void)
{
    static char hostline[200];

    sprintf(hostline, "%s://%s:%ld",
            g_prefs.https ? "https" : "http", g_prefs.host,
            (LONG)g_prefs.port);

    set(str_host, MUIA_String_Contents, (ULONG)hostline);
    set(str_user, MUIA_String_Contents, (ULONG)g_prefs.user);
    set(str_pass, MUIA_String_Contents, (ULONG)g_prefs.pass);
    set(cyc_ahi, MUIA_Cycle_Active, (ULONG)g_prefs.ahiunit);
    {
        ULONG i;

        for (i = 0; i < 5; i++) {
            if (g_vis_fps[i] == g_prefs.visfps) {
                set(cyc_vis, MUIA_Cycle_Active, i);
            }
        }
    }
    set(str_folder, MUIA_String_Contents, (ULONG)g_prefs.folder);
    set(chk_screen, MUIA_Selected, g_prefs.ownscreen ? TRUE : FALSE);
    mode_text();

    set(prefswin, MUIA_Window_Open, TRUE);

    /* Die Leiste markiert beim Anklicken selbst, was angetippt wurde.
     * Settings ist aber keine Ansicht - hinter dem Fenster steht
     * weiterhin das, was vorher zu sehen war, und genau das soll die
     * Markierung auch zeigen. */
    set(sidebar, MUIA_Sb_Active, g_sb_home);
}

/* Alles wegwerfen, was vom Server stammt. Gemeinsamer Teil von
 * "Zugang geaendert" und "Scan" - der Unterschied steht bei den
 * Aufrufern. */
static void data_reset(void)
{
    int v;

    /* Die Bildwaende: beide Ansichten samt Miniaturen. thumbs_free()
     * arbeitet auf der SICHTBAREN - also jede einmal sichtbar machen. */
    for (v = 0; v < VIEW_COUNT; v++) {
        if (v != g_set_cur) {
            view_save(g_set_cur);
            view_load(v);
        }
        thumbs_free();
        list_free(&g_albums);
        list_init(&g_albums, sizeof(struct Album));
        g_sets[v].loaded = FALSE;
    }

    /* Die Titellisten und ihre Cover-Plaetze. */
    list_free(&g_tracks);
    list_init(&g_tracks, sizeof(struct Song));
    list_init(&g_local, sizeof(struct Song));
    list_init(&g_lalbums, sizeof(struct Album));
    list_free(&g_favs);
    list_init(&g_favs, sizeof(struct Song));
    list_free(&g_radios);
    list_init(&g_radios, sizeof(struct Radio));

    DoMethod(lst_tracks, MUIM_TL_SetList, (ULONG)NULL);
    DoMethod(lst_favs,   MUIM_TL_SetList, (ULONG)NULL);
    DoMethod(lst_radio,  MUIM_TL_SetList, (ULONG)NULL);

    if (g_cslots) {
        int i;

        for (i = 0; i < g_cslot_count; i++) {
            if (g_cslots[i].th.rgb) {
                free(g_cslots[i].th.rgb);
            }
        }
        free(g_cslots);
        g_cslots = NULL;
    }
    g_cslot_count = 0;
    g_cslot_max   = 0;
    g_cslot_fill  = 0;
    radio_thumbs_free();

    g_track_offset = 0;
    g_track_more   = TRUE;
    g_track_seen   = FALSE;
    g_fav_seen     = FALSE;
    g_radio_seen   = FALSE;
}

/* Nach einer Aenderung am Zugang: alles weg und von vorn. Erst der
 * Rechnername, dann holt job_done() die Startseite. Der Arbeitsprozess
 * liest g_prefs bei jedem Auftrag neu, ein Neustart ist nicht noetig. */
static void reconnect(void)
{
    data_reset();

    memset(&g_job, 0, sizeof(g_job));
    g_job.op = NJ_RESOLVE;
    if (net_submit(&g_job)) {
        say("reconnecting ...");
    }
}

static void radios_request(void);

/* Der Scan-Knopf: alles neu vom Server holen, ohne die Verbindung
 * anzufassen. Angefangen wird bei dem, was gerade zu sehen ist - die
 * uebrigen Ansichten holen sich ihres beim naechsten Besuch, sie stehen
 * ja jetzt alle wieder auf "noch nicht geholt". */
static void folder_request(void);

static void rescan(void)
{
    if (!g_have_prefs || net_busy()) {
        say("still busy with another request");
        return;
    }

    data_reset();

    /* Der eigene Bestand wird MITGENOMMEN. Er haengt nicht am Server,
     * aber "Aktualisieren" soll alles auffrischen, was angezeigt wird -
     * eine halbe Auffrischung waere nur verwirrend. Der Durchgang kostet
     * gemessene 1,2 s fuer 234 Dateien. */
    g_local_seen = FALSE;
    say("scanning ...");

    switch (g_page) {
    case PAGE_FOLDER:
        g_local_seen = TRUE;
        folder_request();
        break;
    case PAGE_FALBUMS:
        g_local_seen = TRUE;
        g_want_falbums = TRUE;
        folder_request();
        break;
    case PAGE_TRACKS:
        g_track_seen = TRUE;
        tracks_request();
        break;
    case PAGE_FAVS:
        g_fav_seen = TRUE;
        favs_request();
        break;
    case PAGE_RADIO:
        g_radio_seen = TRUE;
        radios_request();
        break;
    default:
        /* Bildwand oder Playeransicht: die sichtbare Albenliste. */
        albums_request();
        break;
    }
}

/* Sichern. Zugang wirkt sofort, der Bildschirm erst beim naechsten
 * Start - ein offenes Fenster laesst sich nicht umhaengen. */
static void prefs_apply(void)
{
    char oldhost[128];
    char olduser[64];
    char oldpass[128];
    STRPTR t = NULL;
    BOOL changed;
    LONG sel = FALSE;

    strcpy(oldhost, g_prefs.host);
    strcpy(olduser, g_prefs.user);
    strcpy(oldpass, g_prefs.pass);

    get(str_host, MUIA_String_Contents, &t);
    if (t && *t) {
        prefs_set_host(&g_prefs, (const char *)t);
    }
    get(str_user, MUIA_String_Contents, &t);
    if (t) {
        strncpy(g_prefs.user, (const char *)t, sizeof(g_prefs.user) - 1);
        g_prefs.user[sizeof(g_prefs.user) - 1] = 0;
    }
    get(str_pass, MUIA_String_Contents, &t);
    if (t) {
        strncpy(g_prefs.pass, (const char *)t, sizeof(g_prefs.pass) - 1);
        g_prefs.pass[sizeof(g_prefs.pass) - 1] = 0;
    }
    {
        LONG u = 0;

        get(cyc_ahi, MUIA_Cycle_Active, &u);
        g_prefs.ahiunit = (int)u;
    }
    {
        LONG u = 3;

        /* Wirkt sofort - anders als die AHI-Unit. */
        get(cyc_vis, MUIA_Cycle_Active, &u);
        if (u < 0 || u > 4) {
            u = 3;
        }
        g_prefs.visfps = g_vis_fps[u];
        set(visual, MUIA_Vis_Fps, (ULONG)g_prefs.visfps);
    }
    {
        char old[192];

        strcpy(old, g_prefs.folder);
        get(str_folder, MUIA_String_Contents, &t);
        if (t) {
            strncpy(g_prefs.folder, (const char *)t,
                    sizeof(g_prefs.folder) - 1);
            g_prefs.folder[sizeof(g_prefs.folder) - 1] = 0;
        }
        /* Anderes Verzeichnis heisst: der alte Bestand gilt nicht mehr.
         * Er wird beim naechsten Besuch von Folder neu durchsucht. */
        if (strcmp(old, g_prefs.folder) != 0) {
            g_local_seen = FALSE;
        }
    }
    get(chk_screen, MUIA_Selected, &sel);
    g_prefs.ownscreen = (sel != 0);

    changed = (strcmp(oldhost, g_prefs.host) != 0)
           || (strcmp(olduser, g_prefs.user) != 0)
           || (strcmp(oldpass, g_prefs.pass) != 0);

    if (prefs_save(&g_prefs) != SUB_OK) {
        say(sub_last_error());
        return;
    }
    set(prefswin, MUIA_Window_Open, FALSE);

    if (changed) {
        g_have_prefs = TRUE;
        reconnect();
    } else {
        say("settings saved");
    }
}

/* Die Radiostationen anfordern. */
static void radios_request(void)
{
    if (g_radio_job || !g_have_prefs || net_busy()) {
        return;
    }

    list_free(&g_radios);
    list_init(&g_radios, sizeof(struct Radio));

    memset(&g_job, 0, sizeof(g_job));
    g_job.op       = NJ_RADIOS;
    g_job.out_list = &g_radios;
    if (net_submit(&g_job)) {
        g_radio_job = TRUE;
        say("loading radio stations ...");
    }
}

static void radios_fill(void)
{
    char msg[64];

    g_radio_job = FALSE;
    radio_thumbs_build();
    DoMethod(lst_radio, MUIM_TL_SetList,   (ULONG)&g_radios);
    DoMethod(lst_radio, MUIM_TL_SetThumbs, (ULONG)g_radio_rows);

    if (g_radios.count > 0) {
        sprintf(msg, "%d radio stations", g_radios.count);
    } else {
        strcpy(msg, "no radio stations on the server");
    }
    say(msg);
}

static void radios_show(void)
{
    show_page(PAGE_RADIO);

    if (!g_radio_seen) {
        g_radio_seen = TRUE;
        radios_request();
    }
}

/* Auf die Titelliste umschalten. Beim ersten Mal wird geholt. */
static void falbums_build(void);
static void falbum_open(int idx);
static void lrows_rebuild(void);

/* Der erste Titel eines eigenen Albums - fuer das eingebettete Cover. */
static struct Song *lalbum_first(int idx)
{
    int i;

    for (i = 0; i < g_local.count; i++) {
        struct Song *s = (struct Song *)list_get(&g_local, i);

        if (s && s->lalbum == idx) {
            return s;
        }
    }
    return NULL;
}

/* Den eigenen Bestand durchsuchen lassen. */
static void folder_request(void)
{
    if (!g_prefs.folder[0] || net_busy()) {
        return;
    }

    /* Die Liste gehoert waehrend des Auftrags dem Arbeiter - erst
     * abhaengen, dann leeren (dieselbe Regel wie bei den Alben). */
    DoMethod(lst_folder, MUIM_TL_SetList, (ULONG)NULL);
    list_free(&g_local);
    list_init(&g_local, sizeof(struct Song));

    memset(&g_job, 0, sizeof(g_job));
    g_job.op = NJ_SCAN;
    strncpy(g_job.path, g_prefs.folder, sizeof(g_job.path) - 1);
    g_job.out_list = &g_local;
    if (net_submit(&g_job)) {
        say("scanning folder ...");
    }
}

/* Der Durchgang ist fertig. */
static void folder_fill(struct NetJob *j)
{
    char msg[128];

    /* ERST gruppieren, DANN alphabetisch sortieren.
     *
     * local_group() sortiert selbst - nach Verzeichnis, Album und
     * Titelnummer, denn nur so stehen die Titel eines Albums beieinander.
     * Wer danach nicht mehr umsortiert, bekommt in der Titelliste die
     * Albenreihenfolge zu sehen statt des Alphabets. Die Zuordnung zum
     * Album ueberlebt das Umsortieren, weil sie in jedem Titel steht
     * (lalbum) und nicht an seiner Zeilennummer haengt.
     *
     * Gruppiert wird immer, nicht nur auf Wunsch: die Zeilen brauchen
     * die Albenzuordnung fuer ihre kleinen Cover, auch wenn die Bildwand
     * noch gar nicht besucht wurde. */
    g_want_falbums = FALSE;
    falbums_build();

    if (g_local.count > 0 && g_local.items) {
        qsort(g_local.items, (size_t)g_local.count,
              (size_t)g_local.itemsize, song_cmp);
    }
    DoMethod(lst_folder, MUIM_TL_SetList, (ULONG)&g_local);
    lrows_rebuild();

    sprintf(msg, "%ld files in %ld folders - scan %ld.%02ld s, "
                 "tags %ld.%02ld s%s",
            (long)j->scan_files, (long)j->scan_dirs,
            (long)(j->scan_cs / 100), (long)(j->scan_cs % 100),
            (long)(j->scan_tagcs / 100), (long)(j->scan_tagcs % 100),
            j->scan_full ? " (limit reached!)" : "");
    say(msg);
}

/* Aus den gefundenen Titeln Alben bilden und die Bildwand fuellen.
 *
 * Gruppiert wird erst hier und nicht schon im Arbeitsprozess: die
 * Titelliste ist die Grundlage, und sie kann sich noch aendern (anderes
 * Verzeichnis, neuer Durchgang). */
static void falbums_build(void)
{
    DoMethod(grid_folder, MUIM_AG_SetList, (ULONG)NULL, (ULONG)NULL);

    DoMethod(lst_folder, MUIM_TL_SetThumbs, (ULONG)NULL);
    if (g_lrows) {
        free(g_lrows);
        g_lrows = NULL;
    }
    if (g_lthumbs) {
        lthumbs_free(g_lthumbs, g_lalbums.count);
        g_lthumbs = NULL;
    }
    if (g_lsmall) {
        lthumbs_free(g_lsmall, g_lalbums.count);
        g_lsmall = NULL;
    }
    list_free(&g_lalbums);
    list_init(&g_lalbums, sizeof(struct Album));

    local_group(&g_local, &g_lalbums);

    if (g_lalbums.count > 0) {
        g_lthumbs = calloc((size_t)g_lalbums.count,
                           sizeof(struct AlbumThumb));
    }
    DoMethod(grid_folder, MUIM_AG_SetList,
             (ULONG)&g_lalbums, (ULONG)g_lthumbs);

    /* Die Cover kommen danach, eines je Schleifendurchlauf. */
    g_lfill_idx = (g_lalbums.count > 0 && g_lthumbs) ? 0 : -1;

    if (g_lalbums.count > 0) {
        g_lsmall = calloc((size_t)g_lalbums.count, sizeof(struct AlbumThumb));
    }
    lrows_rebuild();
}

/* Die Zeigerreihe fuer Folder/Tracks: je Zeile das Cover ihres Albums.
 *
 * Muss nach jedem Sortieren neu gebaut werden - die Zeilen verschieben
 * sich dabei. Ueber das Feld lalbum bleibt die Zuordnung trotzdem
 * richtig. */
static void lrows_rebuild(void)
{
    int i;

    if (g_lrows) {
        free(g_lrows);
        g_lrows = NULL;
    }
    if (g_local.count <= 0 || !g_lsmall) {
        DoMethod(lst_folder, MUIM_TL_SetThumbs, (ULONG)NULL);
        return;
    }

    g_lrows = calloc((size_t)g_local.count, sizeof(struct AlbumThumb *));
    if (!g_lrows) {
        return;
    }
    for (i = 0; i < g_local.count; i++) {
        struct Song *s = (struct Song *)list_get(&g_local, i);

        if (s && s->lalbum >= 0 && s->lalbum < g_lalbums.count) {
            g_lrows[i] = &g_lsmall[s->lalbum];
        }
    }
    DoMethod(lst_folder, MUIM_TL_SetThumbs, (ULONG)g_lrows);
}

/* Ein eigenes Album oeffnen.
 *
 * Kein Netzauftrag noetig: die Titel liegen schon in g_local, und dort
 * stehen sie dank local_group() hintereinander. Sie werden in g_songs
 * KOPIERT - damit arbeitet alles Weitere (Anzeigeflaeche, Warteschlange,
 * Liste) unveraendert, egal woher der Titel kommt. */
static void falbum_open(int idx)
{
    struct Album *al = (struct Album *)list_get(&g_lalbums, idx);
    char path[256];
    int i;

    if (!al) {
        return;
    }

    show_page(PAGE_PLAYER);

    /* g_songs wird gleich freigegeben und neu belegt - das ist malloc,
     * und das ist verboten, solange ein Netzauftrag laeuft (netjob.h).
     * Die Bildwand holt nebenher Cover, also kommt das vor. Dann wie
     * beim Serveralbum warten, job_done() holt es nach. */
    if (net_busy()) {
        g_want_lalbum = idx;
        say("just a moment ...");
        return;
    }
    g_want_lalbum = -1;

    DoMethod(lst_queue, MUIM_TL_SetList, (ULONG)NULL);
    list_free(&g_songs);
    list_init(&g_songs, sizeof(struct Song));

    /* Ueber lalbum suchen und nicht ueber einen Bereich: die Titelliste
     * wird fuer die Anzeige alphabetisch sortiert, ein Bereich waere
     * danach falsch. 234 Eintraege durchzusehen kostet nichts. */
    for (i = 0; i < g_local.count; i++) {
        struct Song *src = (struct Song *)list_get(&g_local, i);
        struct Song *dst;

        if (!src || src->lalbum != idx) {
            continue;
        }
        dst = (struct Song *)list_add(&g_songs);
        if (!dst) {
            break;
        }
        *dst = *src;
    }

    /* In der Reihenfolge des Albums, nicht alphabetisch. */
    if (g_songs.count > 1 && g_songs.items) {
        qsort(g_songs.items, (size_t)g_songs.count,
              (size_t)g_songs.itemsize, track_cmp);
    }

    DoMethod(lst_queue, MUIM_TL_SetList, (ULONG)&g_songs);
    strncpy(g_cur_album, "", sizeof(g_cur_album) - 1);

    /* Die Anzeigeflaeche zeigt Album und Cover - dasselbe Bild wie in
     * der Bildwand, also aus derselben Quelle. */
    set(panel, MUIA_Panel_Title,  (ULONG)al->name);
    set(panel, MUIA_Panel_Artist, (ULONG)al->artist);
    set(panel, MUIA_Panel_Album,  (ULONG)al->name);

    if (local_cover(al, lalbum_first(idx),
                    g_prefs.cache, path, sizeof(path))) {
        strncpy(g_coverpath, path, sizeof(g_coverpath) - 1);
        cover_ready(g_coverpath);
    }

    {
        char msg[64];

        sprintf(msg, "%d tracks", g_songs.count);
        say(msg);
    }
}

/* Ein Schritt beim Fuellen der eigenen Bildwand.
 *
 * Kein Netz, kein Sekundentakt, keine Drosselung: die Bilder liegen auf
 * der Platte. Trotzdem eines je Durchlauf - ein Cover zu laden und zu
 * verkleinern kostet rund 0,2 s, und waehrenddessen soll das Fenster
 * auf Klicks reagieren. */
static BOOL lthumbs_step(void)
{
    struct Album *al;
    struct Song  *first;
    char path[256];

    if (g_lfill_idx < 0 || !g_lthumbs
            || g_lfill_idx >= g_lalbums.count) {
        return FALSE;
    }

    al = (struct Album *)list_get(&g_lalbums, g_lfill_idx);
    first = al ? lalbum_first(g_lfill_idx) : NULL;

    if (al && local_cover(al, first, g_prefs.cache, path, sizeof(path))) {
        struct CoverImage img;
        UBYTE *rgb = NULL;
        LONG w = 0, h = 0;

        if (cover_load(path, &img)) {
            /* Zweimal verkleinern aus DEMSELBEN geladenen Bild: gross
             * fuer die Bildwand, klein fuer die Zeilen. Das Laden ist
             * der teure Teil, das Verkleinern nicht. */
            if (cover_scale_rgb(&img, AG_THUMB_SIZE, FALSE, &rgb, &w, &h)) {
                g_lthumbs[g_lfill_idx].rgb = rgb;
                g_lthumbs[g_lfill_idx].w   = w;
                g_lthumbs[g_lfill_idx].h   = h;
                DoMethod(grid_folder, MUIM_AG_Refresh);
            }
            if (g_lsmall) {
                rgb = NULL;
                if (cover_scale_rgb(&img, TL_THUMB_SIZE, FALSE,
                                    &rgb, &w, &h)) {
                    g_lsmall[g_lfill_idx].rgb = rgb;
                    g_lsmall[g_lfill_idx].w   = w;
                    g_lsmall[g_lfill_idx].h   = h;
                    if (g_page == PAGE_FOLDER) {
                        MUI_Redraw(lst_folder, MADF_DRAWOBJECT);
                    }
                }
            }
            cover_unload(&img);
        }
    }

    g_lfill_idx++;
    if (g_lfill_idx >= g_lalbums.count) {
        g_lfill_idx = -1;           /* fertig */
    }
    return TRUE;
}

/* Die Ansicht Folder/Albums. */
static void falbums_show(void)
{
    show_page(PAGE_FALBUMS);

    if (!g_prefs.folder[0]) {
        say("no folder set - see Settings");
        return;
    }
    if (!g_local_seen) {
        g_local_seen = TRUE;
        g_want_falbums = TRUE;      /* nach dem Durchgang gruppieren */
        folder_request();
        return;
    }
    if (g_lalbums.count == 0) {
        falbums_build();
    }
    {
        char msg[64];

        sprintf(msg, "%d albums from local files", g_lalbums.count);
        say(msg);
    }
}

/* Die Ansicht Folder. Der Bestand wird beim ersten Besuch gescannt -
 * genau wie Tracks beim ersten Mal geholt wird. */
static void folder_show(void)
{
    show_page(PAGE_FOLDER);

    if (!g_prefs.folder[0]) {
        say("no folder set - see Settings");
        return;
    }
    if (!g_local_seen) {
        g_local_seen = TRUE;
        folder_request();
    }
}

static void tracks_show(void)
{
    show_page(PAGE_TRACKS);

    if (!g_track_seen) {
        g_track_seen = TRUE;
        tracks_request();
    }
}

/* Auf eine Ansicht der Bildwand umschalten.
 *
 * Ist sie schon einmal geholt worden, steht sie sofort da - Liste und
 * Miniaturen liegen ja noch. Sonst wird sie jetzt angefordert. */
static void view_show(int v)
{
    if (v != g_set_cur) {
        if (g_list_job) {
            /* Mitten in einer Albenlisten-Abfrage darf nicht getauscht
             * werden - der Arbeiter schreibt nach &g_albums. Der Wunsch
             * wird gemerkt und gleich nachgeholt. */
            g_want_view = v;
            return;
        }
        view_save(g_set_cur);
        view_load(v);
    }

    show_page(PAGE_HOME);

    if (g_sets[v].loaded || !g_have_prefs) {
        return;
    }
    if (net_busy()) {
        g_want_view = v;
        return;
    }
    albums_request();
}

static void album_request_id(const char *albumid);

static void album_request(int idx)
{
    struct Album *a = (struct Album *)list_get(&g_albums, idx);

    if (!a) {
        return;
    }
    if (net_busy()) {
        /* Vermutlich laeuft gerade eine Miniatur. Den Wunsch merken und
         * ihn erledigen, sobald die Antwort da ist - eine Absage waere
         * fuer den Anwender nicht nachvollziehbar, er sieht ja nicht,
         * dass im Hintergrund Cover nachgeladen werden. */
        g_want_album = idx;
        say("just a moment ...");
        return;
    }
    g_want_album = -1;
    album_request_id(a->id);
}

/* Ein Album ueber seine KENNUNG holen statt ueber den Platz in der
 * Bildwand. Der Klick auf die Miniatur unten braucht das: der laufende
 * Titel kann aus Tracks oder Favorites stammen, sein Album steht dann
 * vielleicht in keiner der beiden Bildwaende. Der Aufrufer stellt
 * sicher, dass kein Netzauftrag laeuft. */
static void album_request_id(const char *albumid)
{

    say("loading tracks ...");
    DoMethod(lst_queue, MUIM_TL_SetList, (ULONG)NULL);
    list_free(&g_songs);
    list_init(&g_songs, sizeof(struct Song));
    g_playing = -1;

    strncpy(g_cur_album, albumid, sizeof(g_cur_album) - 1);
    g_cur_album[sizeof(g_cur_album) - 1] = '\0';

    /* Die Cover-ID des Listeneintrags merken: getAlbum liefert zwar auch
     * eine, aber nicht jeder Server fuellt sie. */
    sub_album_cover_id(albumid, g_pending_cover, sizeof(g_pending_cover));
    g_pending_cover[sizeof(g_pending_cover) - 1] = '\0';

    memset(&g_job, 0, sizeof(g_job));
    g_job.op        = NJ_ALBUM_SONGS;
    strncpy(g_job.id, albumid, sizeof(g_job.id) - 1);
    g_job.out_list  = &g_songs;
    g_job.out_album = &g_pending_album;
    net_submit(&g_job);
}

static void album_fill(void)
{
    DoMethod(lst_queue, MUIM_TL_SetList, (ULONG)&g_songs);

    if (g_songs.count > 0) {
        show_song(0);
        set(lst_queue, MUIA_TL_Active, 0);
    }

    /* Steht der laufende Titel in der frisch geladenen Liste, wird er
     * wieder markiert. */
    mark_playing();

    {
        char msg[64];
        sprintf(msg, "%d tracks - loading covers ...", g_songs.count);
        say(msg);
    }

    /* Jetzt das Cover. Liegt es schon im Cache, geht das ohne Netz und
     * ohne Auftrag - der haeufige Fall, sobald die Bildwand einmal
     * durchgelaufen ist. */
    {
        const char *cid = g_pending_album.coverart[0]
                        ? g_pending_album.coverart : g_pending_cover;
        char msg[64];

        if (cid[0] && cache_have(&g_prefs, cid)) {
            cache_path(&g_prefs, cid, g_coverpath, sizeof(g_coverpath));
            cover_ready(g_coverpath);
            sprintf(msg, "%d tracks loaded", g_songs.count);
            say(msg);
        } else if (cid[0]) {
            memset(&g_job, 0, sizeof(g_job));
            g_job.op   = NJ_COVER;
            g_job.size = 300;
            strncpy(g_job.id, cid, sizeof(g_job.id) - 1);
            g_thumb_job = FALSE;        /* dieses Cover ist fuers Panel */
            net_submit(&g_job);
        } else {
            sprintf(msg, "%d tracks loaded", g_songs.count);
            say(msg);
        }
    }
}

/* MESSUNG, 21.9.2026: warum scheitern die Cover in WinUAE? Die Bild-
 * wand verschluckt den Fehlertext absichtlich, die Statuszeile sagte
 * nur "network not responding" - obwohl Radio und Listen liefen. Jede
 * gescheiterte Cover-Anfrage kommt deshalb mit Uhrzeit, Kennung und
 * Grund nach T:, damit man den Grund LIEST statt ihn zu raten. */
static void cover_fail_log(const struct NetJob *j)
{
    struct DateStamp ds;
    BPTR fh;

    fh = Open((STRPTR)"T:AmiSubsonic-cover.log", MODE_READWRITE);
    if (!fh) {
        return;
    }
    Seek(fh, 0, OFFSET_END);
    DateStamp(&ds);
    FPrintf(fh, "%02ld:%02ld:%02ld  %-40s rc=%ld  %s\n",
            ds.ds_Minute / 60, ds.ds_Minute % 60, ds.ds_Tick / 50,
            (LONG)(ULONG)j->id, (LONG)j->rc,
            (LONG)(ULONG)(j->error[0] ? j->error : "(kein Text)"));
    Close(fh);
}

/* Eine Antwort ist da. */
static void job_done(struct NetJob *j)
{
    if (j->rc != SUB_OK && j->op == NJ_COVER) {
        cover_fail_log(j);
    }
    if (j->rc != SUB_OK) {
        /* Ein fehlgeschlagenes Cover in der Bildwand ist kein Fehler,
         * den der Anwender lesen muss - es bleibt einfach ein
         * Platzhalter stehen. Wichtig ist nur, dass die Reihe
         * WEITERLAEUFT: ohne das Weiterzaehlen bliebe sie an einem
         * kaputten Cover haengen und die Wand fuellte sich nie fertig. */
        /* Ein Sender ohne Cover: still weiter zum naechsten, die
         * Zeile behaelt ihre Antenne. */
        if (j->op == NJ_COVER && g_rcover_job) {
            g_rcover_job = FALSE;
            g_rfill++;
            return;
        }
        if (j->op == NJ_COVER && g_thumb_job) {
            g_thumb_job = FALSE;
            g_fill_gap  = TRUE;
            g_fill_idx++;
            g_fill_fails++;

            /* Drei Fehlschlaege in Folge heissen: das Netz ist gerade
             * nicht da. Dann NICHT weiter durch die Liste rennen - jeder
             * weitere Versuch kostet bis zu 5 s Verbindungsgrenze, und
             * bei 44 uebrigen Alben sind das vier Minuten, an deren Ende
             * die Wand luecken haft fertig waere. Lieber Pause und
             * spaeter ein neuer Anlauf. */
            if (g_fill_fails >= FILL_MAXFAIL) {
                /* Den echten Grund nennen. "network not responding"
                 * stand hier fest und hat auf die falsche Faehrte
                 * gefuehrt: in WinUAE liefen Radio und Listen, nur die
                 * Cover scheiterten (21.9.2026). */
                char msg[120];

                sprintf(msg, "covers failed: %.80s - pausing",
                        j->error[0] ? j->error : "unknown error");
                say(msg);
                g_fill_idx  = -1;
                g_fill_hold = FILL_HOLD;
            }
            return;
        }

        /* lrclib ueberlastet: das gehoert in den LYRICS-Tab, nicht in
         * die Statuszeile - ein fehlender Liedtext ist kein Fehler, den
         * man dort lesen muss, und es liegt nicht am eigenen Server. */
        if (j->op == NJ_LYRICS && j->rc == SUB_EBUSY) {
            strcpy(g_lyrics, "\n  (lrclib.net is busy - "
                             "try again later)");
            DoMethod(ft_lyrics, MUIM_TL_SetText, (ULONG)g_lyrics);
            return;
        }

        say(j->error[0] ? j->error : "request failed");
        /* Ein fehlender Liedtext ist kein Grund, den Tab leer zu
         * lassen - da soll stehen, warum. */
        if (j->op == NJ_LYRICS) {
            strcpy(g_lyrics, "\n  (no lyrics found)");
            DoMethod(ft_lyrics, MUIM_TL_SetText, (ULONG)g_lyrics);
        }
        return;
    }

    switch (j->op) {
    case NJ_RESOLVE:
        /* Die Bildwand ist die Startseite - also gleich die
         * meistgespielten Alben holen, ohne dass jemand etwas anklicken
         * muss. Albums kommt erst, wenn es angetippt wird. */
        say("connected");
        view_show(VIEW_HOME);
        break;

    case NJ_ALBUMS:
        g_list_job = FALSE;
        albums_fill();
        break;

    case NJ_SCAN:
        folder_fill(j);
        break;

    case NJ_TRACKS:
        tracks_fill();
        break;

    case NJ_FAVORITES:
        favs_fill();
        break;

    case NJ_RADIOS:
        radios_fill();
        break;

    case NJ_ALBUM_SONGS:
        album_fill();
        break;

    case NJ_COVER:
        /* Der Arbeiter hat den Pfad im Cache nach j->path zurueck-
         * geschrieben. Er wird hier sofort ausgewertet oder kopiert -
         * beim naechsten Auftrag ist er ueberschrieben. */
        if (g_rcover_job) {
            /* Ein Sendercover. Gehoert es zum laufenden Sender, kommt
             * es auch gleich in die Bedienleiste. */
            struct Radio *r;

            g_rcover_job = FALSE;
            if (g_rfill < g_rthumb_count) {
                thumb_small_load(&g_rthumbs[g_rfill], j->path);
                MUI_Redraw(lst_radio, MADF_DRAWOBJECT);
                r = (struct Radio *)list_get(&g_radios, g_rfill);
                if (r && g_radio_on && strcmp(r->id, g_radio_id) == 0) {
                    radio_cover_show();
                }
                g_rfill++;
            }
        } else if (g_cslot_job) {
            /* Ein Cover fuer die Titelliste. */
            g_cslot_job = FALSE;
            if (g_cslot_fill < g_cslot_count) {
                cslot_load(&g_cslots[g_cslot_fill], j->path);
                rows_refresh();
                now_cover_late(g_cslots[g_cslot_fill].id, j->path);
                g_cslot_fill++;
            }
        } else if (g_thumb_job && g_thumb_job_view != g_set_cur) {
            /* Waehrend der Abruf lief, wurde die Ansicht gewechselt. Das
             * Bild gehoert zur anderen Wand und wird hier NICHT
             * eingehaengt - es laege sonst im falschen Miniaturfeld.
             * Verloren ist nichts: die Datei liegt jetzt im Cache, beim
             * naechsten Besuch ist es ein Treffer ohne Netz. */
            g_thumb_job = FALSE;
        } else if (g_thumb_job) {
            g_thumb_job  = FALSE;
            g_fill_fails = 0;
            if (!thumb_load(g_fill_idx, j->path)) {
                g_fill_gap = TRUE;
            }
            DoMethod(grid, MUIM_AG_Refresh);
            g_fill_idx++;
        } else {
            char msg[64];

            strncpy(g_coverpath, j->path, sizeof(g_coverpath) - 1);
            g_coverpath[sizeof(g_coverpath) - 1] = '\0';
            cover_ready(g_coverpath);
            sprintf(msg, "%d tracks loaded", g_songs.count);
            say(msg);
        }
        break;

    case NJ_LYRICS:
        if (!g_lyrics[0]) {
            strcpy(g_lyrics, "\n  (no lyrics found)");
        } else {
            /* lrclib liefert wahlweise mit Zeitmarken. Fuer die
             * Anzeige fliegen sie raus; sobald die Zeile zur
             * Abspielposition hervorgehoben werden soll, werden sie
             * hier gebraucht statt weggeworfen. */
            strip_timestamps(g_lyrics);
        }
        DoMethod(ft_lyrics, MUIM_TL_SetText, (ULONG)g_lyrics);
        break;
    }

    /* Hat der Anwender waehrenddessen ein Album angetippt, kommt es
     * jetzt dran - vor der naechsten Miniatur. */
    if (g_want_album >= 0 && !net_busy()) {
        int idx = g_want_album;

        g_want_album = -1;
        album_request(idx);
    }
    if (g_want_lalbum >= 0 && !net_busy()) {
        falbum_open(g_want_lalbum);
        mark_playing();
    }
    if (g_want_album_id[0] && !net_busy()) {
        char id[SUB_ID_LEN];

        strcpy(id, g_want_album_id);
        g_want_album_id[0] = '\0';
        album_request_id(id);
    }

    /* Dasselbe fuer einen Ansichtswechsel, der auf das Netz warten
     * musste. */
    if (g_want_view >= 0 && !net_busy() && !g_list_job) {
        int v = g_want_view;

        g_want_view = -1;
        view_show(v);
    }

    /* Die naechste Seite der Titelliste wird NICHT hier angestossen,
     * sondern im Sekundentakt - siehe tick(). */
}

/* ------------------------------------------------------------------ */
/* Sekundentakt                                                        */
/* ------------------------------------------------------------------ */

static void tick(void)
{
    int pos = -1, total = -1;
    struct Song *s;

    /* Die Marke fuer HOECHSTENS EINEN Cover-Abruf je Sekunde. Sie ist
     * die ganze Drosselung: ohne sie holt die Bildwand die Cover so
     * schnell, wie die Maschine kann, und genau danach war der Rechner
     * schon zweimal minutenlang nicht mehr erreichbar. */
    g_fill_token = TRUE;

    say_expire();

    if (g_fill_hold > 0) {
        g_fill_hold--;
        if (g_fill_hold == 0 && g_fill_idx < 0
                && g_fill_gap && g_fill_pass + 1 < FILL_PASSES) {
            /* Neuer Anlauf. Was schon im Cache liegt, ueberspringt er
             * ohne Netzzugriff - der zweite Durchgang ist also billig. */
            g_fill_pass++;
            fill_start();
        }
    }

    /* WAS LAEUFT, WEISS JETZT DER AUDIOPROZESS.
     *
     * Frueher stand hier eine Mitschrift dessen, was AmigaAMP tat:
     * Position sprang zurueck = ein Titel weiter, STOP bei Position 0 =
     * Liste durch. Das war Rateraten aus Nebenwirkungen. Der eigene
     * Abspieler sagt es direkt - die Position kommt aus den dekodierten
     * Abtastwerten und ist damit genau. */
    pos = (int)(audio_pos_ms() / 1000);
    s = g_now_ok ? &g_now : NULL;
    total = s ? s->duration : 0;

    set(player, MUIA_Pl_Pos,     pos < 0 ? 0 : pos);
    set(player, MUIA_Pl_Total,   total > 0 ? total : 0);
    set(player, MUIA_Pl_Playing, audio_state() == AU_PLAYING);

    if (audio_error()) {
        say("cannot read stream");
    }

    /* Titel zu Ende: einen weiter. Genau hier liegt die Warteschlange
     * jetzt - nicht mehr in AmigaAMPs Playlist. */
    /* Radio: hat der Sender einen neuen Titel gemeldet? Nur die Zahl
     * vergleichen, den Text erst holen, wenn sie sich bewegt hat. */
    if (g_radio_on && net_icy_seq() != g_icy_seen) {
        char t[160];

        g_icy_seen = net_icy_seq();
        net_icy_title(t, sizeof(t));
        radio_show_title(t);
    }

    if (audio_track_done() && g_radio_on) {
        /* Ein Sender hoert nicht von selbst auf. Ist der Ring trotzdem
         * zu Ende, hat der Netzprozess aufgegeben: weder MP3 noch AAC,
         * oder das Netz blieb laenger weg als die Geduld reicht.
         * Keinesfalls in die Warteschlange weiterschalten. */
        audio_clear_done();
        g_expect_play = FALSE;
        set(player, MUIA_Pl_Playing, FALSE);
        if (net_stream_unsupported()) {
            char msg[96];

            sprintf(msg, "station sends %.40s - "
                    "not supported", net_stream_ctype());
            say(msg);
            radio_mark_unplayable();
        } else {
            say("station not reachable");
        }
    } else if (audio_track_done()) {
        audio_clear_done();
        if (g_qpos + 1 < g_qcount) {
            play_queue(g_qpos + 1);
        } else {
            g_expect_play = FALSE;
            set(player, MUIA_Pl_Playing, FALSE);
        }
    }

    net_stream_reap();
    if (net_stream_total() > 0) {
        g_stream_bytes = net_stream_total();
    }

    /* Wenn das Netz weg ist, soll das nicht stumm bleiben - aber die
     * Meldung muss auch wieder VERSCHWINDEN, sobald es weitergeht.
     * Sonst steht dort minutenlang "Netz weg", waehrend laengst wieder
     * Musik laeuft; genau so gesehen am 20.9.2026. */
    {
        LONG w = net_stream_waited();

        if (w > 2) {
            char msg[64];

            sprintf(msg, "network down - retrying for %ld s", (long)w);
            say(msg);
            g_said_outage = TRUE;
        } else if (g_said_outage) {
            g_said_outage = FALSE;
            say("reconnected");
        }
    }

    /* Und hoechstens EINE Seite der Titelliste je Sekunde.
     *
     * Die Bremse steht hier aus demselben Grund wie die fuer die Cover:
     * ohne sie gehen die Abrufe so schnell hintereinander weg, wie der
     * Server antwortet. Am 6.9.2026 war der Rechner waehrend eines
     * solchen Durchgangs wieder eine knappe Minute nicht erreichbar
     * (AGENTS.md 5.8). Auslöser war vermutlich, dass versehentlich ZWEI
     * Instanzen liefen und beide gleichzeitig holten - bewiesen ist das
     * nicht. Die Bremse kostet nichts (ein Abruf dauert ohnehin rund
     * eine Sekunde) und nimmt dem Verdacht die Grundlage. */
    if (g_track_more && g_track_seen && !g_track_job
            && !g_list_job && !net_busy()) {
        tracks_request();
    }
}

/* Klick auf die Miniatur unten links: dorthin, woher das Laufende
 * stammt. Beim Radio die Senderliste, bei einem eigenen Titel sein
 * Ordner-Album, sonst das Album auf dem Server.
 *
 * Ist das Album schon geladen, wird nur umgeschaltet - kein zweiter
 * Abruf, und die Liste bleibt, wo man sie verlassen hat. */
static void now_goto(void)
{
    if (g_radio_on) {
        g_sb_home = SB_RADIO;
        radios_show();
        return;
    }
    if (!g_now_ok) {
        return;
    }

    if (g_now.path[0]) {
        /* g_now ist eine Kopie; ihre Albumnummer gilt nur, solange der
         * Ordner seitdem nicht neu durchsucht wurde. Deshalb die
         * Gegenprobe ueber den Pfad, bevor umgeschaltet wird. */
        int i;

        for (i = 0; i < g_local.count; i++) {
            struct Song *s = (struct Song *)list_get(&g_local, i);

            if (s && strcmp(s->path, g_now.path) == 0) {
                falbum_open(s->lalbum);
                mark_playing();
                return;
            }
        }
        say("track is no longer in the folder list");
        return;
    }

    if (!g_now.albumid[0]) {
        return;
    }
    if (strcmp(g_cur_album, g_now.albumid) == 0 && g_songs.count > 0) {
        show_page(PAGE_PLAYER);
        mark_playing();
        return;
    }
    show_page(PAGE_PLAYER);
    if (net_busy()) {
        strncpy(g_want_album_id, g_now.albumid, SUB_ID_LEN - 1);
        g_want_album_id[SUB_ID_LEN - 1] = '\0';
        say("just a moment ...");
        return;
    }
    album_request_id(g_now.albumid);
}

/* ------------------------------------------------------------------ */

/* Das eigene Icon fuer den ikonifizierten Zustand. Ohne das zeigt MUI
 * sein Standardbild statt PROGDIR:AmiSubsonic.info. Fehlt die Datei
 * oder icon.library, laeuft das Programm genauso - dann eben mit dem
 * Standardbild. */
struct Library *IconBase = NULL;
static struct DiskObject *g_diskobj = NULL;

static void icon_open(void)
{
    IconBase = OpenLibrary("icon.library", 37);
    if (IconBase) {
        g_diskobj = GetDiskObject((STRPTR)"PROGDIR:AmiSubsonic");
    }
}

/* Erst NACH MUI_DisposeObject(app): bis dahin kann MUI das Bild noch
 * zeigen. */
static void icon_close(void)
{
    if (g_diskobj) {
        FreeDiskObject(g_diskobj);
        g_diskobj = NULL;
    }
    if (IconBase) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
}

int main(void)
{
    ULONG sigs = 0;
    ULONG id;
    static const char *reg_titles[] = { "UP NEXT", "LYRICS", "VISUALIZER",
                                        NULL };

    list_init(&g_albums, sizeof(struct Album));
    list_init(&g_songs, sizeof(struct Song));
    list_init(&g_tracks, sizeof(struct Song));
    list_init(&g_favs, sizeof(struct Song));

    MUIMasterBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN);
    if (!MUIMasterBase) {
        printf("cannot open muimaster.library %d\n",
               MUIMASTER_VMIN);
        return 20;
    }

    if (!tl_init()) {
        printf("cannot create list class\n");
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    if (!pl_init()) {
        printf("cannot create player bar class\n");
        tl_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    if (!panel_init()) {
        printf("cannot create gradient class -\n"
               "is this machine really running RTG?\n");
        pl_cleanup();
        tl_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    if (!ag_init()) {
        printf("cannot create album grid class\n");
        panel_cleanup();
        pl_cleanup();
        tl_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    if (!sb_init()) {
        printf("cannot create sidebar class\n");
        ag_cleanup();
        panel_cleanup();
        pl_cleanup();
        tl_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    if (!tb_init()) {
        printf("cannot create tab class\n");
        sb_cleanup();
        ag_cleanup();
        panel_cleanup();
        pl_cleanup();
        tl_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    if (!vis_init()) {
        printf("cannot create visualizer class\n");
        tb_cleanup();
        sb_cleanup();
        ag_cleanup();
        panel_cleanup();
        pl_cleanup();
        tl_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    /* VOR dem Anlegen der Objekte: die Listen lesen MUIA_TL_Colour beim
     * Erzeugen. */
    g_tint = cover_scale(0x00512D7E, 50);

    g_have_prefs = (prefs_load(&g_prefs) == SUB_OK);

    /* VOR dem Anlegen der Fenster: sie bekommen den Namen des
     * Bildschirms mit, auf dem sie aufgehen sollen. */
    screen_open();

    /* Das feste Symbol der Titelliste. Fehlt es, bleibt der Platz leer. */
    track_icon_load();

    lst_queue = TrackListObject,
        MUIA_TL_Kind,   TLK_SONGS,
        MUIA_TL_Colour, g_tint,
        TAG_DONE);

    ft_lyrics = TrackListObject,
        MUIA_TL_Kind,   TLK_TEXT,
        MUIA_TL_Colour, g_tint,
        TAG_DONE);

    grid = AlbumGridObject,
        MUIA_AG_Colour, g_tint,
        MUIA_AG_Title,  (ULONG)view_title(VIEW_HOME),
        TAG_DONE);

    lst_tracks = TrackListObject,
        MUIA_TL_Kind,   TLK_TRACKS,
        MUIA_TL_Colour, g_tint,
        MUIA_TL_Title,  (ULONG)"Tracks",
        TAG_DONE);

    grid_folder = AlbumGridObject,
        MUIA_AG_Colour, g_tint,
        MUIA_AG_Title,  (ULONG)"Folder",
        TAG_DONE);

    lst_folder = TrackListObject,
        MUIA_TL_Kind,   TLK_TRACKS,
        MUIA_TL_Colour, g_tint,
        MUIA_TL_Title,  (ULONG)"Folder",
        TAG_DONE);

    lst_favs = TrackListObject,
        MUIA_TL_Kind,   TLK_TRACKS,
        MUIA_TL_Colour, g_tint,
        MUIA_TL_Title,  (ULONG)"Favorites",
        TAG_DONE);

    lst_radio = TrackListObject,
        MUIA_TL_Kind,   TLK_RADIO,
        MUIA_TL_Colour, g_tint,
        MUIA_TL_Title,  (ULONG)"Radio",
        TAG_DONE);

    sidebar = SidebarObject,
        MUIA_Sb_Active, SB_HOME,
        TAG_DONE);

    visual = VisualObject,
        MUIA_Vis_Colour, g_tint,
        MUIA_Vis_Fps,    (ULONG)(g_have_prefs ? g_prefs.visfps : 30),
        TAG_DONE);

    tabbar = TabsObject,
        MUIA_Tb_Titles, (ULONG)reg_titles,
        MUIA_Tb_Colour, g_tint,
        MUIA_Tb_Active, 0,
        TAG_DONE);

    player = PlayerObject,
        MUIA_Pl_Colour, g_tint,
        TAG_DONE);

    txt_status = MUI_NewObject(MUIC_Text,
        MUIA_Text_Contents, (char *)"",
        MUIA_Text_PreParse, (char *)MUIX_PH,
        TAG_DONE);

    panel = PanelObject,
        MUIA_Panel_Colour, 0x00512D7E,
        TAG_DONE);

    icon_open();
    app = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title,       "AmiSubsonic",
        MUIA_Application_Version,     "$VER: AmiSubsonic 0.2 (20.9.2026)",
        MUIA_Application_Copyright,   "2026 radi777",
        MUIA_Application_Author,      "radi777",
        MUIA_Application_Description, "Subsonic/Navidrome client",
        MUIA_Application_Base,        "AMISUBSONIC",
        MUIA_Application_DiskObject,  (ULONG)g_diskobj,

        /* Das Einstellfenster. Erst beim Anklicken von Settings
         * geoeffnet, aber schon hier angelegt - MUI verwaltet die
         * Fenster einer Anwendung, und ein spaeter angehaengtes waere
         * nur Umstand. */
        MUIA_Application_Window, prefswin = MUI_NewObject(MUIC_Window,
            MUIA_Window_Title,  "AmiSubsonic - Settings",
            MUIA_Window_ID,     0x41535550UL,   /* 'ASUP' */
            MUIA_Window_PublicScreen, (ULONG)g_pubname,
            MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Columns, 2,
                    MUIA_Frame,         MUIV_Frame_Group,
                    MUIA_FrameTitle,    "Navidrome",

                    MUIA_Group_Child, MUI_MakeObject(MUIO_Label,
                        (ULONG)"Server", 0),
                    MUIA_Group_Child, str_host = MUI_NewObject(MUIC_String,
                        MUIA_Frame,           MUIV_Frame_String,
                        MUIA_String_MaxLen,   190,
                        TAG_DONE),

                    MUIA_Group_Child, MUI_MakeObject(MUIO_Label,
                        (ULONG)"User", 0),
                    MUIA_Group_Child, str_user = MUI_NewObject(MUIC_String,
                        MUIA_Frame,           MUIV_Frame_String,
                        MUIA_String_MaxLen,   60,
                        TAG_DONE),

                    MUIA_Group_Child, MUI_MakeObject(MUIO_Label,
                        (ULONG)"Password", 0),
                    /* MUIA_String_Secret: MUI zeigt Sternchen. Das
                     * Passwort steht trotzdem im Klartext in den Prefs -
                     * anders geht es nicht, Subsonic bildet bei JEDER
                     * Anfrage md5(passwort+salt). */
                    MUIA_Group_Child, str_pass = MUI_NewObject(MUIC_String,
                        MUIA_Frame,           MUIV_Frame_String,
                        MUIA_String_MaxLen,   120,
                        MUIA_String_Secret,   TRUE,
                        TAG_DONE),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Frame,       MUIV_Frame_Group,
                    MUIA_FrameTitle,  "Local MP3 files",

                    MUIA_Group_Child, str_folder = MUI_NewObject(MUIC_String,
                        MUIA_Frame,         MUIV_Frame_String,
                        MUIA_String_MaxLen, 190,
                        TAG_DONE),
                    MUIA_Group_Child, btn_folder =
                        MUI_MakeObject(MUIO_Button, (ULONG)"Choose ..."),
                    TAG_DONE),

                /* Welcher Modus hinter einer Unit liegt, stellt der
                 * Anwender in den AHI-Voreinstellungen ein - so macht es
                 * jedes andere Amiga-Programm auch. Hier steht nur, ueber
                 * welche der vier Units gespielt wird. */
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Frame,       MUIV_Frame_Group,
                    MUIA_FrameTitle,  "Sound",

                    MUIA_Group_Child, MUI_MakeObject(MUIO_Label,
                        (ULONG)"AHI unit (applies after restart)", 0),
                    MUIA_Group_Child, cyc_ahi = MUI_NewObject(MUIC_Cycle,
                        MUIA_Frame,        MUIV_Frame_Button,
                        MUIA_Cycle_Entries, (ULONG)g_ahi_units,
                        TAG_DONE),
                    MUIA_Group_Child, MUI_MakeObject(MUIO_Label,
                        (ULONG)"Visualizer", 0),
                    MUIA_Group_Child, cyc_vis = MUI_NewObject(MUIC_Cycle,
                        MUIA_Frame,        MUIV_Frame_Button,
                        MUIA_Cycle_Entries, (ULONG)g_vis_rates,
                        MUIA_Cycle_Active, 3,
                        TAG_DONE),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Frame,      MUIV_Frame_Group,
                    MUIA_FrameTitle, "Screen",

                    MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                        MUIA_Group_Horiz, TRUE,
                        MUIA_Group_Child, chk_screen =
                            MUI_MakeObject(MUIO_Checkmark, NULL),
                        MUIA_Group_Child, MUI_MakeObject(MUIO_Label,
                            (ULONG)"Own screen (applies after restart)", 0),
                        MUIA_Group_Child, MUI_NewObject(MUIC_Rectangle,
                            TAG_DONE),
                        TAG_DONE),

                    MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                        MUIA_Group_Horiz, TRUE,
                        MUIA_Group_Child, btn_mode =
                            MUI_MakeObject(MUIO_Button, (ULONG)"Mode ..."),
                        MUIA_Group_Child, txt_mode = MUI_NewObject(MUIC_Text,
                            MUIA_Text_Contents, (ULONG)"same as Workbench",
                            TAG_DONE),
                        TAG_DONE),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Child, btn_save =
                        MUI_MakeObject(MUIO_Button, (ULONG)"Save"),
                    MUIA_Group_Child, MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                    MUIA_Group_Child, btn_cancel =
                        MUI_MakeObject(MUIO_Button, (ULONG)"Cancel"),
                    TAG_DONE),
                TAG_DONE),
            TAG_DONE),

        MUIA_Application_Window, win = MUI_NewObject(MUIC_Window,
            MUIA_Window_Title,  "AmiSubsonic",
            MUIA_Window_ID,     0x41535542UL,   /* 'ASUB' */
            MUIA_Window_PublicScreen, (ULONG)g_pubname,
            MUIA_Window_Width,  760,
            MUIA_Window_Height, 560,
            MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,
                MUIA_Group_Spacing, 0,

                /* Oben: Seitenleiste und die umschaltbaren Seiten. */
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz,   TRUE,
                    MUIA_Group_Spacing, 0,
                    MUIA_Group_Child, sidebar,

                    /* MUIA_Group_PageMode: dieselbe Mechanik, die das
                     * Register darunter schon benutzt - genau ein Kind
                     * ist sichtbar, umgeschaltet wird ueber
                     * MUIA_Group_ActivePage. Damit braucht die
                     * Albenansicht kein eigenes Fenster mehr. */
                    MUIA_Group_Child, pages = MUI_NewObject(MUIC_Group,
                        MUIA_Group_PageMode,   TRUE,
                        MUIA_Group_ActivePage, PAGE_HOME,

                        /* Seite 0: die Bildwand. */
                        MUIA_Group_Child, grid,

                        /* Seite 1: die Playeransicht.
                         *
                         * Kein Zurueck-Knopf und keine Leiste darueber -
                         * zurueck geht es ueber die Seitenleiste, die
                         * ohnehin immer da ist. Die Leiste war nur ein
                         * schwarzer Streifen ohne Inhalt. */
                        MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                            MUIA_Group_Horiz,   TRUE,
                            MUIA_Group_Spacing, 0,
                            MUIA_Group_Child, panel,

                            /* Eigene Reiterleiste statt Register.mui,
                             * damit sich die Laschen einfaerben lassen.
                             * Der Seitenwechsel bleibt MUIs Sache. */
                            MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                                MUIA_Group_Spacing, 0,
                                MUIA_Group_Child, tabbar,
                                MUIA_Group_Child,
                                    tabpages = MUI_NewObject(MUIC_Group,
                                        MUIA_Group_PageMode,   TRUE,
                                        MUIA_Group_ActivePage, 0,
                                        MUIA_Group_Child, lst_queue,
                                        MUIA_Group_Child, ft_lyrics,
                                        MUIA_Group_Child, visual,
                                        TAG_DONE),
                                TAG_DONE),
                            TAG_DONE),

                        /* Seite 2: alle Titel von A bis Z. Ohne UP NEXT
                         * und ohne Liedtext - nur die Liste. */
                        MUIA_Group_Child, lst_tracks,

                        /* Seite 3: die Favoriten, gleiches Zeilenbild. */
                        MUIA_Group_Child, lst_favs,

                        /* Seite 4: die Radiostationen. */
                        MUIA_Group_Child, lst_radio,

                        /* Seite 5: die eigenen Dateien von der Platte.
                         * Gleiches Zeilenbild wie Tracks - es sind
                         * dieselben Angaben, nur aus einer anderen
                         * Quelle.
                         *
                         * ACHTUNG: Die Reihenfolge dieser Kinder IST die
                         * Seitennummer (PAGE_...). Wer hier etwas
                         * einschiebt, verschiebt alle folgenden Seiten. */
                        MUIA_Group_Child, lst_folder,

                        /* Seite 6: die eigenen Alben als Bildwand. */
                        MUIA_Group_Child, grid_folder,
                        TAG_DONE),
                    TAG_DONE),

                /* Unten quer ueber beides: Bedienleiste und Status. */
                MUIA_Group_Child, player,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    /* Wie die Bedienleiste darueber: schwarz. */
                    MUIA_Background, (ULONG)"2:00000000,00000000,00000000",
                    MUIA_Group_Child, txt_status,
                    TAG_DONE),
                TAG_DONE),
            TAG_DONE),
        TAG_DONE);

    if (!app) {
        printf("cannot create application object\n");
        icon_close();
        vis_cleanup();
        tb_cleanup();
        sb_cleanup();
        ag_cleanup();
        panel_cleanup();
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             app, 2, MUIM_Application_ReturnID,
             MUIV_Application_ReturnID_Quit);

    DoMethod(sidebar, MUIM_Notify, MUIA_Sb_Pressed, MUIV_EveryTime,
             app, 2, MUIM_Application_ReturnID, ID_SIDEBAR);
    /* Die Leiste meldet ueber MUIA_Pl_Pressed, welcher Knopf es war -
     * ein Signal statt vier. Welcher genau, fragt die Schleife dann ab. */
    DoMethod(player, MUIM_Notify, MUIA_Pl_Pressed, MUIV_EveryTime,
             app, 2, MUIM_Application_ReturnID, ID_PLAYER);

    DoMethod(tabbar, MUIM_Notify, MUIA_Tb_Pressed, MUIV_EveryTime,
             app, 2, MUIM_Application_ReturnID, ID_TAB);

    /* Die eigene Klasse meldet den Doppelklick ueber MUIA_TL_DoubleClick.
     * Das funktioniert, weil ihr OM_SET den Aufruf IMMER an die
     * Oberklasse weiterreicht - erst dort loest MUIs Notify-Mechanik
     * aus. */
    DoMethod(grid, MUIM_Notify, MUIA_AG_Click, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_ALBUM_PICK);
    DoMethod(grid_folder, MUIM_Notify, MUIA_AG_Click, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_FALBUM_PICK);
    DoMethod(lst_queue, MUIM_Notify, MUIA_TL_DoubleClick, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_SONG_PICK);
    DoMethod(lst_tracks, MUIM_Notify, MUIA_TL_DoubleClick, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_TRACK_PICK);
    DoMethod(lst_favs, MUIM_Notify, MUIA_TL_DoubleClick, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_FAV_PICK);
    DoMethod(lst_radio, MUIM_Notify, MUIA_TL_DoubleClick, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_RADIO_PICK);
    /* Folder/Tracks. Fehlte bis zum 21.9.2026 ganz - der Doppelklick
     * kam nie an, und nichts meldete das (AGENTS.md 6: lautlos). */
    DoMethod(lst_folder, MUIM_Notify, MUIA_TL_DoubleClick, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_FOLDER_PICK);

    /* Das Einstellfenster. Sein Schliesskreuz schliesst nur DIESES
     * Fenster - das Programm laeuft weiter. */
    DoMethod(prefswin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             prefswin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(btn_save, MUIM_Notify, MUIA_Pressed, FALSE,
             app, 2, MUIM_Application_ReturnID, ID_SET_SAVE);
    DoMethod(btn_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             prefswin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(btn_mode, MUIM_Notify, MUIA_Pressed, FALSE,
             app, 2, MUIM_Application_ReturnID, ID_SET_MODE);
    DoMethod(btn_folder, MUIM_Notify, MUIA_Pressed, FALSE,
             app, 2, MUIM_Application_ReturnID, ID_SET_FOLDER);

    set(win, MUIA_Window_Open, TRUE);

    if (!g_have_prefs) {
        say(sub_last_error());
    } else if (!net_start(&g_prefs)) {
        say("cannot start network process");
        g_have_prefs = FALSE;
    } else {
        /* Tonpuffer und Audioprozess. Ohne sie laeuft die Oberflaeche
         * weiter - man kann blaettern und Cover ansehen, nur nicht
         * hoeren. Deshalb ist das hier kein Abbruch, sondern eine
         * Meldung. */
        if (ring_init(&g_rings[0], 1024UL * 1024UL)
                && ring_init(&g_rings[1], 1024UL * 1024UL)) {
            g_ring_ok = TRUE;
        }
        if (!audio_start(g_prefs.ahiunit)) {
            say(audio_last_error());
        }

        /* Die Namensaufloesung ist der einzige Netzschritt ohne
         * Zeitgrenze. Sie laeuft jetzt im Arbeitsprozess, also bleibt
         * das Fenster auch dann bedienbar, wenn der Resolver haengt. */
        say("connecting ...");
        memset(&g_job, 0, sizeof(g_job));
        g_job.op = NJ_RESOLVE;
        net_submit(&g_job);
    }

    if (timer_open()) {
        timer_start();
    }

    while ((id = DoMethod(app, MUIM_Application_NewInput, &sigs))
               != (ULONG)MUIV_Application_ReturnID_Quit) {

        /* HIER, nicht direkt nach dem Aendern eines Kindes: MUIM_Application_
         * NewInput hat gerade alle vorgemerkten Auffrischungen erledigt,
         * also auch das Wegwischen alter Textzeilen. Erst jetzt darf der
         * Verlauf darueber - andersherum blieben graue Balken stehen. */
        switch (id) {
        case ID_TAB: {
            LONG which = 0;

            get(tabbar, MUIA_Tb_Pressed, &which);
            set(tabpages, MUIA_Group_ActivePage, which);
            /* Die eben sichtbar gewordene Seite hat den aktuellen Farbton
             * zwar gesetzt bekommen, aber unsichtbar nicht gezeichnet -
             * dieselbe Falle wie bei show_page(). */
            tint_all();
            break;
        }

        case ID_SIDEBAR: {
            LONG which = -1;

            get(sidebar, MUIA_Sb_Pressed, &which);
            switch (which) {
            case SB_HOME:
                g_sb_home = which;
                view_show(VIEW_HOME);
                break;
            case SB_ALBUMS:
                g_sb_home = which;
                view_show(VIEW_ALBUMS);
                break;
            case SB_TRACKS:
                g_sb_home = which;
                tracks_show();
                break;
            case SB_FAVORITES:
                g_sb_home = which;
                favs_show();
                break;
            case SB_RADIO:
                g_sb_home = which;
                radios_show();
                break;
            case SB_FOLDER_TRACKS:
                g_sb_home = which;
                folder_show();
                break;
            case SB_FOLDER_ALBUMS:
                g_sb_home = which;
                falbums_show();
                break;
            case SB_SCAN:
                /* Kein Seitenwechsel - die Markierung bleibt, wo sie
                 * war, wie bei Settings. */
                rescan();
                set(sidebar, MUIA_Sb_Active, g_sb_home);
                break;
            case SB_SETTINGS:
                /* Ein eigenes Fenster, kein Seitenwechsel - die
                 * Markierung in der Leiste bleibt deshalb stehen, wo
                 * sie war. */
                prefs_open();
                break;
            }
            break;
        }

        case ID_FALBUM_PICK: {
            LONG n = -1;

            get(grid_folder, MUIA_AG_Active, &n);
            if (n >= 0) {
                falbum_open((int)n);
            }
            break;
        }

        case ID_ALBUM_PICK: {
            LONG n = -1;

            get(grid, MUIA_AG_Active, &n);
            if (n >= 0) {
                /* Sofort umschalten, nicht erst wenn die Titel da sind:
                 * so sieht der Anwender unmittelbar, dass sein Klick
                 * angekommen ist. Die Liste fuellt sich dann vor seinen
                 * Augen. */
                show_page(PAGE_PLAYER);
                album_request((int)n);
            }
            break;
        }

        case ID_SET_SAVE:
            prefs_apply();
            break;

        case ID_SET_MODE:
            mode_request();
            break;

        case ID_SET_FOLDER:
            folder_request_path();
            break;

        case ID_RADIO_PICK: {
            LONG n = -1;

            get(lst_radio, MUIA_TL_Active, &n);
            if (n >= 0) {
                radio_play((int)n);
            }
            break;
        }

        case ID_TRACK_PICK:
        case ID_FAV_PICK:
        case ID_FOLDER_PICK: {
            /* Alle drei Listen fuellen die Warteschlange auf dieselbe
             * Weise - ab dem angeklickten Titel, hoechstens QUEUE_MAX
             * Stueck. Bei Folder/Tracks stehen eigene Dateien darin;
             * stream_start() erkennt sie am Pfad. */
            struct SubList *src = (id == ID_FAV_PICK) ? &g_favs
                                : (id == ID_FOLDER_PICK) ? &g_local
                                : &g_tracks;
            Object *lst = (id == ID_FAV_PICK) ? lst_favs
                        : (id == ID_FOLDER_PICK) ? lst_folder
                        : lst_tracks;
            LONG n = -1;

            get(lst, MUIA_TL_Active, &n);
            if (n >= 0) {
                queue_from_list(src, (int)n);
                play_queue(0);
            }
            break;
        }

        case ID_SONG_PICK: {
            LONG n = -1;
            get(lst_queue, MUIA_TL_Active, &n);
            if (n >= 0) {
                play_index((int)n);
            }
            break;
        }

        case ID_PLAYER: {
            LONG which = PLB_NONE;

            get(player, MUIA_Pl_Pressed, &which);
            switch (which) {
            /* ABSPIELEN / ANHALTEN.
             *
             * Gemessen am 6.9.2026 an einem Navidrome-Strom: bei einem
             * NETZSTROM greifen PAUSE, SEEK und ein PLAY nach STOP nicht.
             * Sie werden angenommen (rc=0, "kein Fehler") und tun nichts.
             * Der Knopf HAELT deshalb AN statt zu pausieren, und ein
             * erneuter Druck faengt den Titel von vorn an - ueber
             * amp_jump() auf denselben Eintrag. Die Statuszeile sagt das
             * dazu.
             *
             * Der frueher hier stehende PAUSE-Aufruf war genau deshalb
             * wirkungslos - der Knopf sah tot aus, obwohl der ganze Weg
             * vom Klick bis zum ARexx-Befehl in Ordnung war. */
            case PLB_PLAY:
                /* Jetzt eine ECHTE Pause. Mit AmigaAMP ging das am
                 * Netzstrom nicht: PAUSE wurde angenommen (rc=0) und tat
                 * nichts, der Knopf musste deshalb anhalten und von vorn
                 * anfangen. Der eigene Abspieler haelt an Ort und Stelle
                 * an und laeuft dort weiter (gemessen 20.9.2026). */
                if (g_radio_on) {
                    /* Radio: anhalten und beim naechsten Druck neu
                     * verbinden, live - siehe radio_start(). */
                    if (audio_state() == AU_STOPPED) {
                        if (radio_start()) {
                            set(player, MUIA_Pl_Playing, TRUE);
                        }
                    } else {
                        audio_halt();
                        net_stream_end();
                        set(player, MUIA_Pl_Playing, FALSE);
                    }
                } else if (audio_state() == AU_PLAYING) {
                    audio_pause(TRUE);
                    set(player, MUIA_Pl_Playing, FALSE);
                } else if (audio_state() == AU_PAUSED) {
                    audio_pause(FALSE);
                    set(player, MUIA_Pl_Playing, TRUE);
                } else if (g_qpos >= 0) {
                    play_queue(g_qpos);
                } else if (g_songs.count > 0) {
                    LONG sel = -1;

                    get(lst_queue, MUIA_TL_Active, &sel);
                    if (sel < 0) {
                        sel = 0;
                    }
                    play_index((int)sel);
                }
                break;

            case PLB_STOP:
                audio_halt();
                net_stream_end();
                g_expect_play = FALSE;
                set(player, MUIA_Pl_Playing, FALSE);
                set(player, MUIA_Pl_Pos, 0);
                break;

            /* Vor und zurueck laufen ueber die WARTESCHLANGE, nicht ueber
             * die sichtbare Liste - man darf sich also beim Hoeren in
             * aller Ruhe ein anderes Album ansehen. */
            case PLB_PREV:
                play_queue(g_qpos - 1);
                break;
            case PLB_NEXT:
                play_queue(g_qpos + 1);
                break;
            case PLB_COVER:
                now_goto();
                break;
            case PLB_SHUFFLE:
                say("shuffle is not implemented yet");
                break;
            case PLB_REPEAT:
                say("repeat is not implemented yet");
                break;

            case PLB_SEEK: {
                /* Der Fortschrittsbalken ist jetzt ein BEDIENELEMENT.
                 * Mit AmigaAMP war er nur Anzeige, weil SEEK am
                 * Netzstrom wirkungslos war. Gesprungen wird, indem der
                 * Strom an der passenden Stelle neu geholt wird - der
                 * Server nimmt Range an. */
                LONG secs = 0;

                get(player, MUIA_Pl_SeekTo, &secs);
                if (!g_radio_on && g_qpos >= 0 && secs >= 0) {
                    stream_start(g_qpos, secs * 1000);
                }
                break;
            }
            case PLB_VOLUME: {
                LONG vol = 0;

                get(player, MUIA_Pl_Volume, &vol);
                audio_set_volume((LONG)vol);
                break;
            }
            }
            break;
        }
        }

        /* Hintergrundarbeit VOR dem Warten: solange die Bildwand noch
         * Miniaturen braucht, wird je Durchlauf genau eine erledigt und
         * dann sofort wieder in MUIM_Application_NewInput gegangen. Das
         * Fenster bleibt dabei bedienbar - anders als bei einer Schleife,
         * die alle 51 Cover am Stueck durchrechnet und dafuer zehn
         * Sekunden nicht auf Klicks reagiert. */
        /* Bilder bauen, aber den Sekundentakt NICHT verhungern lassen.
         *
         * Vorher stand hier nur "continue": solange es Miniaturen zu
         * bauen gab, kam die Schleife nie bis zum Wait() - und damit
         * lief tick() nicht, und damit wurde keine weitere Seite der
         * Titelliste geholt. Gemessen: 1311 Titel brauchten statt 20 s
         * ueber zwei Minuten, weil jede Seite auf die naechste Bildpause
         * warten musste.
         *
         * Jetzt wird nach jedem Bild NACHGESEHEN, ob inzwischen ein
         * Signal anliegt - ohne zu warten. Liegt keines an, geht es
         * sofort mit dem naechsten Bild weiter. */
        if (track_covers_step() || radio_covers_step() || thumbs_step()
                || lthumbs_step()) {
            ULONG have = SetSignal(0, 0) & (g_tsig | net_signal());

            if (!have) {
                continue;
            }
            /* Die verbrauchten Bits loeschen - SetSignal(0,0) fragt nur
             * ab. Ohne das stuende das Signal weiter an und tick() liefe
             * in jedem Schleifendurchlauf statt einmal je Sekunde. */
            SetSignal(0, have);
            sigs = have;
        } else if (sigs) {
            sigs = Wait(sigs | SIGBREAKF_CTRL_C | g_tsig | net_signal());
        }

        {
            if (sigs & net_signal()) {
                struct NetJob *j;
                while ((j = net_poll()) != NULL) {
                    job_done(j);
                }
            }
            if (sigs & g_tsig) {
                while (GetMsg(g_tport)) {
                    ;
                }
                g_twait = FALSE;
                tick();
                timer_start();
            }
            if (sigs & SIGBREAKF_CTRL_C) {
                break;
            }
        }
    }

    set(win, MUIA_Window_Open, FALSE);

    /* Erst den Ton anhalten, dann den Netzprozess: der Abspieler liest
     * aus dem Ring, in den der Netzprozess schreibt. */
    audio_halt();
    net_stream_end();
    audio_shutdown();

    /* Der Netzprozess als naechstes: er schreibt in Strukturen, die
     * gleich freigegeben werden, und ihm gehoert AmiSSL. */
    net_stop();

    if (g_ring_ok) {
        ring_free(&g_rings[0]);
        ring_free(&g_rings[1]);
        g_ring_ok = FALSE;
    }
    timer_close();
    /* Erst die Miniaturen abhaengen und freigeben, dann die Objekte
     * wegwerfen - danach gibt es die Bildwand nicht mehr, die darauf
     * zeigt. */
    thumbs_free();
    MUI_DisposeObject(app);
    icon_close();
    vis_cleanup();
    tb_cleanup();
    sb_cleanup();
    ag_cleanup();
    panel_cleanup();
    pl_cleanup();
    tl_cleanup();
    CloseLibrary(MUIMasterBase);

    /* Erst NACH den Fenstern - ein Bildschirm mit offenen Fenstern
     * laesst sich nicht schliessen. */
    screen_close();

    list_free(&g_albums);
    list_free(&g_songs);
    list_free(&g_tracks);
    list_free(&g_favs);
    list_free(&g_radios);
    /* Kein sub_cleanup() hier - bsdsocket und AmiSSL gehoeren dem
     * Netzprozess und werden dort abgeraeumt. */
    return 0;
}
