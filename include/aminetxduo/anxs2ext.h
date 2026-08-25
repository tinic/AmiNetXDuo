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

typedef UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len, ULONG sum,
                                 UBYTE summed);

#endif /* AMINETXDUO_ANXS2EXT_H */
