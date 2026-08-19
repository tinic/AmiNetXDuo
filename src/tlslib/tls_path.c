/*
 * tls.library, checked caller-supplied filesystem paths.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"


LONG tls_path_set(char *dst, ULONG size, CONST_STRPTR path)
{
    ULONG length;
    ULONG i;

    if (dst == NULL || size == 0 || path == NULL)
        return TLS_ERR_INTERNAL;

    /* Scan only as far as the destination can represent.  If there is no NUL
       in that range, copying would turn the caller's path into another file
       name, which is unsafe for both roots and cached master secrets. */
    for (length = 0; length < size && path[length] != '\0'; length++)
        ;

    if (length == size)
        return TLS_ERR_BADPATH;

    for (i = 0; i <= length; i++)
        dst[i] = path[i];

    return TLS_OK;
}
