/*
 * tcpdrill: a synthetic SANA-II device installed inside the guest, so a test
 * script can be the remote peer with full control of every received byte.
 *
 * Not a network device: no wire, no loss, emulated-machine timing.  Use it for
 * conformance, never for throughput.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TCPDRILL_TAPDEV_H
#define TCPDRILL_TAPDEV_H

#include <exec/types.h>
#include <exec/libraries.h>

#define TAP_DEVICE_NAME     "tcpdrill.device"

/* One frame does not exceed 14 + 1500. Rounded up for the odd oversize. */
#define TAP_FRAME_MAX       1600

#define TAP_TX_SLOTS        96

typedef struct TapStats
{
    ULONG   tx_frames;          /* CMD_WRITEs completed                    */
    ULONG   tx_overrun;         /* frames the ring could not hold          */
    ULONG   rx_delivered;       /* frames handed to a queued CMD_READ      */
    ULONG   rx_no_reader;       /* frames dropped: no read of that type    */
    ULONG   rx_copy_failed;     /* the stack's CopyToBuff hook said no     */
    ULONG   reads_queued;       /* CMD_READs the device holds right now    */
    ULONG   online_count;       /* S2_ONLINEs seen                         */
    ULONG   offline_count;      /* S2_OFFLINEs seen                        */
} TapStats;

/* Must be called before anything opens bsdsocket.library: the stack opens its
   interfaces when it starts and OpenDevice() only finds what is already there. */
LONG tap_install(const UBYTE *mac);
VOID tap_remove(VOID);

/* TRUE between S2_ONLINE and S2_OFFLINE. */
BOOL tap_is_online(VOID);

/* Name DEVS:NetInterfaces/tap0 to a just-opened bsdsocket.library.  Until 0.27
   the library opened that drawer itself on its first open; it now opens no
   card, and this device does not exist before tap_install() runs, so no boot
   script can name it for us.  0 on success, else the library's errno. */
LONG tap_bring_up(struct Library *base);

/* Returns length, or 0 when the ring is empty.  `stamp` receives the E-Clock
   reading taken inside BeginIO. */
ULONG tap_tx_get(UBYTE *buf, ULONG max, ULONG *stamp);

/* Returns 0 on delivery, -1 when no CMD_READ of that EtherType was
   outstanding (a real SANA-II behaviour, not an error). */
LONG tap_rx_put(const UBYTE *frame, ULONG len);

VOID tap_set_rx_lifo(BOOL on);

/* How many reads are outstanding for one EtherType (0 = any). */
ULONG tap_reads_for(UWORD ether_type);

VOID tap_get_stats(TapStats *out);

ULONG tap_eclock_rate(VOID);
ULONG tap_eclock_now(VOID);

#endif /* TCPDRILL_TAPDEV_H */
