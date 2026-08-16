/*
 * tls_expiry, when a cached TLS session has aged out.  See tls_expiry.h for
 * the rule and why it is its own file.
 *
 * Includes nothing but its own header, deliberately.  This translation unit is
 * compiled twice, once for m68k as part of tls.library and once natively by
 * src/tlslib/test/test_tls_expiry.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_expiry.h"

int tls_expired(unsigned long stamp, unsigned long lifetime,
                unsigned long now)
{
    unsigned long age;
    unsigned long limit;

    /* One is a wall clock and the other an uptime: not comparable. */
    if ((now >= TLS_CLOCK_FLOOR) != (stamp >= TLS_CLOCK_FLOOR))
        return 1;

    /* Stamped after the time being asked about, so a previous boot's uptime. */
    if (now < stamp)
        return 1;

    age   = now - stamp;
    limit = (lifetime != 0UL && lifetime < TLS_RESUME_MAX_AGE)
            ? lifetime : TLS_RESUME_MAX_AGE;

    return (age > limit) ? 1 : 0;
}
