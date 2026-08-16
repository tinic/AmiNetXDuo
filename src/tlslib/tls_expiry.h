/*
 * When a cached TLS session has aged out.
 *
 * Split out of tls_resume.c so the rule can be asked without a disk or a
 * clock, see src/tlslib/test/test_tls_expiry.c.  It takes the two numbers it
 * needs rather than a TLSResumeEntry, which is what keeps this file free of
 * the Amiga headers tls_internal.h reaches.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_EXPIRY_H
#define AMINETXDUO_TLS_EXPIRY_H

/*
 * A stamp at or above this is wall-clock seconds.  Below it, the stamp is
 * seconds since boot on a machine with no clock.  The two cannot be compared
 * with each other, and this is what tells them apart.
 */
#define TLS_CLOCK_FLOOR         1767225600UL    /* 2026-01-01 00:00:00 UTC */

/*
 * Ceiling on how long a cached session is offered when the server gave no
 * lifetime hint, and on how far a server's hint is believed.  Cloudflare says
 * 64800 s and badssl says 300.  A client that offers a stale session loses one
 * round trip and gets a full handshake, so this is a comfort limit and not a
 * security boundary.
 */
#define TLS_RESUME_MAX_AGE          86400UL

/*
 * Nonzero when an entry stamped `stamp`, with the server's `lifetime` hint in
 * seconds, has aged out as of `now`.
 *
 * `now` is tls_time_monotonic(): wall time on a machine with a clock, seconds
 * since boot on one without.  It used to be tls_time_now(), which is zero on a
 * machine with no clock, so on those the cap never fired and a master secret
 * written to DEVS:Internet/tlssessions was offered again for as long as the
 * file existed.  A cached master secret is a key on disk, and a key on disk
 * with no expiry is one an attacker who takes the disk can use against
 * captured traffic indefinitely.
 *
 * Both sides must be the same kind of stamp for the subtraction to mean
 * anything, and an uptime from a previous boot cannot be compared with this
 * one.  Where they disagree, or where this boot's uptime has not yet reached
 * the stored one, the entry is from another boot and is treated as expired.
 * That costs one full handshake, which is the outcome either way.
 *
 * A `lifetime` of 0 means the server gave no hint, so the ceiling applies.
 */
int tls_expired(unsigned long stamp, unsigned long lifetime,
                unsigned long now);

#endif /* AMINETXDUO_TLS_EXPIRY_H */
