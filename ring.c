/* AmiSubsonic - Ringpuffer zwischen Netz und Dekoder, siehe ring.h. */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <string.h>

#include "ring.h"

BOOL ring_init(struct Ring *r, ULONG size)
{
    memset(r, 0, sizeof(*r));
    r->buf = AllocVec(size, MEMF_PUBLIC);
    if (!r->buf) {
        return FALSE;
    }
    r->size = size;
    return TRUE;
}

void ring_free(struct Ring *r)
{
    if (r->buf) {
        FreeVec(r->buf);
        r->buf = NULL;
    }
    r->size = 0;
}

void ring_reset(struct Ring *r)
{
    r->wr = 0;
    r->rd = 0;
    r->eof = FALSE;
    r->stop = FALSE;
    r->filled = 0;
    r->fmt = RING_MPEG;
}

/* Ein Platz bleibt immer frei: sonst waere "ganz voll" nicht von "ganz
 * leer" zu unterscheiden, beides ist wr == rd. */
ULONG ring_used(struct Ring *r)
{
    ULONG wr = r->wr, rd = r->rd;

    return (wr >= rd) ? (wr - rd) : (r->size - rd + wr);
}

ULONG ring_space(struct Ring *r)
{
    return r->size - 1 - ring_used(r);
}

ULONG ring_write(struct Ring *r, const UBYTE *src, ULONG len)
{
    ULONG space = ring_space(r);
    ULONG wr = r->wr;
    ULONG first;

    if (len > space) {
        len = space;
    }
    if (len == 0) {
        return 0;
    }

    /* Bis zum Ende des Feldes, dann von vorn. */
    first = r->size - wr;
    if (first > len) {
        first = len;
    }
    memcpy(r->buf + wr, src, first);
    if (len > first) {
        memcpy(r->buf, src + first, len - first);
    }

    r->wr = (wr + len) % r->size;
    r->filled += len;
    return len;
}

ULONG ring_read(struct Ring *r, UBYTE *dst, ULONG len)
{
    ULONG used = ring_used(r);
    ULONG rd = r->rd;
    ULONG first;

    if (len > used) {
        len = used;
    }
    if (len == 0) {
        return 0;
    }

    first = r->size - rd;
    if (first > len) {
        first = len;
    }
    memcpy(dst, r->buf + rd, first);
    if (len > first) {
        memcpy(dst + first, r->buf, len - first);
    }

    r->rd = (rd + len) % r->size;
    return len;
}
