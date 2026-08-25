/*
 * tls.library, what "now" means on a machine that does not know.  Many Amigas
 * have no working RTC, so the certificate validity dates are checked only when
 * the clock is inside the window below; TLSInfo() reports which happened.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <dos/dos.h>
#include <proto/dos.h>

/* Seconds from the UNIX epoch (1970-01-01) to the AmigaOS epoch (1978-01-01):
   eight years, of which 1972 and 1976 are leap, so 8 * 365 + 2 = 2922 days. */
#define TLS_AMIGA_EPOCH         252460800UL

/* TLS_CLOCK_FLOOR is in tls_internal.h: 2026-01-01 00:00:00 UTC, and nothing
   this software runs on is older.  tls_resume.c needs it too, to tell a wall
   clock from an uptime counter. */

/* Fifty years past the floor. */
#define TLS_CLOCK_CEILING       (TLS_CLOCK_FLOOR + (50UL * 31556952UL))

#define TLS_TICKS_PER_SECOND    50UL

/*
 * DateStamp() as it stands, with no floor or ceiling applied.  On a machine
 * without a set clock it counts from boot, which is wrong as a date and still
 * useful as elapsed time; TLS_CLOCK_FLOOR tells the two cases apart.
 */
ULONG tls_time_monotonic(VOID)
{
    struct DateStamp ds;
    ULONG            seconds;

    if (DOSBase == NULL)
        return 0;

    (VOID)DateStamp(&ds);

    if (ds.ds_Days < 0 || ds.ds_Minute < 0 || ds.ds_Tick < 0)
        return 0;

    seconds  = (ULONG)ds.ds_Days * 86400UL;
    seconds += (ULONG)ds.ds_Minute * 60UL;
    seconds += (ULONG)ds.ds_Tick / TLS_TICKS_PER_SECOND;

    return seconds + TLS_AMIGA_EPOCH;
}

BOOL tls_time_is_known(VOID)
{
    ULONG now = tls_time_monotonic();

    return (BOOL)((now >= TLS_CLOCK_FLOOR && now <= TLS_CLOCK_CEILING)
                  ? TRUE : FALSE);
}

/*
 * The callback nx_secure holds.  Zero is NetX Duo's own sentinel: it treats
 * current_time == 0 as "do not check"
 * (nx_secure_x509_certificate_chain_verify.c, `if (current_time != 0)`).
 */
ULONG tls_time_now(VOID)
{
    ULONG now = tls_time_monotonic();

    if (now < TLS_CLOCK_FLOOR || now > TLS_CLOCK_CEILING)
        return 0;

    return now;
}
