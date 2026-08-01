/*
 * AmiNetXDuo -- SANA-II shim internals.
 *
 * Private to src/sana2/. Include "tx_api.h" and "nx_api.h" before any exec
 * header: exec/types.h turns VOID into a macro, which breaks tx_port.h's
 * `typedef void VOID`.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_INTERNAL_H
#define AMINETXDUO_SANA2_INTERNAL_H

#include "tx_api.h"
#include "nx_api.h"

#include "aminetxduo/sana2.h"
#include "aminetxduo/config.h"
#include "aminetxduo/compat.h"

#include "sana2_device.h"

/* --------------------------------------------------------------- tunables */

/*
 * CMD_READ is per packet type and the device has no buffers of its own: every
 * frame that arrives with no matching read outstanding is dropped on the
 * floor. Each outstanding read therefore pins one NX_PACKET for its whole
 * life, so depth trades pool occupancy against loss under burst.
 *
 * The IPv4 depth is the receive window in frames, and four is too few.
 * Measured with tests/curl/run-curlverify.sh -p: sixteen concurrent HTTP
 * transfers through curl's multi interface lost six, twenty-four lost seven,
 * forty lost fifteen -- all as `curl: (7) Could not connect` after about
 * thirteen seconds, on connections the host had already accepted. The SYN went
 * out, the peer answered, and the SYN/ACK arrived in a burst with no read
 * outstanding to catch it. At depth eight, forty concurrent transfers lost
 * none.
 *
 * The value below is therefore a floor. ami_sana2_rx_start() sizes the IPv4
 * reader from the packet pool instead (see the comment there), since the pool
 * is itself sized from AvailMem().
 *
 * The 1 MB floor (docs/RESEARCH.md §81) gives a pool of 17 packets, close to
 * AMI_POOL_MIN_PACKETS (16), and pinning a quarter of that would starve
 * transmit, so such a machine keeps the four and cannot absorb the burst.
 * ARP and IPv6 ND are low-rate and stay shallow.
 */
#ifndef AMI_SANA2_RX_DEPTH_IPV4
#define AMI_SANA2_RX_DEPTH_IPV4     4
#endif
#ifndef AMI_SANA2_RX_DEPTH_ARP
#define AMI_SANA2_RX_DEPTH_ARP      2
#endif
#ifndef AMI_SANA2_RX_DEPTH_IPV6
#define AMI_SANA2_RX_DEPTH_IPV6     2
#endif

/* One in this many pool packets may be pinned by the IPv4 reader. */
#ifndef AMI_SANA2_RX_POOL_SHARE
#define AMI_SANA2_RX_POOL_SHARE     8
#endif

#ifndef AMI_SANA2_RX_MAX_DEPTH
#define AMI_SANA2_RX_MAX_DEPTH      32
#endif

/*
 * How long a reader waits for the device to give its queued CMD_READs back
 * once it has asked. 25 x 2 ticks is one second: generous for a driver that
 * honours AbortIO(), and bounded for one that does not.
 */
#ifndef AMI_SANA2_RX_REAP_TRIES
#define AMI_SANA2_RX_REAP_TRIES     25
#endif

#ifndef AMI_SANA2_TX_SLOTS
#define AMI_SANA2_TX_SLOTS          8
#endif

#include "../thread_priorities.h"
#ifndef AMI_SANA2_RX_STACK_SIZE
#define AMI_SANA2_RX_STACK_SIZE     4096
#endif

/* Ticks to spin on a full TX ring before dropping the frame. */
#ifndef AMI_SANA2_TX_WAIT_TICKS
#define AMI_SANA2_TX_WAIT_TICKS     4
#endif

/*
 * Probe for raw-frame support at open time (see ami_sana2_probe_raw).
 *
 * Off, and the default has to be off rather than the build's job: the probe's
 * WaitIO() has no deadline and a2065.device 2.16 does not answer the AbortIO()
 * before it, so ami_sana2_open() never returns. CMakeLists.txt names the same
 * answer in both directions; this is what a build that does not go through it
 * gets.
 */
#ifndef AMI_SANA2_PROBE_RAW
#define AMI_SANA2_PROBE_RAW         0
#endif

/* Use raw framing when the probe says it is available. Off: see sana2_device.c. */
#ifndef AMI_SANA2_RAW_DEFAULT
#define AMI_SANA2_RAW_DEFAULT       0
#endif

/* Offer the 16-bit buffer-management tags. Off: the tag numbers could not be
   verified against any header on this toolchain -- see sana2_device.h. */
#ifndef AMI_SANA2_OFFER_COPY16
#define AMI_SANA2_OFFER_COPY16      0
#endif

#ifdef AMINETXDUO_IPV6
#define AMI_SANA2_RX_READERS        3
#else
#define AMI_SANA2_RX_READERS        2
#endif

/*
 * Two bytes of slack in front of the synthesised Ethernet header so the IP
 * header lands on a 4-byte boundary. NetX Duo's NX_PHYSICAL_HEADER is 16 for
 * the same reason, and the 68020 pays for misaligned longword reads.
 */
#define AMI_SANA2_RX_PAD            2

#define AMI_ETHERTYPE_RARP          0x8035

/* --------------------------------------------------------------- RX slots */

struct AmiSana2Rx;

/*
 * One outstanding CMD_READ. `req` must stay first: completed requests come
 * back as struct Message * from GetMsg() and are cast straight back to the
 * slot.
 */
typedef struct AmiRxSlot
{
    struct IOSana2Req   req;
    struct AmiSana2Rx  *owner;
    NX_PACKET          *packet;     /* pinned for the life of the request  */
    UCHAR              *dst;        /* where S2_CopyToBuff must write      */
    ULONG               capacity;   /* bytes available at dst              */
    ULONG               copied;     /* set by the copy hook                */
    BOOL                posted;
} AmiRxSlot;

typedef struct AmiSana2Rx
{
    AmiSana2If         *iface;
    ULONG               packet_type;
    UWORD               depth;

    TX_THREAD           thread;
    TX_SEMAPHORE        ready;
    TX_SEMAPHORE        exited;
    APTR                stack;

    struct Task        *task;
    struct MsgPort     *port;
    ULONG               wake_mask;

    /*
     * TX reaping duty. Exactly one reader carries it (see
     * ami_sana2_tx_reap_bind()), and only that reader has a nonzero reap_mask,
     * the signal the TX reply port raises on completion.
     */
    BOOL                reap_tx;        /* this reader has the duty         */
    BYTE                reap_sigbit;    /* -1 when none is held             */
    ULONG               reap_mask;

    volatile BOOL       started;    /* tx_thread_create succeeded          */
    volatile BOOL       running;
    volatile BOOL       stop;
    volatile BOOL       failed;

    /* Reads the device would not give back at teardown. Nonzero means this
       reader's slots, pinned packets and reply port are still reachable by the
       device, so none may be freed -- see ami_sana2_rx_teardown(). */
    volatile UWORD      orphans;

    AmiRxSlot           slot[AMI_SANA2_RX_MAX_DEPTH];
} AmiSana2Rx;

/* --------------------------------------------------------------- TX slots */

/* `req` must stay first -- same GetMsg() cast as AmiRxSlot. */
typedef struct AmiTxSlot
{
    struct IOSana2Req   req;
    AmiSana2If         *iface;

    NX_PACKET          *packet;     /* released when the write completes   */
    NX_PACKET          *cursor;     /* chain walk state for S2_CopyFromBuff*/
    ULONG               cursor_off;
    ULONG               consumed;
    ULONG               total;
    UWORD               hdr_len;    /* Ethernet bytes prepended in raw mode*/
    volatile BOOL       busy;
} AmiTxSlot;

/* ------------------------------------------------------------- interface */

struct AmiSana2If
{
    /* Device. The opened request doubles as the template every cloned
       request is copied from -- it carries io_Device, io_Unit and the
       device-side ios2_BufferManagement cookie. */
    struct IOSana2Req   templ;
    BOOL                device_open;
    char                device[AMI_CFG_PATH_LEN];
    ULONG               unit;
    struct TagItem      buffer_tags[8];

    /* Hardware facts from S2_DEVICEQUERY / S2_GETSTATIONADDRESS. */
    UCHAR               mac[AMI_ETH_ADDR_SIZE];
    ULONG               mtu;
    ULONG               bps;
    ULONG               hw_type;
    UWORD               addr_bits;
    UWORD               addr_bytes;     /* 6 for Ethernet, 0 if addressless */

    BOOL                online;
    /* Administrative state: the stack's intent, not the wire's condition.
       Only the driver entry's enable/disable cases write it. */
    BOOL                admin_up;
    BOOL                raw_supported;
    BOOL                raw_mode;

    /* NetX Duo binding. */
    NX_IP              *ip;
    UINT                index;
    NX_INTERFACE       *interface_ptr;
    NX_PACKET_POOL     *pool;

    /* TX ring. The reply port raises a signal on the reader that carries the
       reaping duty, and falls back to PA_IGNORE when no reader holds it; the
       sending thread never blocks on it either way. */
    struct MsgPort      tx_port;
    AmiTxSlot           tx[AMI_SANA2_TX_SLOTS];

    /* RX readers, one per packet type. */
    AmiSana2Rx          rx[AMI_SANA2_RX_READERS];
    BOOL                rx_running;

    /* Set when a reader could not be reclaimed -- either it never exited or
       the device kept its CMD_READs. The whole interface is then unfreeable
       and unrestartable, because the device holds pointers into it. */
    BOOL                rx_orphaned;

    /* Same on the transmit side: set while ami_sana2_tx_drain() has left the
       device holding a CMD_WRITE. Those requests and their reply port are
       inside this allocation too. Cleared by a later drain that succeeds. */
    BOOL                tx_orphaned;

    AmiSana2Stats       stats;
};

/* ------------------------------------------------------------- internals */

/* sana2_copy.c -- called by the device in m68k register convention. */
BOOL ami_sana2_copy_to_buff(register APTR to    __asm("a0"),
                            register APTR from  __asm("a1"),
                            register ULONG len  __asm("d0"));
BOOL ami_sana2_copy_from_buff(register APTR to   __asm("a0"),
                              register APTR from __asm("a1"),
                              register ULONG len __asm("d0"));
VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);

/* sana2_device.c */
VOID ami_sana2_block_enter(VOID);
VOID ami_sana2_block_leave(VOID);
VOID ami_sana2_port_init(struct MsgPort *port, struct Task *task, BYTE sigbit,
                         UBYTE flags);
LONG ami_sana2_command(AmiSana2If *iface, struct IOSana2Req *req, UWORD command);
LONG ami_sana2_online(AmiSana2If *iface);
LONG ami_sana2_offline(AmiSana2If *iface);
LONG ami_sana2_multicast(AmiSana2If *iface, UWORD command,
                         ULONG addr_msw, ULONG addr_lsw);
VOID ami_sana2_refresh_stats(AmiSana2If *iface);

/* sana2_driver.c */
VOID ami_sana2_unbind(AmiSana2If *iface);

/* sana2_rx.c */
LONG ami_sana2_rx_start(AmiSana2If *iface);
VOID ami_sana2_rx_stop(AmiSana2If *iface);
VOID ami_sana2_rx_deliver(AmiSana2If *iface, NX_PACKET *packet);

/* sana2_tx.c */
VOID ami_sana2_tx_init(AmiSana2If *iface);
VOID ami_sana2_tx_reap(AmiSana2If *iface);
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit);
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface);
VOID ami_sana2_tx_defer(AmiSana2If *iface);
VOID ami_sana2_tx_drain(AmiSana2If *iface);
UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                       ULONG dst_msw, ULONG dst_lsw);

#endif /* AMINETXDUO_SANA2_INTERNAL_H */
