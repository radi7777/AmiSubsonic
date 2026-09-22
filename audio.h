/* AmiSubsonic - eigener Abspieler (im Aufbau).
 *
 * Stufe 0 misst nur, wie schnell mpega.library dekodiert. AHI-Ausgabe,
 * Ringpuffer und Netzstrom kommen in den naechsten Stufen dazu. */

#ifndef AUDIO_H
#define AUDIO_H

#include <exec/types.h>

struct AudioProbe {
    LONG  freq;         /* Abtastrate der Ausgabe in Hz */
    LONG  channels;     /* 1 oder 2 */
    LONG  bitrate;      /* kbps laut Dateikopf */
    LONG  layer;        /* 1..3 */
    ULONG ms_total;     /* Spieldauer laut Datei */
    ULONG frames;       /* dekodierte Bloecke */
    ULONG samples;      /* dekodierte Abtastwerte je Kanal */
    ULONG cs_decode;    /* gebrauchte Zeit in Hundertstelsekunden */
};

struct Ring;

const char *audio_last_error(void);

/* ------------------------------------------------------------------ */
/* Der Audioprozess                                                    */
/* ------------------------------------------------------------------ */

/* Zustaende. Wer fragt, liest audio_state(). */
#define AU_STOPPED  0
#define AU_PLAYING  1
#define AU_PAUSED   2

/* Startet den Prozess "AmiSubsonic.audio". Er oeffnet ahi.device EINMAL
 * und haelt es offen - das Oeffnen kostet 0,12 s, und die will niemand
 * bei jedem Titelwechsel hoeren (gemessen 20.9.2026).
 *
 * unit ist die AHI-Unit aus den Einstellungen, ueblicherweise 0. */
BOOL audio_start(LONG unit);
void audio_shutdown(void);

/* Spielt, was in den Ring geschrieben wird. base_ms ist die Stelle im
 * Titel, an der der Ring anfaengt - nach einem Sprung also nicht 0.
 * Kehrt sofort zurueck; gespielt wird im Audioprozess. */
void audio_play(struct Ring *r, LONG base_ms);

void audio_pause(BOOL on);
void audio_halt(void);              /* laufenden Titel abbrechen */
void audio_set_volume(LONG vol);    /* 0..64, wie in der Bedienleiste */

LONG audio_state(void);
LONG audio_pos_ms(void);            /* Position im laufenden Titel */
BOOL audio_track_done(void);        /* Titel ist zu Ende gelaufen */
BOOL audio_error(void);             /* Strom war unlesbar (loescht sich) */
void audio_clear_done(void);

/* Visualizer: n Abtastwerte (mono, 16 Bit) an der Stelle, die gerade
 * aus dem Lautsprecher kommt. FALSE, wenn nichts laeuft (Stop, Pause,
 * Ende) - dann soll die Anzeige abklingen. Fasst keinen Speicher an,
 * darf also jederzeit aus der Oberflaeche gerufen werden. */
BOOL audio_vis_window(WORD *mono, LONG n);

/* Dekodiert eine Datei vollstaendig, ohne sie auszugeben, und misst die
 * Zeit. Kein AHI, kein Netz - nur der Dekoder. */
BOOL audio_probe_file(const char *path, struct AudioProbe *out);

/* Spielt eine Datei ueber AHI ab (Unit wie in den AHI-Voreinstellungen,
 * ueblicherweise 0). Kehrt erst am Ende des Titels zurueck oder bei
 * Strg-C. cs_decode enthaelt danach die verbrauchte Zeit - bei
 * Wiedergabe ist das die Spieldauer, nicht die Rechenzeit. */
BOOL audio_play_file(const char *path, LONG unit, struct AudioProbe *out);

/* Spielt, was ein anderer Prozess in den Ring schreibt. Endet, wenn der
 * Ring leer UND als beendet markiert ist, oder bei Strg-C. */
BOOL audio_play_ring(struct Ring *r, LONG unit, struct AudioProbe *out);

#endif
