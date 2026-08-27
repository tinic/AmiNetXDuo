/*
 * tls.library, the parts a shared library must bring itself.
 *
 * No crt0, so this library opens DOSBase itself (the trust store is a file).
 * The memory and string helpers are here rather than borrowed from src/common
 * because this library is standalone in the same way usergroup.library is.
 * Everything else it needs comes from its own objects; the only thing it asks
 * the machine's bsdsocket.library for is send(), recv() and WaitSelect(), at
 * the published vectors (tls_sock.c).
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include "tls.h"                /* ami_tls_timer_close() */

#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

struct DosLibrary *DOSBase;

BOOL tls_runtime_open(VOID)
{
    if (DOSBase == NULL)
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);

    return (BOOL)((DOSBase != NULL) ? TRUE : FALSE);
}

VOID tls_runtime_close(VOID)
{
    /* The E-Clock timerequest ami_tls_timer_open() opened is a file-scope
       static in this segment, and expunge is about to UnLoadSeg() it. */
    ami_tls_timer_close();

    if (DOSBase != NULL)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

APTR tls_alloc(ULONG size)
{
    if (size == 0)
        return NULL;

    return AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
}

VOID tls_free(APTR ptr)
{
    if (ptr != NULL)
        FreeVec(ptr);
}

VOID tls_bzero(APTR ptr, ULONG size)
{
    UBYTE *p = (UBYTE *)ptr;

    while (size-- > 0)
        *p++ = 0;
}

VOID tls_memcpy(APTR dst, const void *src, ULONG size)
{
    UBYTE       *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;

    while (size-- > 0)
        *d++ = *s++;
}

/*
 * NetX Duo counts in NX_IP_PERIODIC_RATE ticks and Delay() counts in fiftieths
 * of a second.  Rounded up, so a caller that asked to wait always does.
 */
VOID tls_delay_ticks(ULONG ticks)
{
    ULONG fiftieths;

    if (ticks == 0)
        return;

    fiftieths = (ticks * 50UL + (NX_IP_PERIODIC_RATE - 1UL)) / NX_IP_PERIODIC_RATE;
    if (fiftieths == 0)
        fiftieths = 1;

    Delay((LONG)fiftieths);
}

ULONG tls_strlen(const char *s)
{
    const char *p = s;

    if (s == NULL)
        return 0;
    while (*p != '\0')
        p++;

    return (ULONG)(p - s);
}

VOID tls_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i;

    if (dst == NULL || size == 0)
        return;

    for (i = 0; i + 1 < size && src != NULL && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}
