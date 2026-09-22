/* pgmspace.h - Ersatz fuer AmiSubsonic (nicht von RealNetworks).
 *
 * Auf dem Amiga gibt es keinen getrennten Programmspeicher. PROGMEM
 * legt die Tabellen aber mit Absicht in den DATENbereich: gcc packt
 * "const"-Felder sonst in den Codebereich, und "make check-fpu" haelt
 * deren Bytes fuer FPU-Befehle (gemessen 22.9.2026: 25 Treffer, alle in
 * Tabellen wie noiseTab). Im Datenbereich prueft niemand, und der
 * Pruefer bleibt fuer echten Code scharf, statt Ausnahmen zu lernen. */
#ifndef AMISUB_PGMSPACE_SHIM_H
#define AMISUB_PGMSPACE_SHIM_H
#define PROGMEM __attribute__((section(".data")))
#endif
