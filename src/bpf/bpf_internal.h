/*
 * AmiNetXDuo, bpf_* internals.
 *
 * Private to src/bpf/. bpf_filter.c, bpf_validate.c, bpf_channel.c and
 * bpf_tap.c contain no AmigaOS calls. Everything platform-specific is behind
 * the five hooks at the bottom, which bpf_amiga.c implements and the host test
 * stubs out. Same split as src/config/ and src/mbuf/.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BPF_INTERNAL_H
#define AMINETXDUO_BPF_INTERNAL_H

#include "aminetxduo/bpf.h"
#include "aminetxduo/compat.h"

/* --------------------------------------------------------------- tunables */

#ifndef AMI_BPF_MAX_IFACES
#define AMI_BPF_MAX_IFACES      4
#endif

#define AMI_BPF_IFNAMSIZ        IFNAMSIZ

/* Ethernet framing the tap has to synthesise on transmit. */
#define AMI_BPF_ETH_HDR_LEN     14
#define AMI_BPF_ETH_ADDR_LEN    6

/* ------------------------------------------------------------- interfaces */

typedef struct AmiBpfIf
{
    BOOL           used;
    char           name[AMI_BPF_IFNAMSIZ];
    APTR           cookie;
    ULONG          dlt;
    ULONG          mtu;
    AmiBpfInjectFn inject;
} AmiBpfIf;

/* --------------------------------------------------------------- channels */

/*
 * Two buffers per channel, as in 4.4BSD: the tap fills `store` and the reader
 * drains `hold`, and they swap when the reader asks for data it has finished
 * with. This keeps the reader's copy-out, which can be the whole buffer, out
 * of the critical section the tap also needs.
 */
typedef struct AmiBpfChan
{
    BOOL             open;
    APTR             owner;                     /* the base that opened it  */

    AmiBpfIf        *iface;                     /* NULL until BIOCSETIF     */
    char             ifname[AMI_BPF_IFNAMSIZ];  /* remembered across detach */

    ULONG            blen;                      /* size of each buffer      */
    UBYTE           *bufbase;                   /* one allocation of 2*blen */
    UBYTE           *store;
    UBYTE           *hold;
    ULONG            store_len;
    ULONG            hold_len;
    ULONG            hold_pos;
    volatile BOOL    reading;                   /* a copy-out is in flight  */
    volatile BOOL    release_pending;           /* closed under a reader    */

    struct bpf_insn *filter;
    ULONG            filter_len;

    ULONG            dlt;
    BOOL             immediate;
    BOOL             promisc;
    ULONG            rtimeout_sec;
    ULONG            rtimeout_usec;

    APTR             notify_task;
    ULONG            notify_mask;
    APTR             irq_task;
    ULONG            irq_mask;

    ULONG            recv_count;                /* bpf_stat.bs_recv         */
    ULONG            drop_count;                /* bpf_stat.bs_drop         */
} AmiBpfChan;

/* There is deliberately no ami_bpf_chan[] here. The channel table is static in
   bpf_channel.c: nothing outside it reaches the table, and every path that
   needs it is one of the functions declared below. */

/* The interface table, which is shared: bpf_tap.c fills it from the SANA-II
   attachments and bpf_channel.c binds channels to its entries. */
extern AmiBpfIf   ami_bpf_iface[AMI_BPF_MAX_IFACES];

/*
 * Number of channels currently bound to an interface. The taps read this
 * before anything else so that the common "nobody is capturing" case costs a
 * load, a compare and a return.
 */
extern volatile UWORD ami_bpf_bound_channels;

/* ------------------------------------------------------- shared internals */

/* bpf_channel.c */
VOID ami_bpf_capture(AmiBpfIf *ifp, const AmiBpfView *view);
VOID ami_bpf_chan_unbind(AmiBpfIf *ifp);
VOID ami_bpf_chan_rebind(AmiBpfIf *ifp);

/* bpf_tap.c */
AmiBpfIf *ami_bpf_iface_by_cookie(APTR cookie);
AmiBpfIf *ami_bpf_iface_by_name(const char *name);
ULONG     ami_bpf_iface_address(const AmiBpfIf *ifp);

/* bpf_filter.c, byte count for a BPF_SIZE field, 0 if the encoding is bad. */
UWORD ami_bpf_size_bytes(UWORD code);

/* Endian- and alignment-neutral field access inside a capture record. The put
   and copy halves are static in bpf_channel.c, the only writer. The get half
   stays shared because tests/mbuf_bpf reads records back. */
ULONG ami_bpf_get32(const UBYTE *p);
UWORD ami_bpf_get16(const UBYTE *p);

VOID ami_bpf_zero_bytes(void *dst, ULONG len);

/* ---------------------------------------------------------- platform hooks */

/*
 * Guard the channel table. Forbid()/Permit() on the Amiga: the taps run on the
 * SANA-II reader threads and on whatever thread is inside nx_tcp_socket_send,
 * and the vectors run on application tasks, so the table is shared. Nothing
 * inside the bracket allocates, frees or blocks.
 */
VOID ami_bpf_lock(VOID);
VOID ami_bpf_unlock(VOID);

/* Open whatever clock ami_bpf_now() reads. Called from ami_bpf_open(), outside
   the lock. A lazy open inside the lock blocks under Forbid(). */
VOID ami_bpf_time_init(VOID);

/* Wall-clock time for bh_tstamp, seconds and microseconds since 1970. Must not
   block: ami_bpf_capture() calls it with the lock held. */
VOID ami_bpf_now(ULONG *sec, ULONG *usec);

/* The calling task, as an opaque token for ami_bpf_notify(). */
APTR ami_bpf_current_task(VOID);

/* Send `mask` to `task`. No-op when either is zero/NULL. */
VOID ami_bpf_notify(APTR task, ULONG mask);

/* Ticks per second for the BIOCSRTIMEOUT budget, and the slice bpf_read()
   sleeps between looks. One tick keeps a capture responsive. */
#define AMI_BPF_TICKS_PER_SEC   50UL
#define AMI_BPF_WAIT_SLICE      1UL

/* Sleep about `ticks` fiftieths of a second on the calling task. */
VOID ami_bpf_sleep(ULONG ticks);

/* Which of `mask` are already set on the calling task, without clearing any. */
ULONG ami_bpf_signals_set(ULONG mask);

#endif /* AMINETXDUO_BPF_INTERNAL_H */
