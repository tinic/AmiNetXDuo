/*
 * AmiNetXDuo, private SANA-II buffer-management extensions.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ANXS2EXT_H
#define AMINETXDUO_ANXS2EXT_H

#include <exec/types.h>

/* S2_Dummy is (TAG_USER + 0xB0000), spelled out so this header needs no
   sana2.h; the offsets sit clear of S2_Dummy's growing neighbourhood. */

/* Where would `len` payload bytes for this posted CMD_READ land?  NULL
   declines, and the device falls back to the staging copy + S2_CopyToBuff.  A
   non-NULL answer must be followed by exactly one RX_FILLED. */
#define ANXD_S2_RX_DIRECT       (0x80000000UL + 0xB0000UL + 0x4181UL)

/* `len` bytes written at the answered pointer.  `summed` says whether `sum`
   holds the running longword ones-complement sum (n68k_copy_sum_longwords
   semantics, zero-padded tail).  Both hooks run at interrupt level, before
   the CMD_READ is replied. */
#define ANXD_S2_RX_FILLED       (0x80000000UL + 0xB0000UL + 0x4182UL)

/* THE LINK HEADER, WRITTEN ONCE INSTEAD OF COPIED TWICE.
 *
 * Without it the fourteen bytes travel a long way for what they are: the
 * device lifts the two addresses out of the frame into ios2_SrcAddr and
 * ios2_DstAddr, and the opener lifts them back out of the request into the
 * packet and adds the type -- four six-byte moves and a word, per frame, for
 * bytes the device was already holding.
 *
 * Set, and the device writes the WHOLE fourteen-byte header at the fourteen
 * bytes IMMEDIATELY BEFORE the pointer RX_DIRECT answered, then fills the
 * request fields as before.  The opener may then skip synthesising it.
 *
 * ONLY MEANINGFUL WITH RX_DIRECT, and only on a cooked read: the direct path
 * refuses SANA2IOF_RAW requests already (netdev_direct.c:145), because a raw
 * destination starts at the frame and there is nothing in front of it.  An
 * opener that sets this promises those fourteen bytes are its own to write.
 *
 * ti_Data IS A POINTER TO A BOOL, NOT A FLAG AND NOT A HOOK.  A device that
 * understands the tag sets it TRUE; one that does not leaves it alone, and the
 * opener then knows to keep synthesising.  There is no other way round: the
 * tag list is one-way, RX_FILLED's signature is published and cannot grow an
 * argument, and an opener that guessed from RX_FILLED alone would hand a
 * third-party direct-path driver's frames a header of stale bytes. */
#define ANXD_S2_RX_LINK_HDR     (0x80000000UL + 0xB0000UL + 0x4183UL)

typedef UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len, ULONG sum,
                                 UBYTE summed);

#endif /* AMINETXDUO_ANXS2EXT_H */
