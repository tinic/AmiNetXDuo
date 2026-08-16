/*
 * tls.library, what "now" means on a machine that does not know.
 *
 *   A certificate carries notBefore and notAfter, and a check of them needs a
 *   clock.  A great many Amigas do not have one: no battery-backed RTC, or a
 *   dead battery, and AmigaOS then starts at its epoch, 1 January 1978.
 *   tests/tls/tls_https saw this, `tv_secs == 0`.  Every certificate on the
 *   internet was issued after 1978, so on such a machine every certificate
 *   fails notBefore and every HTTPS connection fails.
 *
 *   A refusal to connect is correct, and it makes the library unusable on the
 *   hardware it was written for.  A user with a flat battery gets "certificate
 *   not yet valid" from every site and no way to work out why.  A check that
 *   runs anyway and fails is the same outcome.
 *
 *   The library therefore skips the validity dates when the clock is obviously
 *   unset, checks them when it is not, and reports which happened.
 *
 *   The expiry check does not stop an attacker from impersonating a site.  The
 *   signature chain to a trusted root and the host name check do that, and
 *   both still run.  Expiry bounds how long a certificate whose private key
 *   has leaked stays useful.  An attacker who stole a key and can get between
 *   this Amiga and the site can use it indefinitely against a machine with no
 *   clock.  That is a real weakening, the same one every device with a dead
 *   RTC has, and this stack has no revocation check of any kind (no OCSP, no
 *   CRL fetch), so the stolen-key case was never covered.
 *
 *   The alternative is a machine that cannot reach any HTTPS site.  The
 *   library reports an unset clock in TLSInfo()'s ti_ExpiryChecked, so a
 *   program that cares can say so.
 *
 *   "Obviously unset" means anything outside a fifty-year window starting
 *   before this software was written.  Below the floor covers the 1978 case
 *   and every partially-set clock.  Above the ceiling covers the machine whose
 *   clock was typed in wrong and now reads 2145, which otherwise rejects every
 *   valid certificate as expired and looks identical to a real failure.
 *
 *   The floor is a constant rather than __DATE__.  A build-date check makes
 *   the binary non-reproducible, and makes an old build behave differently
 *   from a new one on the same machine.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <dos/dos.h>
#include <proto/dos.h>

/*
 * Seconds from the UNIX epoch (1970-01-01) to the AmigaOS epoch (1978-01-01):
 * eight years, of which 1972 and 1976 are leap, so 8 * 365 + 2 = 2922 days.
 */
#define TLS_AMIGA_EPOCH         252460800UL

/* TLS_CLOCK_FLOOR is in tls_internal.h: 2026-01-01 00:00:00 UTC, and nothing
   this software runs on is older.  tls_resume.c needs it too, to tell a wall
   clock from an uptime counter. */

/* Fifty years past the floor. */
#define TLS_CLOCK_CEILING       (TLS_CLOCK_FLOOR + (50UL * 31556952UL))

#define TLS_TICKS_PER_SECOND    50UL

/*
 * DateStamp() as it stands, with no floor or ceiling applied.
 *
 * On a machine with a set clock this is wall time.  On one without it starts
 * at the AmigaOS epoch and counts up from boot, which is wrong as a date and
 * still useful as elapsed time.  tls_resume.c ages cached sessions against it,
 * and TLS_CLOCK_FLOOR is what tells the two cases apart.
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
 * (nx_secure_x509_certificate_chain_verify.c, `if (current_time != 0)`), so
 * this needs no vendored change and no extra flag.
 */
ULONG tls_time_now(VOID)
{
    ULONG now = tls_time_monotonic();

    if (now < TLS_CLOCK_FLOOR || now > TLS_CLOCK_CEILING)
        return 0;

    return now;
}
