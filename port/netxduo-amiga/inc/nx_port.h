/***************************************************************************
 * Eclipse ThreadX NetX Duo, AmigaOS / m68k port.
 *
 * Derived from ports/linux/gnu/inc/nx_port.h
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2025-present Eclipse ThreadX Contributors
 *
 * Changes from the Linux original:
 *   - NX_LITTLE_ENDIAN is not defined.  m68k is big-endian, which is also the
 *     network byte order, so NX_CHANGE_ULONG_ENDIAN/NX_CHANGE_USHORT_ENDIAN
 *     and htons/htonl/ntohs/ntohl all compile away to nothing, removing every
 *     header byte-swap in the stack (docs/RESEARCH.md 5.3).
 *   - <stdio.h> and <stdlib.h> are not pulled in.  A shared library build
 *     must not drag newlib's stdio along; the core needs memset/memcpy/memcmp
 *     only, which we supply.
 *   - nx_user.h is included unconditionally rather than behind
 *     NX_INCLUDE_USER_DEFINE_FILE, so the 4 MB tuning always applies whatever
 *     the build system passes.
 *
 * This program and the accompanying materials are made available under the
 * terms of the MIT License which is available at
 * https://opensource.org/licenses/MIT.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*  PORT SPECIFIC C INFORMATION                                           */
/*                                                                        */
/*    nx_port.h                                        AmigaOS/m68k       */
/*                                                                        */
/**************************************************************************/

#ifndef NX_PORT_H
#define NX_PORT_H


/* Always take the AmiNetXDuo tuning.  */

#include "nx_user.h"


#include <string.h>


/* Big-endian target: NX_LITTLE_ENDIAN left undefined.  */


/*
 * Random numbers.
 *
 * Left undefined, nx_api.h falls back to NX_RAND == rand(), which on this
 * toolchain is newlib's 32-bit LCG, and NX_RAND generates ECDHE private keys
 * and the TLS client random in nx_secure, as well as TCP initial sequence
 * numbers, IP identification fields, ephemeral ports and the DHCP transaction
 * id in the core.  An LCG is fully recoverable from one output.
 *
 * src/common/ami_random.c replaces it with a SHA-256 hash DRBG over an entropy
 * pool.  See include/aminetxduo/random.h: the expansion is sound, the
 * collection is not audited, and ami_random_is_seeded() reports the difference.
 * Anything generating a real key is expected to check it.
 *
 * Declared by hand rather than by including the header: nx_port.h is pulled
 * into every vendored translation unit, and aminetxduo/random.h drags in
 * exec/types.h, whose `#define VOID void` collides with tx_port.h.
 */
#ifndef NX_RAND
extern int ami_random_rand(void);
#define NX_RAND                     ami_random_rand
#endif

#ifndef NX_SRAND
extern void ami_random_srand(unsigned int seed);
#define NX_SRAND                    ami_random_srand
#endif

/*
 * NX_CRYPTO_RBG is the huge-number source: the ECDHE private key comes out of
 * it, through _nx_crypto_ec_key_pair_generation_extra().  nx_crypto.h defaults
 * it to _nx_crypto_huge_number_rbg(), which assembles the number one
 * NX_CRYPTO_RAND() per 32 bits, and NX_CRYPTO_RAND is NX_RAND is
 * ami_random_rand(), which owes rand()'s caller a value in 0..0x7FFFFFFF and
 * masks bit 31 off to provide it.  So every 32-bit word of the number is one
 * bit short, at a known position.
 *
 * nx_crypto.h guards its own definition with #ifndef and is reached through
 * nx_api.h, which includes this header first, so defining it here wins.
 */
#ifndef NX_CRYPTO_RBG
extern unsigned int ami_crypto_rbg(unsigned int bits, unsigned char *result);
#define NX_CRYPTO_RBG               ami_crypto_rbg
#endif


/* Define various constants for the port.  */

#ifndef NX_IP_PERIODIC_RATE
#ifdef TX_TIMER_TICKS_PER_SECOND
#define NX_IP_PERIODIC_RATE         TX_TIMER_TICKS_PER_SECOND
#else
#define NX_IP_PERIODIC_RATE         100
#endif
#endif


/* Endian conversion.  No-ops on m68k, spelled as a no-op assignment rather
 * than as nothing at all.
 *
 * Every little-endian port defines these as `a = ((a >> 8) | (a << 8))` and so
 * on: an assignment expression, legal in statement position and in the middle
 * of a larger expression, requiring `a` to be an lvalue.  An empty definition
 * is legal only in the first position.  One caller uses the second:
 *
 *     addons/mdns/nxd_mdns.c:8489
 *       *(USHORT *)(... + NX_MDNS_FLAGS_OFFSET) |= NX_CHANGE_USHORT_ENDIAN(tc_bit);
 *
 * which expanded to `x |= ;` and would not compile.  It is the only such use in
 * the vendored tree, six others exist, all in nx_icmpv6_*, all in statement
 * position, so no big-endian NetX Duo port has hit it, the mDNS add-on
 * apparently never having been built on one.  Fixing it here rather than in
 * third_party/ keeps the rule that vendored source is not modified, and this
 * definition evaluates to the value and requires an lvalue exactly as the
 * little-endian one does.
 *
 * `((a) = (a))` and not the shorter `(a)`: the plain form makes every statement
 * use a bare expression with no effect, and two of those are in our own code
 * (src/net68k/n68k_checksum.c), which unlike third_party/ is compiled with
 * -Wall -Wextra -Werror by cmake/ci-warnings.cmake.  All four cross
 * configurations failed on -Werror=unused-value.  A self-assignment has an
 * effect as far as the front end is concerned, compiles to nothing, and keeps
 * both call positions legal.
 */

#define NX_CHANGE_ULONG_ENDIAN(a)   ((a) = (a))
#define NX_CHANGE_USHORT_ENDIAN(a)  ((a) = (a))

#ifndef htons
#define htons(val) (val)
#endif

#ifndef ntohs
#define ntohs(val) (val)
#endif

#ifndef ntohl
#define ntohl(val) (val)
#endif

#ifndef htonl
#define htonl(val) (val)
#endif


/*
 * The error-checking shell's caller checks.
 *
 * The generic ones read _tx_thread_system_state, which on a target is "an ISR is
 * running", one CPU-wide fact, so it is the same answer for every caller.  Here
 * interrupt context is an Exec Task holding the core lock, and the ThreadX port
 * raises that same counter around _tx_thread_terminate()/_tx_thread_delete()
 * with task switching enabled, because the reaper underneath can Wait().  During
 * that window every other Task reads a non-zero counter and fails a check it
 * should pass.  Two processes sharing bsdsocket.library is enough to hit it:
 * docs/RESEARCH.md 77.6 has a 44-byte write returning NX_CALLER_ERROR in a
 * loopback SSH session, mapped to EINVAL, killing the session, while the same
 * binary against a remote server never failed, because only one process was
 * bracketing.
 *
 * tx_amiga_caller_is_thread() asks whether the calling Task is the ThreadX baton
 * holder, which is what "a thread is calling" means on this port and cannot be
 * perturbed by another Task.  It rejects the tick task, the scheduler task and
 * any Task ThreadX never adopted, so the thread-only checks lose nothing.
 *
 * The two that admit non-thread callers by design keep the counter test: their
 * job is to reject an ISR, not to identify a thread, and NX_INIT_AND_THREADS in
 * particular has to let a plain Exec Task through during bring-up.  They are
 * still exposed to the teardown window, on nx_*_socket_create() only.
 *
 * There is no _tx_timer_thread to reject.  tx_port.h defines
 * TX_TIMER_PROCESS_IN_ISR, so a timer callback runs in the tick task and
 * arrives here as an ISR, which _tx_thread_system_state and
 * tx_amiga_caller_is_thread() already refuse; ThreadX does not declare the
 * thread at all in that configuration, and naming it here is what stopped the
 * whole tree linking when the define went in.
 */

/* At file scope, not only in NX_CALLER_CHECKING_EXTERNS: several addons expand
   the checks without placing that macro (nx_auto_ip.c among them) and take the
   ThreadX globals from tx_api.h instead. */
extern  UINT    tx_amiga_caller_is_thread(void);

#define NX_CALLER_CHECKING_EXTERNS          extern  volatile ULONG      _tx_thread_system_state;

#define NX_THREADS_ONLY_CALLER_CHECKING     if (tx_amiga_caller_is_thread() == ((UINT) 0)) \
                                                return(NX_CALLER_ERROR);

#define NX_INIT_AND_THREADS_CALLER_CHECKING if ((_tx_thread_system_state) && (_tx_thread_system_state < ((ULONG) 0xF0F0F0F0))) \
                                                return(NX_CALLER_ERROR);

#define NX_NOT_ISR_CALLER_CHECKING          if ((_tx_thread_system_state) && (_tx_thread_system_state < ((ULONG) 0xF0F0F0F0))) \
                                                return(NX_CALLER_ERROR);

#define NX_THREAD_WAIT_CALLER_CHECKING      if ((wait_option) && \
                                                (tx_amiga_caller_is_thread() == ((UINT) 0))) \
                                            return(NX_CALLER_ERROR);


/*
 * Count the mutex traffic, for -DAMINETXDUO_SCHEDCOUNT=ON.
 *
 * _tx_mutex_get()/_tx_mutex_put() are ThreadX core and the vendored tree is not
 * patched, so the count is taken at the call site instead.  nx_port.h reaches
 * every NetX Duo translation unit and every file of ours that includes
 * nx_api.h, which between them are all of the mutex traffic in the data path,
 * the IP protection mutex and nothing else.  ThreadX's own internal uses are
 * not counted, and there are none on this path.
 */
#ifdef AMINETXDUO_SCHEDCOUNT
extern ULONG    _tx_amiga_sched_count[];
#undef  tx_mutex_get
#undef  tx_mutex_put
#define tx_mutex_get(m, w)          (_tx_amiga_sched_count[TX_AMIGA_SC_MUTEX_GET]++, \
                                     _tx_mutex_get((m), (w)))
#define tx_mutex_put(m)             (_tx_amiga_sched_count[TX_AMIGA_SC_MUTEX_PUT]++, \
                                     _tx_mutex_put(m))
#endif


/* Define the version ID of NetX.  */

#ifdef NX_SYSTEM_INIT
CHAR                            _nx_version_id[] =
                                    "(c) Microsoft Corp. (c) Eclipse ThreadX Contributors"
                                    "  *  NetX Duo AmigaOS/m68k (AmiNetXDuo)  *";
#else
extern  CHAR                    _nx_version_id[];
#endif

#endif /* NX_PORT_H */
