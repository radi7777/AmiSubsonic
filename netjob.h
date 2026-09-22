/* AmiSubsonic - Netzarbeit in einem eigenen Prozess.
 *
 * WARUM ES DAS GIBT
 *
 * Bis hierher liefen die Abfragen im Task der Oberflaeche. Bei gesundem
 * Netz kostet eine Anfrage gemessen rund 1,4 s (TLS-Handschlag), bei
 * totem Netz laeuft sie in die Zeitgrenzen von 5 bzw. 20 s - und die
 * Namensaufloesung ganz ohne Grenze. Beobachtet: waehrend das Fenster
 * offen stand, brach das WLAN weg; der Amiga lief weiter, aber die
 * Fenster von AmiSubsonic waren tot, weil der Task in gethostbyname()
 * stand und nie in die MUI-Eingabeschleife zurueckkam.
 *
 * Jetzt macht ein eigener Prozess die Netzarbeit. Die Oberflaeche
 * schickt ihm einen Auftrag, nimmt sein Antwortsignal mit in ihr
 * Wait() auf und bleibt derweil am Leben - das Fenster laesst sich
 * bewegen, zeichnet neu und meldet "hole Alben ...", statt tot
 * dazustehen.
 *
 * DIE REGEL, DIE MAN EINHALTEN MUSS
 *
 * Solange ein Auftrag laeuft, ruehrt die Oberflaeche WEDER malloc/free
 * NOCH die Ergebnisstrukturen an. Grund: beide Prozesse teilen sich das
 * Datensegment desselben Programms, also auch den Speicherverwalter von
 * libnix, und der ist nicht gegen gleichzeitigen Zugriff gesichert.
 * Streng abwechselnd - Auftrag hin, Antwort her - ist er es sehr wohl.
 * Deshalb gibt es net_busy(): wer einen zweiten Auftrag schicken will,
 * waehrend einer laeuft, bekommt eine Absage statt eines stillen
 * Fehlers.
 *
 * AmiSSL gehoert vollstaendig dem Arbeitsprozess. Dessen Doku verlangt,
 * dass jeder Prozess selbst initialisiert und aufraeumt; da nur dieser
 * eine Prozess ins Netz geht, ist das genau die richtige Aufteilung.
 */

#ifndef NETJOB_H
#define NETJOB_H

#include <exec/types.h>
#include <exec/ports.h>

#include "amisub.h"

struct Ring;

enum {
    NJ_QUIT = 0,
    NJ_RESOLVE,         /* Rechnernamen aufloesen */
    NJ_PING,            /* Anmeldung pruefen */
    NJ_ALBUMS,          /* Albenliste holen */
    NJ_ALBUM_SONGS,     /* Titel eines Albums */
    NJ_COVER,           /* Cover in eine Datei holen */
    NJ_LYRICS,          /* Liedtext, Server dann lrclib */
    NJ_TRACKS,          /* eine Seite aller Titel (search3, leere Anfrage) */
    NJ_FAVORITES,       /* die markierten Titel (getStarred2) */
    NJ_RADIOS,          /* die Radiostationen des Servers */

    /* Wiedergabe. Diese beiden Auftraege sind SOFORT beantwortet: sie
     * setzen nur den Zustand. Gefuellt wird danach nebenher, zwischen
     * den anderen Auftraegen - deshalb kann waehrend der Wiedergabe
     * weiter geblaettert und nachgeladen werden. */
    NJ_SCAN,            /* eigenes Verzeichnis rekursiv durchsuchen */
    NJ_STREAM,          /* Titel in den Ring holen (id, offset, out_ring) */
    NJ_STREAM_STOP      /* laufenden Strom beenden */
};

struct NetJob {
    struct Message msg;

    int   op;

    /* Eingaben. Feste Felder statt Zeiger, damit nichts an fremdem
     * Speicher haengt. */
    char  id[SUB_ID_LEN];
    char  path[256];            /* NJ_COVER: der Arbeiter schreibt den
                                 * Pfad im Cache hier ZURUECK */
    char  artist[SUB_ARTIST_LEN];
    char  title[SUB_NAME_LEN];
    char  album[SUB_ALBUM_LEN];
    int   duration;
    int   size;                 /* Kantenlaenge fuers Cover, Anzahl Alben */

    /* NJ_ALBUMS: die Art der Liste fuer getAlbumList2 -
     * "alphabeticalByName" fuer Albums, "frequent" fuer Home.
     * Leer heisst alphabetisch. */
    char  listtype[32];

    /* NJ_TRACKS: ab welchem Titel die Seite anfaengt. */
    int   offset;

    /* Ausgaben. Die Strukturen gehoeren dem Auftraggeber; der
     * Arbeitsprozess fuellt sie und fasst sie danach nicht mehr an. */
    struct Ring    *out_ring;   /* NJ_STREAM: dorthin wird gefuellt */
    char            url[512];   /* NJ_STREAM: Radiosender statt Titel */
    struct SubList *out_list;
    struct Album   *out_album;
    char           *out_text;
    int             out_textsize;

    /* NJ_SCAN: was der Durchgang gefunden hat. */
    LONG  scan_dirs, scan_files, scan_cs, scan_tagcs;
    BOOL  scan_full;

    int   rc;                   /* SUB_OK oder ein SUB_E... */
    char  error[200];           /* Meldung, falls rc != SUB_OK */
};

/* Startet den Arbeitsprozess. FALSE, wenn das nicht geht - dann bleibt
 * nur, die Abfragen wie bisher unmittelbar zu erledigen. */
BOOL net_start(struct Prefs *p);
void net_stop(void);

/* Die Signalmaske des Antwortports, fuer das Wait() der Oberflaeche. */
ULONG net_signal(void);

/* Auftrag abschicken. FALSE, wenn schon einer laeuft. */
BOOL net_submit(struct NetJob *job);

/* Laeuft gerade einer? */
BOOL net_busy(void);

/* Stromauftrag abschicken, OHNE auf die Antwort zu warten: der Arbeiter
 * beantwortet ihn sofort, aber er koennte gerade an einem Cover sitzen,
 * und darauf darf die Oberflaeche nicht warten. Die Antwort wird im
 * Sekundentakt mit net_stream_reap() abgeholt. */
BOOL net_submit_stream(struct NetJob *job);
void net_stream_reap(void);
void net_stream_end(void);

/* Zustand des laufenden Stroms, fuer die Anzeige. */
BOOL net_stream_active(void);
long net_stream_pos(void);      /* schon geholte Bytes ab Dateianfang */
long net_stream_total(void);    /* Groesse der Datei, 0 = unbekannt */
LONG net_stream_waited(void);   /* Sekunden, die das Netz weg war */
const char *net_stream_msg(void);   /* was beim Strom zuletzt geschah */
/* Radio: TRUE, wenn der Sender weder MP3 noch AAC liefert. Der
 * Content-Type steht
 * dann in net_stream_ctype(). Beides gilt bis zum naechsten Strom. */
BOOL net_stream_unsupported(void);
const char *net_stream_ctype(void);
/* Radio: laufender Titel aus den ICY-Metadaten ("Interpret - Titel").
 * net_icy_seq() zaehlt jede neue Meldung - die Oberflaeche vergleicht
 * nur die Zahl und holt den Text, wenn sie sich geaendert hat. */
ULONG net_icy_seq(void);
void  net_icy_title(char *out, int size);

/* Fertige Antwort abholen, oder NULL. Nach dem Aufruf ist der Auftrag
 * abgeschlossen und die Ergebnisstrukturen gehoeren wieder dem
 * Auftraggeber. */
struct NetJob *net_poll(void);

#endif
