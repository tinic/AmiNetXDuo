/*
 * AmiNetXDuo -- crypto68k test support.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"

#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define C68K_LOG_SIZE   16384

static char     c68k_log_buffer[C68K_LOG_SIZE];
static ULONG    c68k_log_used;

static VOID c68k_put(UBYTE c)
{

    RawPutChar(c);

    if (c68k_log_used < (ULONG)(C68K_LOG_SIZE - 1))
    {
        c68k_log_buffer[c68k_log_used++] = (char)c;
    }
}

static VOID c68k_put_char(register UBYTE c      __asm("d0"),
                          register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        c68k_put(c);
    }
}

VOID c68k_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) c68k_put_char, NULL);
    va_end(args);

    c68k_put('\n');
}

VOID c68k_flush(VOID)
{

BPTR    out;


    out = Output();
    if (out != (BPTR)0)
    {
        (VOID) Write(out, (APTR)c68k_log_buffer, (LONG)c68k_log_used);
    }
}


/* ----------------------------------------------------------------- RNG --- */

static ULONG    c68k_rng_state = 0x2026072AUL;

VOID c68k_rng_seed(ULONG seed)
{

    c68k_rng_state = (seed != 0) ? seed : 0x2026072AUL;
}

c68k_limb c68k_rand(VOID)
{

ULONG   x = c68k_rng_state;


    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c68k_rng_state = x;

    return((c68k_limb)x);
}

VOID c68k_rand_limbs(c68k_limb *p, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        p[i] = c68k_rand();
    }

    if (n > 0)
    {
        /*
         * The top limb must be nonzero or the value is not really n limbs
         * wide, and _nx_crypto_huge_number_adjust_size would shorten it behind
         * our back -- which would silently stop testing the width we asked for.
         */
        while (p[n - 1] == 0)
        {
            p[n - 1] = c68k_rand();
        }
    }
}


/* --------------------------------------------------------- huge numbers --- */

VOID c68k_hn_set(NX_CRYPTO_HUGE_NUMBER *hn, c68k_limb *data,
                 UINT used_limbs, UINT buffer_limbs)
{

    hn -> nx_crypto_huge_number_data        = data;
    hn -> nx_crypto_huge_number_size        = used_limbs;
    hn -> nx_crypto_huge_buffer_size        = buffer_limbs << 2;
    hn -> nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
}

UINT c68k_hn_equals(const NX_CRYPTO_HUGE_NUMBER *hn, const c68k_limb *v,
                    UINT n)
{

UINT                i;
UINT                size = hn -> nx_crypto_huge_number_size;
const c68k_limb    *d    = hn -> nx_crypto_huge_number_data;


    /*
     * The vendored routines return a size-adjusted number and this module
     * returns a fixed m_len limbs, so the comparison has to be on values, not
     * on buffers: every limb above the shorter length must be zero.
     */
    for (i = 0; (i < n) || (i < size); i++)
    {
        c68k_limb   a = (i < size) ? d[i] : 0u;
        c68k_limb   b = (i < n) ? v[i] : 0u;

        if (a != b)
        {
            return(0u);
        }
    }

    return(1u);
}
