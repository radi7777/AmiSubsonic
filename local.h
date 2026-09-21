/* AmiSubsonic - eigene MP3-Dateien auf der Platte.
 *
 * Zweite Quelle neben dem Server. Gefunden wird rekursiv, gelesen werden
 * die ID3-Etiketten; gespielt wird ueber denselben Ringpuffer und
 * denselben Audioprozess wie ein Titel vom Server - nur gefuellt wird
 * aus einer Datei statt aus einer TLS-Verbindung. */

#ifndef LOCAL_H
#define LOCAL_H

#include <exec/types.h>
#include "amisub.h"

/* Obergrenze, damit ein falsch gewaehltes Wurzelverzeichnis (SYS: oder
 * gar DH0:) nicht minutenlang laeuft und den Speicher fuellt. */
#define LOCAL_MAX_FILES  4000

struct ScanStat {
    LONG dirs;          /* besuchte Verzeichnisse */
    LONG files;         /* gefundene MP3-Dateien */
    LONG skipped;       /* andere Dateien */
    LONG cs;            /* Suchen, in Hundertstelsekunden */
    LONG tag_cs;        /* Etiketten lesen, in Hundertstelsekunden */
    BOOL full;          /* Obergrenze erreicht */
};

/* Liest die Angaben aus einer Datei: ID3v2 am Anfang, sonst ID3v1 am
 * Ende, dazu die Spieldauer aus dem ersten MPEG-Rahmenkopf (bei
 * wechselnder Bitrate aus dem Xing-Tag). Fuellt title, artist, album,
 * year, track, duration und bitrate. FALSE heisst: keine brauchbare
 * MP3-Datei. */
BOOL local_tags(struct Song *s);

/* Durchsucht root rekursiv nach *.mp3 und haengt jede Datei als Titel an
 * out an. Gefuellt werden vorerst nur path und ein vorlaeufiger Titel
 * aus dem Dateinamen; die Etiketten kommen in der naechsten Stufe. */
int local_scan(const char *root, struct SubList *out, struct ScanStat *st);

/* Fasst die Titelliste zu Alben zusammen. songs MUSS dafuer nach Album
 * und Titelnummer sortiert sein - local_group() sortiert selbst.
 * Zusammengehoerig ist, was dasselbe Album-Etikett UND dasselbe
 * Verzeichnis hat; ohne Etikett gilt der Verzeichnisname. */
int local_group(struct SubList *songs, struct SubList *albums);

/* Wo das Cover eines eigenen Albums liegt. Zuerst folder.jpg und Co. im
 * Verzeichnis; sonst wird das eingebettete Bild aus dem ersten Titel in
 * den Cover-Cache geschrieben und DESSEN Pfad zurueckgegeben. */
BOOL local_cover(struct Album *al, struct Song *first,
                 const char *cachedir, char *out, int outsize);

const char *local_last_error(void);

#endif
