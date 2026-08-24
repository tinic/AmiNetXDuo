/*
 * AmiNetXDuo, private SANA-II buffer-management extensions.
 *
 * The standard hook set moves a received frame twice on a PIO card: once
 * from the FIFO port into the driver's staging buffer, and once from there
 * into the stack's packet through S2_CopyToBuff.  A 2026-08 profile of the
 * physical A1200/3C589 case measured the port drain at 9.4% of wall time and
 * the staging-to-packet copy/checksum at another 8.6%.  This extension removes
 * the latter and folds its checksum work into the former.  It is a bounded
 * saving, not an explanation for the whole gap to another stack.
 *
 * SANA-II's own answer is the DMA hook pair, and src/sana2/sana2_device.c
 * records why it is not offered: it is a contract about memory the shim
 * cannot cheaply honour for foreign drivers.  These two tags are the private
 * form of the same idea, between this project's own device and its own shim,
 * where both ends of the contract are in one repository:
 *
 *   ANXD_S2_RX_DIRECT   UBYTE *(*)(APTR ios2_data, ULONG len)
 *     The device asks where `len` bytes of payload for this posted CMD_READ
 *     would land, before it has copied anything anywhere.  The shim answers
 *     with the packet's payload pointer, or NULL to decline, in which case
 *     the device falls back to the staging copy and S2_CopyToBuff exactly as
 *     if the tag had never been offered.  A non-NULL answer must be followed
 *     by exactly one RX_FILLED call before the CMD_READ is replied.  Runs at
 *     interrupt level; the same constraints as copybuff.doc puts on the
 *     standard hooks.
 *
 *   ANXD_S2_RX_FILLED   VOID (*)(APTR ios2_data, ULONG len, ULONG sum,
 *                                UBYTE summed)
 *     The device has written `len` bytes at the answered pointer, straight
 *     off the hardware.  `summed` says whether `sum` carries the running
 *     longword ones-complement sum the verifier expects (the exact
 *     semantics of n68k_copy_sum_longwords over the payload, zero-padded
 *     tail); a device that only drained says summed = 0 and the verifier
 *     walks the frame at task level, which is still one interrupt-level
 *     copy fewer than the standard path.  Called before the CMD_READ is
 *     replied, from the same context as RX_DIRECT.
 *
 * A driver that never looks for the tags loses nothing; a shim that never
 * offers them costs the device one NULL test per open.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ANXS2EXT_H
#define AMINETXDUO_ANXS2EXT_H

#include <exec/types.h>

/*
 * S2_Dummy is (TAG_USER + 0xB0000); spelled out so this header needs no
 * sana2.h, which not every consumer's include path carries.  The offsets sit
 * far from S2_Dummy's neighbourhood, where sana2.h keeps growing.
 */
#define ANXD_S2_RX_DIRECT       (0x80000000UL + 0xB0000UL + 0x4181UL)
#define ANXD_S2_RX_FILLED       (0x80000000UL + 0xB0000UL + 0x4182UL)

typedef UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len, ULONG sum,
                                 UBYTE summed);

#endif /* AMINETXDUO_ANXS2EXT_H */
