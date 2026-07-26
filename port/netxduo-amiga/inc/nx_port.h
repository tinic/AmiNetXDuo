/***************************************************************************
 * Eclipse ThreadX NetX Duo -- AmigaOS / m68k port.
 *
 * Derived from ports/linux/gnu/inc/nx_port.h
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2025-present Eclipse ThreadX Contributors
 *
 * Changes from the Linux original:
 *   - NX_LITTLE_ENDIAN is NOT defined.  m68k is big-endian, which is also the
 *     network byte order, so NX_CHANGE_ULONG_ENDIAN/NX_CHANGE_USHORT_ENDIAN
 *     and htons/htonl/ntohs/ntohl all compile away to nothing.  Every header
 *     byte-swap in the stack disappears -- a real advantage over x86 targets
 *     (docs/RESEARCH.md 5.3).
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


/* Big-endian target: NX_LITTLE_ENDIAN deliberately left undefined.  */


/*
 * Random numbers.
 *
 * Left undefined, nx_api.h falls back to NX_RAND == rand(), which on this
 * toolchain is newlib's 32-bit LCG -- and NX_RAND is what generates ECDHE
 * private keys and the TLS client random in nx_secure, as well as TCP initial
 * sequence numbers, IP identification fields, ephemeral ports and the DHCP
 * transaction id in the core.  An LCG is fully recoverable from one output.
 *
 * src/common/ami_random.c replaces it with a SHA-256 hash DRBG over an
 * entropy pool.  Read include/aminetxduo/random.h before believing the pool
 * is any good: the expansion is sound, the collection is not audited, and the
 * module reports its own weakness through ami_random_is_seeded() rather than
 * pretending.  Anything generating a real key is expected to check that.
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


/* Define various constants for the port.  */

#ifndef NX_IP_PERIODIC_RATE
#ifdef TX_TIMER_TICKS_PER_SECOND
#define NX_IP_PERIODIC_RATE         TX_TIMER_TICKS_PER_SECOND
#else
#define NX_IP_PERIODIC_RATE         100
#endif
#endif


/* Endian conversion.  All no-ops on m68k -- but SPELLED as a no-op assignment
 * rather than as nothing at all, and that is load-bearing.
 *
 * Every little-endian port defines these as `a = ((a >> 8) | (a << 8))` and so
 * on: an ASSIGNMENT EXPRESSION, which is legal in statement position AND in
 * the middle of a larger expression, and which requires `a` to be an lvalue.
 * The empty definition this used to carry is legal only in the first of those,
 * and the difference is invisible until a caller uses the second form.  One
 * does:
 *
 *     addons/mdns/nxd_mdns.c:8489
 *       *(USHORT *)(... + NX_MDNS_FLAGS_OFFSET) |= NX_CHANGE_USHORT_ENDIAN(tc_bit);
 *
 * which expanded to `x |= ;` and would not compile.  It is the only such use
 * in the whole vendored tree -- six others exist, all in nx_icmpv6_*, all in
 * statement position -- so it is a defect in the add-on that no big-endian
 * NetX Duo port has ever run into, because as far as we can tell nobody has
 * ever built the mDNS add-on on one.  Fixing it HERE rather than in
 * third_party/ keeps the standing rule that vendored source is not modified,
 * and it is a correction rather than a workaround: this now evaluates to the
 * value, and requires an lvalue, exactly as the little-endian definition does.
 *
 * `((a) = (a))` and not the shorter `(a)`, for one measured reason.  The plain
 * form makes every statement use a bare expression with no effect, and two of
 * those are in OUR code (src/net68k/n68k_checksum.c), which unlike anything in
 * third_party/ is compiled with -Wall -Wextra -Werror by cmake/ci-warnings
 * .cmake.  All four cross configurations failed on -Werror=unused-value.  A
 * self-assignment has an effect as far as the front end is concerned, compiles
 * to nothing, and keeps both call positions legal.
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


/* Define several macros for the error checking shell in NetX.  */

#ifndef TX_TIMER_PROCESS_IN_ISR

#define NX_CALLER_CHECKING_EXTERNS          extern  TX_THREAD           *_tx_thread_current_ptr; \
                                            extern  TX_THREAD           _tx_timer_thread; \
                                            extern  volatile ULONG      _tx_thread_system_state;

#define NX_THREADS_ONLY_CALLER_CHECKING     if ((_tx_thread_system_state) || \
                                                (_tx_thread_current_ptr == TX_NULL) || \
                                                (_tx_thread_current_ptr == &_tx_timer_thread)) \
                                                return(NX_CALLER_ERROR);

#define NX_INIT_AND_THREADS_CALLER_CHECKING if (((_tx_thread_system_state) && (_tx_thread_system_state < ((ULONG) 0xF0F0F0F0))) || \
                                                (_tx_thread_current_ptr == &_tx_timer_thread)) \
                                                return(NX_CALLER_ERROR);

#define NX_NOT_ISR_CALLER_CHECKING          if ((_tx_thread_system_state) && (_tx_thread_system_state < ((ULONG) 0xF0F0F0F0))) \
                                                return(NX_CALLER_ERROR);

#define NX_THREAD_WAIT_CALLER_CHECKING      if ((wait_option) && \
                                               ((_tx_thread_current_ptr == NX_NULL) || (_tx_thread_system_state) || (_tx_thread_current_ptr == &_tx_timer_thread))) \
                                            return(NX_CALLER_ERROR);

#else

#define NX_CALLER_CHECKING_EXTERNS          extern  TX_THREAD           *_tx_thread_current_ptr; \
                                            extern  volatile ULONG      _tx_thread_system_state;

#define NX_THREADS_ONLY_CALLER_CHECKING     if ((_tx_thread_system_state) || \
                                                (_tx_thread_current_ptr == TX_NULL)) \
                                                return(NX_CALLER_ERROR);

#define NX_INIT_AND_THREADS_CALLER_CHECKING if (((_tx_thread_system_state) && (_tx_thread_system_state < ((ULONG) 0xF0F0F0F0)))) \
                                                return(NX_CALLER_ERROR);

#define NX_NOT_ISR_CALLER_CHECKING          if ((_tx_thread_system_state) && (_tx_thread_system_state < ((ULONG) 0xF0F0F0F0))) \
                                                return(NX_CALLER_ERROR);

#define NX_THREAD_WAIT_CALLER_CHECKING      if ((wait_option) && \
                                               ((_tx_thread_current_ptr == NX_NULL) || (_tx_thread_system_state))) \
                                            return(NX_CALLER_ERROR);

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
