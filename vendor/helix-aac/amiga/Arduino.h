/* Arduino.h - Ersatz fuer AmiSubsonic (nicht von RealNetworks).
 *
 * Die Helix-Quellen stammen aus ESP8266Audio und binden in aaccommon.h
 * <Arduino.h> und <pgmspace.h> ein. Wir uebersetzen sie mit -DARDUINO,
 * damit buffers.c/sbr.c die normale stdlib nehmen und aacdec.h eine
 * Plattform findet - an den Quellen selbst muss dafuer nichts geaendert
 * werden. Die Rechenroutinen kommen trotzdem aus dem eigenen 68k-Block
 * in assembly.h, der VOR dem ARDUINO-Zweig steht. */
#ifndef AMISUB_ARDUINO_SHIM_H
#define AMISUB_ARDUINO_SHIM_H
#include <stdlib.h>
#include <string.h>
#endif
