/* Small bounded string operations shared by the HTTP server.
 * SPDX-License-Identifier: MIT
 */

#include "httpstr.h"

ULONG hs_len(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/* Bounded append, in the shape src/tools/fetch.c builds its request with: one
   `ok = ok && ...` chain, and a single overflow fails the whole thing. */
BOOL hs_append(char *dst, ULONG dstlen, ULONG *used, const char *src)
{
    ULONG n = *used;

    while (*src != '\0')
    {
        if (n + 1UL >= dstlen)
            return FALSE;
        dst[n++] = *src++;
    }

    dst[n] = '\0';
    *used  = n;

    return TRUE;
}

BOOL hs_append_num(char *dst, ULONG dstlen, ULONG *used, ULONG value)
{
    char  text[12];
    ULONG n = 0;

    if (value == 0UL)
    {
        text[n++] = '0';
    }
    else
    {
        char  rev[12];
        ULONG r = 0;

        while (value > 0UL && r < sizeof(rev))
        {
            rev[r++] = (char)('0' + (value % 10UL));
            value /= 10UL;
        }
        while (r > 0UL)
            text[n++] = rev[--r];
    }

    text[n] = '\0';

    return hs_append(dst, dstlen, used, text);
}

/* Case-insensitive compare of `n` characters, so a header name matches
   however the client capitalised it. */
int hs_nicmp(const char *a, const char *b, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
    {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;

        if (ca != cb || ca == 0)
            return ca - cb;
    }

    return 0;
}

BOOL hs_equal(const char *a, const char *b)
{
    ULONG n = hs_len(b);

    return (hs_len(a) == n && hs_nicmp(a, b, n) == 0) ? TRUE : FALSE;
}

VOID hs_copy(char *dst, ULONG dstlen, const char *src)
{
    ULONG n = 0;

    if (dstlen == 0UL)
        return;

    while (src[n] != '\0' && n + 1UL < dstlen)
    {
        dst[n] = src[n];
        n++;
    }

    dst[n] = '\0';
}
