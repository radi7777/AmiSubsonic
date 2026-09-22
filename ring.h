/* AmiSubsonic - Ringpuffer zwischen Netz und Dekoder.
 *
 * Genau EIN Schreiber (der Prozess, der aus dem Netz liest) und genau
 * EIN Leser (der Dekoder). Das ist der Grund, warum hier keine
 * Semaphore noetig ist: wr gehoert dem Schreiber, rd dem Leser, und
 * beide lesen den Wert des anderen nur. Auf dem 68k ist ein ULONG
 * unteilbar, ein halb geschriebener Wert kann also nicht gelesen werden.
 *
 * Der Speicher kommt von AllocVec, nicht von malloc: beide Prozesse
 * fassen ihn an, und libnix' Speicherverwalter ist nicht gegen
 * gleichzeitigen Zugriff gesichert (siehe netjob.h). */

#ifndef RING_H
#define RING_H

#include <exec/types.h>

struct Ring {
    UBYTE *buf;
    ULONG  size;
    volatile ULONG wr;      /* nur der Schreiber aendert das */
    volatile ULONG rd;      /* nur der Leser aendert das */
    volatile BOOL  eof;     /* Schreiber ist fertig */
    volatile BOOL  stop;    /* Leser will nicht mehr */
    ULONG  filled;          /* Gesamtzahl geschriebener Bytes, fuer Statistik */
    /* Was im Ring steckt. Der Schreiber setzt es VOR dem ersten Byte,
     * der Leser liest es erst, wenn Daten da sind - so ist es nie
     * halb gueltig. ring_reset stellt MPEG ein: Titel vom Server und
     * eigene Dateien sind immer MP3, nur ein Sender kann anders. */
    volatile LONG  fmt;
};

#define RING_MPEG  0
#define RING_AAC   1        /* ADTS, wie Radiosender es senden */

BOOL  ring_init(struct Ring *r, ULONG size);
void  ring_free(struct Ring *r);
void  ring_reset(struct Ring *r);

ULONG ring_used(struct Ring *r);
ULONG ring_space(struct Ring *r);

/* Schreibt so viel wie hineinpasst, ohne zu warten. Rueckgabe: Anzahl. */
ULONG ring_write(struct Ring *r, const UBYTE *src, ULONG len);

/* Liest so viel wie da ist, ohne zu warten. Rueckgabe: Anzahl. */
ULONG ring_read(struct Ring *r, UBYTE *dst, ULONG len);

#endif
