/*
 * tls.library, what src/common/ami_random.c reaches for that only lives in
 * src/common/compat.c.
 *
 * The entropy pool used to be bsdsocket.library's, borrowed over the private
 * LVO; that is what made tls.library refuse to run on any other stack.  It is
 * this library's own now, which means ami_random.c is linked here -- and it
 * expects two things from compat.c that this library does not link:
 *
 *   TimerBase     the conventional name <proto/timer.h> resolves ReadEClock()
 *                 and GetSysTime() through.  src/tls/tls_amiga.c already owns
 *                 an E-Clock unit for the crypto timings and renames its base
 *                 to ami_tls_timer_base for exactly this reason, so there is
 *                 one open device and two names for it.
 *   ami_millis()  ami_random.c calls it once, for the side effect of getting
 *                 timer.device open before it samples.  The value is not read.
 *
 * TWO ENTROPY POOLS IN ONE MACHINE IS THE POINT, not an accident: on Roadshow
 * or AmiTCP there is no other one to share.  The pool's arrival source is fed
 * from tls_netx.c's receive path, where a record off the wire stands in for
 * the SANA-II frame timing bsdsocket.library's copy is fed with.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include "tls.h"                /* ami_tls_timer_open(), ami_tls_eclock() */

#include <devices/timer.h>

struct Device *TimerBase;

ULONG ami_millis(VOID)
{
    if (TimerBase == NULL)
    {
        if (!ami_tls_timer_open())
            return 0;

        TimerBase = ami_tls_timer_base;
    }

    /*
     * E-Clock ticks converted once, not accumulated: the only caller is
     * ami_random.c's "make sure the device is open", and nothing in this
     * library times anything through this function.
     */
    return ami_tls_eclock_micros(ami_tls_eclock()) / 1000UL;
}
