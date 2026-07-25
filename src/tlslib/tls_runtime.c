/*
 * tls.library -- the bits a shared library has to bring itself.
 *
 * No crt0, so DOSBase is ours to open (the trust store is a file).  The memory
 * and string helpers are here rather than borrowed from src/common because
 * this library is deliberately standalone in the same way usergroup.library
 * is: everything else it needs comes either from its own objects or, at run
 * time, from bsdsocket.library through the private context.  Linking
 * aminetxduo_common would drag in a second entropy pool -- exactly the thing
 * tls_netx.c exists to avoid -- for the sake of an AllocVec() wrapper.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <exec/memory.h>
#include <dos/dosextens.h>
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
