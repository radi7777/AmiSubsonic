/* Inline-Header fuer mpega.library, geschrieben gegen die mitgelieferte
 * FD-Datei (bias 30) mit den LP*-Makros der Toolchain - derselbe Weg wie
 * beim vendorierten muimaster.h. Das Paket bringt nur Pragmas fuer SAS/C
 * und StormC mit, mit denen gcc nichts anfangen kann.
 *
 * Die LVOs stammen aus mpega.fd, sind also NICHT geraten:
 *   ##bias 30, Reihenfolge open, close, decode_frame, seek, time,
 *   find_sync, scale. */

#ifndef _INLINE_MPEGA_H
#define _INLINE_MPEGA_H

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif

#ifndef LIBRARIES_MPEGA_H
#include <libraries/mpega.h>
#endif

#ifndef MPEGA_BASE_NAME
#define MPEGA_BASE_NAME MPEGABase
#endif

extern struct Library *MPEGABase;

#define MPEGA_open(___filename, ___ctrl) \
    LP2(0x1e, MPEGA_STREAM *, MPEGA_open, char *, ___filename, a0, \
        MPEGA_CTRL *, ___ctrl, a1, \
    , MPEGA_BASE_NAME)

#define MPEGA_close(___mpds) \
    LP1NR(0x24, MPEGA_close, MPEGA_STREAM *, ___mpds, a0, \
    , MPEGA_BASE_NAME)

#define MPEGA_decode_frame(___mpds, ___pcm) \
    LP2(0x2a, LONG, MPEGA_decode_frame, MPEGA_STREAM *, ___mpds, a0, \
        WORD **, ___pcm, a1, \
    , MPEGA_BASE_NAME)

#define MPEGA_seek(___mpds, ___ms) \
    LP2(0x30, LONG, MPEGA_seek, MPEGA_STREAM *, ___mpds, a0, \
        LONG, ___ms, d0, \
    , MPEGA_BASE_NAME)

#define MPEGA_time(___mpds, ___ms) \
    LP2(0x36, LONG, MPEGA_time, MPEGA_STREAM *, ___mpds, a0, \
        LONG *, ___ms, a1, \
    , MPEGA_BASE_NAME)

#define MPEGA_find_sync(___buffer, ___size) \
    LP2(0x3c, LONG, MPEGA_find_sync, UBYTE *, ___buffer, a0, \
        LONG, ___size, d0, \
    , MPEGA_BASE_NAME)

#define MPEGA_scale(___mpds, ___percent) \
    LP2(0x42, LONG, MPEGA_scale, MPEGA_STREAM *, ___mpds, a0, \
        LONG, ___percent, d0, \
    , MPEGA_BASE_NAME)

#endif
