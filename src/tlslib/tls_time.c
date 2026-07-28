/*
 * tls.library -- what "now" means on a machine that does not know.
 *
 *   A certificate carries notBefore and notAfter, and checking them needs a
 *   clock.  A great many Amigas do not have one: no battery-backed RTC, or a
 *   dead battery, and AmigaOS then starts at its epoch, 1 January 1978.
 *   tests/tls/tls_https saw exactly this -- `tv_secs == 0`.  Every certificate
 *   on the internet was issued after 1978, so on such a machine EVERY
 *   certificate fails notBefore and every HTTPS connection fails.
 *
 *   Refuse.  Correct, and it makes the library useless on the hardware it was
 *   written for.  A user with a flat battery gets "certificate not yet valid"
 *   from every site and no way to work out why.
 *
 *   Check anyway and let it fail.  The same thing without the honesty.
 *
 *   Skip the validity dates when the clock is obviously unset, check them when
 *   it is not, and say which happened.  That is what this does.
 *
 *   Not much, and it is worth being exact rather than reassuring.  Expiry
 *   checking does not stop an attacker from impersonating a site -- the
 *   signature chain to a trusted root and the host name check do that, and
 *   both still run.  What expiry adds is a bound on how long a certificate
 *   whose private key has leaked stays useful.  An attacker who has stolen a
 *   key AND can get between this Amiga and the site can use it indefinitely
 *   against a machine with no clock.  That is a real weakening.  It is also
 *   the same weakening every device with a dead RTC has, and this stack
 *   already has no revocation checking of any kind (no OCSP, no CRL fetch), so
 *   the stolen-key case was never covered anyway.
 *
 *   The alternative on offer is a machine that cannot reach any HTTPS site at
 *   all.  Set your clock -- and the library tells you when you have not, in
 *   TLSInfo()'s ti_ExpiryChecked, so a program that cares can say so.
 *
 * WHAT COUNTS AS "OBVIOUSLY UNSET"
 *
 *   Anything outside a fifty-year window starting before this software was
 *   written.  Below the floor covers the 1978 case and every partially-set
 *   clock; above the ceiling covers the machine whose clock was typed in wrong
 *   and now reads 2145, which would otherwise reject every valid certificate
 *   as expired and look identical to a real failure.
 *
 *   The floor is a constant rather than __DATE__ on purpose: a build-date
 *   check would make the binary non-reproducible and would make an old build
 *   behave differently from a new one on the same machine, which is the last
 *   thing anybody debugging a certificate problem needs.
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

/* 2026-01-01 00:00:00 UTC.  Nothing this software runs on is older. */
#define TLS_CLOCK_FLOOR         1767225600UL

/* Fifty years past the floor. */
#define TLS_CLOCK_CEILING       (TLS_CLOCK_FLOOR + (50UL * 31556952UL))

#define TLS_TICKS_PER_SECOND    50UL

static ULONG tls_time_read(VOID)
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
    ULONG now = tls_time_read();

    return (BOOL)((now >= TLS_CLOCK_FLOOR && now <= TLS_CLOCK_CEILING)
                  ? TRUE : FALSE);
}

/*
 * The callback nx_secure holds.  Zero is not a sentinel we invented: NetX Duo
 * itself treats current_time == 0 as "do not check"
 * (nx_secure_x509_certificate_chain_verify.c, `if (current_time != 0)`), which
 * is why this needs no vendored change and no extra flag.
 */
ULONG tls_time_now(VOID)
{
    ULONG now = tls_time_read();

    if (now < TLS_CLOCK_FLOOR || now > TLS_CLOCK_CEILING)
        return 0;

    return now;
}
