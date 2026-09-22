/* AmiSubsonic - Groesse des Helix-Zustands.
 *
 * Eigene Datei (MIT, nicht RealNetworks), aber mit den Flags und
 * Headern von Helix uebersetzt, siehe Makefile: nur hier sind die
 * inneren Strukturen bekannt. Der Audioprozess braucht die Zahl, weil
 * er den Dekoder mit AACInitDecoderPre in EIGENEM Speicher anlegt -
 * AACInitDecoder nimmt malloc, und libnix' Speicherverwalter darf der
 * Audioprozess nicht anfassen, solange ein Netzauftrag laeuft (siehe
 * netjob.h). */

#include "coder.h"

/* sbr.h und coder.h definieren beide HuffInfo und lassen sich nicht in
 * EINER Datei einbinden - der SBR-Teil steht deshalb in aacsize_sbr.c. */
extern long aac_sbr_size(void);

/* Dieselbe Rechnung wie AllocateBuffersPre (buffers.c) und InitSBRPre
 * (sbr.c): die beiden ersten Bloecke auf 8 Bytes gerundet, der
 * SBR-Zustand dahinter ungerundet. */
long aac_state_size(void)
{
    return (long)(((sizeof(AACDecInfo) + 7) & ~7)
                + ((sizeof(PSInfoBase) + 7) & ~7))
         + aac_sbr_size();
}
