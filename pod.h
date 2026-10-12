/* AmiSubsonic - Podcasts, unabhaengig von jedem Server.
 *
 * Ein Podcast ist ein RSS-Feed: eine XML-Datei mit den Folgen und je
 * einem Link auf die MP3. Abonnieren heisst hier nur: Feed-Adresse
 * merken. Gefunden wird ueber die Suche von Apple (dasselbe Verzeichnis,
 * das Apple Podcasts benutzt) - ohne Schluessel, ohne Anmeldung.
 *
 * Gemessen am 12.10.2026 an fuenf deutschen Podcasts: Feeds 2 bis 8 MB
 * mit 289 bis 565 Folgen, alle MP3 mit fester Bitrate, die MP3 hinter
 * 0 bis 3 Weiterleitungen, Range ueberall (206). Den Feed nur zum Teil
 * per Range zu holen geht NICHT ueberall (Tagesschau schickt immer
 * alles) - deshalb wird beim Lesen mitgezaehlt und nach den neuesten
 * POD_EPISODES Folgen aufgehoert.
 *
 * Ablage unter PROGDIR:Podcasts:
 *   <id>.feed   was der Feed hergab - schreibt NUR der Netzprozess
 *   <id>.state  Stand je Folge        - schreibt NUR, wer abspielt
 * Getrennt, damit ein Scan waehrend der Wiedergabe keine Position
 * ueberschreibt. id ist ein Streuwert der Feed-Adresse. */

#ifndef POD_H
#define POD_H

#include <exec/types.h>
#include "amisub.h"

#define POD_EPISODES  50        /* Wunsch des Anwenders: die neuesten 50 */
#define POD_URL_LEN   512       /* podtrac-Ketten sind lang */
#define POD_DIR       "PROGDIR:Podcasts"

struct PodShow {
    char id[12];                /* 8 Hexziffern */
    char title[SUB_NAME_LEN];
    char author[SUB_ARTIST_LEN];
    char feed[POD_URL_LEN];
    char image[POD_URL_LEN];    /* Cover, 300x300 von Apple */
    int  nnew;                  /* Folgen ohne Stand: neu */
    int  count;
};

struct PodEpisode {
    char  key[12];              /* Streuwert der GUID (oder der URL) */
    char  title[SUB_NAME_LEN];
    char  url[POD_URL_LEN];
    char  date[16];             /* "12 Oct 2026" */
    ULONG when;                 /* sortierbar: Minuten seit Jahr 0 */
    long  dur_s;                /* aus itunes:duration, 0 = unbekannt */
    long  size;                 /* aus enclosure length */
    char  type[24];             /* audio/mpeg ... */
    /* Stand, aus der .state-Datei */
    long  pos_ms;
    BOOL  done;
};

struct PodFeed {
    struct PodShow    show;
    char              etag[80];
    char              lastmod[40];
    int               count;
    struct PodEpisode ep[POD_EPISODES];
};

const char *pod_last_error(void);

/* Apple durchsuchen. out: struct PodShow (ohne id). */
int  pod_search(const char *term, struct SubList *out);

/* Streuwert -> 8 Hexziffern. */
void pod_key(const char *s, char *out);

/* Den Feed holen und mit dem gespeicherten Stand zusammenfuehren. f
 * enthaelt vorher die gespeicherte Fassung (pod_load) oder eine neue
 * show mit feed/title/image. *changed sagt, ob es neue Daten gab (304 =
 * nein). Schreibt <id>.feed. NUR im Netzprozess rufen. */
int  pod_refresh(struct PodFeed *f, BOOL *changed);

/* Gespeicherten Feed samt Stand lesen. id wie in show.id. */
int  pod_load(const char *id, struct PodFeed *f);

/* Stand aller Folgen schreiben (<id>.state). */
int  pod_save_state(const struct PodFeed *f);

/* Alle Abos: liest die Kopfzeilen der .feed-Dateien und zaehlt die
 * neuen Folgen. out: struct PodShow. */
int  pod_list(struct SubList *out);

/* Abo loeschen (beide Dateien). */
int  pod_remove(const char *id);

/* Cover einer Sendung im Cache ("pc-<id>"). Holt es, wenn es fehlt. */
void pod_cover_id(const char *id, char *out);
int  pod_cover_cached(struct Prefs *p, const struct PodShow *s,
                      char *out, int outsize);

#endif
