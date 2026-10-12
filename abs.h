/* AmiSubsonic - Anbindung an Audiobookshelf (Hoerbuecher).
 *
 * Ein zweiter Server neben Navidrome, mit eigenem Zugang (absurl,
 * absuser, abspassword in den Prefs). Er spricht JSON statt XML und
 * meldet sich mit einem Token an statt mit md5(passwort+salt).
 *
 * Das Wichtigste daran ist die HOERPOSITION: Audiobookshelf merkt sie
 * sich je Benutzer und Buch, und jedes Geraet setzt dort fort, wo das
 * letzte aufgehoert hat. abs_get_pos() liest sie, abs_set_pos()
 * schreibt sie zurueck - gemessen am 12.10.2026, siehe AGENTS.md.
 *
 * Alles hier fasst das Netz an und gehoert damit in den Prozess, der
 * das Netz besitzt (CLI: der Fueller, Oberflaeche: AmiSubsonic.net). */

#ifndef ABS_H
#define ABS_H

#include <exec/types.h>
#include "amisub.h"

/* Audiobookshelf vergibt UUIDs, 36 Zeichen - passt in SUB_ID_LEN. */
struct AbsBook {
    char id[SUB_ID_LEN];
    char title[SUB_NAME_LEN];
    char author[SUB_ARTIST_LEN];
    long dur_ms;
    long pos_ms;                /* aus /api/me, 0 = nie gehoert */
    BOOL finished;
    int  ntracks;
};

/* Eine Datei des Buchs. Ein Buch sind oft viele Dateien hintereinander
 * (hier 132 MP3 zu je rund 10 Minuten); start_ms ist die Stelle im
 * ganzen Buch, an der die Datei anfaengt. */
struct AbsTrack {
    char ino[16];               /* Dateinummer fuer /file/<ino> */
    char title[SUB_NAME_LEN];
    long start_ms;
    long dur_ms;
    long bitrate;               /* bit/s, fuer den Sprung in der Datei */
    long size;                  /* Bytes */
    int  index;                 /* Reihenfolge laut Server */
    BOOL exclude;               /* vom Anwender ausgeschlossen */
};

/* Meldet sich an und merkt sich den Token im Speicher. Wird von allen
 * anderen Aufrufen bei Bedarf selbst gerufen, auch nach einem 401
 * (der Token gilt nur eine Stunde). */
int  abs_login(struct Prefs *p);

/* Text zum letzten Fehler - eigener oder der des Netzwegs. */
const char *abs_last_error(void);

/* Rohe Antwort auf ein GET - zum Nachsehen, was der Server wirklich
 * schickt (CLI "abs raw"). Freigeben mit free(). */
int  abs_raw(struct Prefs *p, const char *path, char **resp, long *len);

/* Alle Buecher aller Hoerbuch-Bibliotheken, mit Hoerposition. */
int  abs_books(struct Prefs *p, struct SubList *out);

/* Die Dateien eines Buchs. Die Antwort ist gross (194 KB fuer 132
 * Dateien), gelesen wird sie in einem Durchgang ohne Baum. dur_ms darf
 * NULL sein. */
int  abs_tracks(struct Prefs *p, const char *itemid, struct SubList *out,
                long *dur_ms);

/* Hoerposition eines Buchs. Nie gehoert ist kein Fehler: pos 0. */
int  abs_get_pos(struct Prefs *p, const char *itemid, long *pos_ms,
                 BOOL *finished);

/* Schreibt die Hoerposition zurueck. */
int  abs_set_pos(struct Prefs *p, const char *itemid, long pos_ms,
                 long dur_ms);

/* Oeffnet eine Datei des Buchs ab Byte offset, gelesen wird mit
 * sub_stream_read. */
int  abs_open_file(struct Prefs *p, const char *itemid, const char *ino,
                   long offset, struct SubStream *st);

/* In welcher Datei liegt die Stelle pos_ms? Liefert den Index und die
 * Stelle innerhalb der Datei. Hinter dem Ende: letzte Datei. */
int  abs_find_track(struct SubList *tracks, long pos_ms, long *in_ms);

/* Byte in der Datei fuer eine Stelle in ms - feste Bitrate angenommen,
 * wie bei den eigenen MP3-Dateien. Das ID3-Etikett davor zaehlt der
 * Aufrufer dazu. */
long abs_byte_for_ms(const struct AbsTrack *t, long in_ms);

/* Kennung eines Buchcovers im Cache: "ab-<itemid>". out braucht
 * SUB_ID_LEN Bytes. */
void abs_cover_id(const char *itemid, char *out);

/* Wie sub_get_cover_cached: liegt das Cover schon im Cache, nur den
 * Pfad liefern, sonst als JPEG holen. coverid ist "ab-<itemid>". */
int  abs_cover_cached(struct Prefs *p, const char *coverid, int size,
                      char *out, int outsize);

#endif
