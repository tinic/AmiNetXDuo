/*
 * AmiNetXDuo, SANA-II receive pipeline: one reader thread per packet type,
 * each keeping several CMD_READs in flight, because a SANA-II device drops any
 * frame that arrives with no matching read outstanding.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"
#include "aminetxduo/nxstatus.h"
#include "aminetxduo/budget.h"

/* tx_amiga_stack_in_use(), for the reader stacks. */
#include "tx_amiga.h"

#ifdef AMINETXDUO_RX_VERIFY
#include "net68k.h"
#endif

#include "nx_ip.h"
#ifndef NX_DISABLE_IPV4
#include "nx_arp.h"
#include "nx_rarp.h"
#endif

#ifdef AMINETXDUO_BPF
#include "aminetxduo/bpf.h"
#endif

#include "aminetxduo/events.h"

#include "aminetxduo/random.h"

#include <proto/exec.h>
/* BeginIO(): a macro over the device's own vector, no amiga.lib to link. */
#include <inline/alib.h>

VOID ami_sana2_block_enter(VOID);
VOID ami_sana2_block_leave(VOID);

/* --------------------------------------------------------- receive probe */

#ifdef AMINETXDUO_RXPROBE

#include <proto/timer.h>
#include <devices/timer.h>

extern struct Device *TimerBase;

static ULONG ami_rxprobe_clock(VOID)
{
    struct EClockVal ev;

    if (TimerBase == NULL)
        return 0;

    (VOID)ReadEClock(&ev);

    return ev.ev_lo;
}

static UWORD ami_rxprobe_bucket(ULONG ticks)
{
    UWORD b = 0;

    while (ticks != 0 && b < (AMI_RXPROBE_BUCKETS - 1))
    {
        ticks >>= 1;
        b++;
    }

    return b;
}

/* Replies already sitting on the port. depth - this is what the device holds. */
static UWORD ami_rxprobe_backlog(struct MsgPort *port)
{
    struct Node *node;
    UWORD        n = 0;

    Disable();
    for (node = port->mp_MsgList.lh_Head;
         node->ln_Succ != NULL && n <= AMI_SANA2_RX_MAX_DEPTH;
         node = node->ln_Succ)
        n++;
    Enable();

    return n;
}

static ULONG ami_rxprobe_be32(const UCHAR *p)
{
    return (((ULONG)p[0]) << 24) | (((ULONG)p[1]) << 16) |
           (((ULONG)p[2]) <<  8) |  ((ULONG)p[3]);
}

static UWORD ami_rxprobe_be16(const UCHAR *p)
{
    return (UWORD)((((UWORD)p[0]) << 8) | (UWORD)p[1]);
}

VOID ami_sana2_rxprobe_deliver(AmiSana2If *iface, const UCHAR *frame,
                               ULONG length)
{
    AmiRxSeqProbe *sp = &iface->seq;
    const UCHAR   *ip;
    const UCHAR   *tcp;
    ULONG          seq;
    ULONG          total;
    UWORD          ihl;
    UWORD          doff;
    UWORD          payload;

    if (length < AMI_ETH_HEADER_SIZE + 40)
        return;
    if (ami_rxprobe_be16(&frame[12]) != AMI_ETHERTYPE_IPV4)
        return;

    ip = frame + AMI_ETH_HEADER_SIZE;

    if ((ip[0] >> 4) != 4 || ip[9] != 6)
        return;

    ihl   = (UWORD)((ip[0] & 0x0F) * 4);
    total = (ULONG)ami_rxprobe_be16(&ip[2]);

    if (ihl < 20 || total < (ULONG)ihl + 20UL ||
        total > length - AMI_ETH_HEADER_SIZE)
        return;

    tcp     = ip + ihl;
    doff    = (UWORD)((tcp[12] >> 4) * 4);

    if (doff < 20 || (ULONG)ihl + doff > total)
        return;

    payload = (UWORD)(total - ihl - doff);
    seq     = ami_rxprobe_be32(&tcp[4]);

    if (payload == 0)
    {
        sp->pure_ack++;
        return;
    }

    sp->data_frames++;

    if (!sp->armed)
    {
        if (payload < 512)
        {
            sp->other++;
            return;
        }

        sp->peer  = ami_rxprobe_be32(&ip[12]);
        sp->sport = ami_rxprobe_be16(&tcp[0]);
        sp->dport = ami_rxprobe_be16(&tcp[2]);
        sp->next  = seq;
        sp->armed = TRUE;
    }
    else if (sp->peer  != ami_rxprobe_be32(&ip[12]) ||
             sp->sport != ami_rxprobe_be16(&tcp[0]) ||
             sp->dport != ami_rxprobe_be16(&tcp[2]))
    {
        sp->other++;
        return;
    }

    if (seq == sp->next)
    {
        sp->inorder++;
        sp->next = seq + payload;
    }
    else if ((LONG)(seq - sp->next) > 0)
    {
        if (sp->gaps < AMI_RXPROBE_GAPS)
        {
            sp->gap_want[sp->gaps]  = sp->next;
            sp->gap_got[sp->gaps]   = seq;
            sp->gap_avail[sp->gaps] = sp->avail;
            sp->gap_open[sp->gaps]  = ami_rxprobe_clock();
        }
        sp->gaps++;
        sp->ahead++;
        sp->ahead_bytes += seq - sp->next;
        sp->next = seq + payload;
    }
    else
    {
        UWORD g;

        sp->behind++;

        for (g = 0; g < sp->gaps && g < AMI_RXPROBE_GAPS; g++)
        {
            if (sp->gap_want[g] == seq && sp->gap_fill[g] == 0)
            {
                ULONG dt = ami_rxprobe_clock() - sp->gap_open[g];

                sp->gap_fill[g] = (dt != 0) ? dt : 1;
                break;
            }
        }

        if ((LONG)(seq + payload - sp->next) > 0)
            sp->next = seq + payload;
    }
}

#ifdef AMINETXDUO_RX_VERIFY

static VOID ami_sana2_rxprobe_drop(NX_PACKET *packet, const AmiRxSum *sum)
{
    const UCHAR *ip     = packet->nx_packet_prepend_ptr;
    UINT         rewalk = NX_FALSE;
    ULONG        seq    = 0;
    ULONG        stored = 0;
    ULONG        pseudo = 0;
    UWORD        part   = 0;

    (VOID)n68k_rx_verify(packet, &rewalk);

    if (packet->nx_packet_length >= 40 && (ip[0] >> 4) == 4)
    {
        UWORD ihl   = (UWORD)((ip[0] & 0x0F) * 4);
        ULONG total = (ULONG)ami_rxprobe_be16(&ip[2]);

        if (ihl >= 20 && total >= (ULONG)ihl + 20 &&
            total <= packet->nx_packet_length &&
            (ip[9] == 6 || ip[9] == 17))
        {
            const UCHAR *tr    = ip + ihl;
            ULONG        trlen = total - ihl;
            UWORD        coff  = (ip[9] == 6) ? 16 : 6;

            if (ip[9] == 6)
                seq = ami_rxprobe_be32(&tr[4]);
            stored = (ULONG)ami_rxprobe_be16(&tr[coff]);

            pseudo  = (ULONG)ami_rxprobe_be16(&ip[12]);
            pseudo += (ULONG)ami_rxprobe_be16(&ip[14]);
            pseudo += (ULONG)ami_rxprobe_be16(&ip[16]);
            pseudo += (ULONG)ami_rxprobe_be16(&ip[18]);
            pseudo += (ULONG)ip[9] + trlen;
            while ((pseudo >> 16) != 0UL)
                pseudo = (pseudo & 0xFFFFUL) + (pseudo >> 16);

            part = (stored == pseudo ||
                    stored == ((~pseudo) & 0xFFFFUL)) ? 1 : 0;
        }
    }

    AMI_ERROR("rxprobe drop: seq %08lx lane %s rewalk %ld partial %ld "
              "stored %04lx pseudo %04lx len %ld",
              (unsigned long)seq,
              (sum != NULL && sum->summed != FALSE) ? "sum" : "walk",
              (long)rewalk, (long)part,
              (unsigned long)stored, (unsigned long)pseudo,
              (long)packet->nx_packet_length);
}

#endif /* AMINETXDUO_RX_VERIFY */

VOID ami_sana2_rxprobe_report(const AmiSana2If *iface)
{
    const AmiRxSeqProbe *sp = &iface->seq;
    UWORD          i;
    UWORD          j;

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        const AmiSana2Rx *rx = &iface->rx[i];
        const AmiRxProbe *pr = &rx->probe;

        if (pr->drains == 0 && pr->posts == 0)
            continue;

        AMI_ERROR("rxprobe %ld: type %04lx depth %ld posts %ld drains %ld "
                  "dry %ld postzero %ld postpartial %ld",
                  (long)i, (long)rx->packet_type, (long)rx->depth,
                  (long)pr->posts, (long)pr->drains, (long)pr->dry,
                  (long)pr->post_zero, (long)pr->post_partial);

        AMI_ERROR("rxprobe %ld: baton max %ld sum %ld ticks",
                  (long)i, (long)pr->baton_max, (long)pr->baton_sum);

        /*
         * sweep_skip/sweep_run is the size of the win; unposted against the
         * recount is the invariant it rests on.  The recount is a SAMPLE, not
         * a snapshot -- this runs on NetStat's task while the reader runs --
         * so read a difference of one as a race and a standing difference as
         * a bug.
         */
        {
            UWORD k;
            UWORD held = 0;

            for (k = 0; k < rx->depth; k++)
            {
                if (rx->slot[k].posted)
                    held++;
            }

            AMI_ERROR("rxprobe %ld: sweeps run %ld skipped %ld, unposted %ld "
                      "recount %ld",
                      (long)i, (long)pr->sweep_run, (long)pr->sweep_skip,
                      (long)rx->unposted, (long)(rx->depth - held));
        }

        for (j = 0; j < AMI_RXPROBE_BUCKETS; j++)
        {
            if (pr->baton_hist[j] != 0)
                AMI_ERROR("rxprobe %ld: baton < 2^%ld ticks %ld",
                          (long)i, (long)j, (long)pr->baton_hist[j]);
        }

        for (j = 0; j <= AMI_SANA2_RX_MAX_DEPTH; j++)
        {
            if (pr->avail_hist[j] != 0)
                AMI_ERROR("rxprobe %ld: avail %ld -> %ld drains",
                          (long)i, (long)j, (long)pr->avail_hist[j]);
        }

        for (j = 0; j <= AMI_SANA2_RX_MAX_DEPTH; j++)
        {
            if (pr->backlog_hist[j] != 0)
                AMI_ERROR("rxprobe %ld: backlog %ld -> %ld drains",
                          (long)i, (long)j, (long)pr->backlog_hist[j]);
        }

        for (j = 0; j < AMI_RXPROBE_WORST; j++)
        {
            if (pr->worst_backlog[j] != 0)
                AMI_ERROR("rxprobe %ld: worst backlog %ld avail %ld "
                          "since last wake %ld baton %ld ticks",
                          (long)i, (long)pr->worst_backlog[j],
                          (long)pr->worst_avail[j], (long)pr->worst_when[j],
                          (long)pr->worst_baton[j]);
        }
    }

    AMI_ERROR("rxprobe dev: rx %ld tx %ld, shim rx %ld tx %ld, "
              "bad %ld ovr %ld unk %ld alloc %ld err %ld",
              (long)iface->probe_dev_rx, (long)iface->probe_dev_tx,
              (long)iface->stats.packets_received,
              (long)iface->stats.packets_sent,
              (long)iface->stats.bad_data, (long)iface->stats.overruns,
              (long)iface->stats.unknown_types,
              (long)iface->stats.alloc_failures,
              (long)iface->stats.rx_errors);

    {
        UWORD ri;

        for (ri = 0; ri < AMI_SANA2_RX_READERS; ri++)
        {
            const UBYTE *st = (const UBYTE *)iface->rx[ri].stack;
            ULONG        used = 0;

            if (st == NULL)
                continue;

            /* The stack grows down, so the deepest byte touched is the first
               one from the low end that is no longer the pattern. */
            while (used < (ULONG)AMI_SANA2_RX_STACK_SIZE &&
                   st[used] == 0xA5)
                used++;

            AMI_ERROR("rxprobe stack: reader %ld used %ld of %ld, %ld spare",
                      (long)ri,
                      (long)((ULONG)AMI_SANA2_RX_STACK_SIZE - used),
                      (long)AMI_SANA2_RX_STACK_SIZE, (long)used);
        }
    }

#ifdef AMINETXDUO_RX_VERIFY
    /*
     * Whether the fused checksum is actually being used. The copy hook sums
     * the frame out of the loads the copy is already doing, but only on its
     * aligned path; anything else falls to n68k_rx_verify(), which walks the
     * whole payload a second time -- about 2,900 cycles on a 1460-byte
     * segment, against a per-frame drain budget of roughly 15,750. So
     * from_copy well below transport_ok is a re-walk on most frames, and the
     * counters existed with nothing printing them.
     */
    AMI_ERROR("rxprobe verify: transport %ld from_copy %ld ip %ld, "
              "bad ip %ld transport %ld",
              (long)n68k_rx_verify_stats.transport_ok,
              (long)n68k_rx_verify_stats.from_copy,
              (long)n68k_rx_verify_stats.ip_ok,
              (long)n68k_rx_verify_stats.bad_ip,
              (long)n68k_rx_verify_stats.bad_transport);
    AMI_ERROR("rxprobe verify skips: short %ld version %ld length %ld "
              "frag %ld proto %ld udp_nosum %ld",
              (long)n68k_rx_verify_stats.skip_short,
              (long)n68k_rx_verify_stats.skip_version,
              (long)n68k_rx_verify_stats.skip_length,
              (long)n68k_rx_verify_stats.skip_fragment,
              (long)n68k_rx_verify_stats.skip_protocol,
              (long)n68k_rx_verify_stats.skip_udp_nosum);
#endif

    if (!sp->armed)
    {
        AMI_ERROR("rxprobe seq: no bulk flow seen");
        return;
    }

    AMI_ERROR("rxprobe seq: peer %08lx %ld->%ld inorder %ld ahead %ld "
              "(%ld bytes) behind %ld ack %ld other %ld data %ld",
              (long)sp->peer, (long)sp->sport, (long)sp->dport,
              (long)sp->inorder, (long)sp->ahead, (long)sp->ahead_bytes,
              (long)sp->behind, (long)sp->pure_ack, (long)sp->other,
              (long)sp->data_frames);

    for (i = 0; i < sp->gaps && i < AMI_RXPROBE_GAPS; i++)
    {
        AMI_ERROR("rxprobe gap %ld: want %08lx got %08lx (%ld bytes) avail %ld "
                  "filled after %ld ticks",
                  (long)i, (long)sp->gap_want[i], (long)sp->gap_got[i],
                  (long)(sp->gap_got[i] - sp->gap_want[i]),
                  (long)sp->gap_avail[i], (long)sp->gap_fill[i]);
    }
}

#endif /* AMINETXDUO_RXPROBE */

/* ------------------------------------------------------------- delivery */

/*
 * Dispatch one frame into NetX Duo, on this thread.
 *
 * _nx_ip_packet_receive() is documented as called by the application I/O
 * driver (nx_ip_packet_receive.c:65-68), so the reader is a supported caller.
 * What the deferred entry points bought was the lock, and the caller holds it:
 * ami_sana2_rx_drain() takes nx_ip_protection once for the whole run it
 * drains, the way nx_ip_thread_entry.c holds it for the whole of its event
 * loop.  The header on that function is where the reasoning lives.
 */
static VOID ami_sana2_rx_dispatch(NX_IP *ip, NX_PACKET *packet, UINT type)
{
    switch (type)
    {
#ifndef NX_DISABLE_IPV4
    case AMI_ETHERTYPE_ARP:
        _nx_arp_packet_receive(ip, packet);
        break;

    case AMI_ETHERTYPE_RARP:
        _nx_rarp_packet_receive(ip, packet);
        break;
#endif

    default:
        _nx_ip_packet_receive(ip, packet);
        break;
    }
}

/*
 * Both the cooked path (header synthesised) and the raw path (header off the
 * wire) end here, so the two modes must produce the same thing.  Input runs on
 * this thread, under the lock ami_sana2_rx_drain() is holding.
 */
VOID ami_sana2_rx_deliver(AmiSana2If *iface, NX_PACKET *packet,
                          const AmiRxSum *sum)
{
    UINT type;

#ifndef AMINETXDUO_RX_VERIFY
    /* The sum the copy carried is only consulted by the verify path, so
       without it this is the one caller that takes one and reads nothing from
       it. */
    (VOID)sum;
#endif

#ifdef AMINETXDUO_BPF
    /*
     * Before the link header is stripped below, so one call covers both modes
     * and the frame is a complete link-layer frame in one contiguous run.
     */
    ami_bpf_tap_rx(iface, packet->nx_packet_prepend_ptr,
                   packet->nx_packet_length);
#endif

#ifdef AMINETXDUO_RXPROBE
    ami_sana2_rxprobe_deliver(iface, packet->nx_packet_prepend_ptr,
                              packet->nx_packet_length);
#endif

    if (packet->nx_packet_length < AMI_ETH_HEADER_SIZE)
    {
        nx_packet_release(packet);
        iface->stats.rx_errors++;
        iface->stats.rx_err_runt++;
        return;
    }

    /*
     * ONE WORD ON THE 68000, TWO BYTES EVERYWHERE ELSE.
     *
     * prepend_ptr is nx_packet_data_start + AMI_SANA2_RX_PAD, and the static
     * assertions below put data_start on a longword and the pad at 2, so byte
     * 12 of the frame is at an EVEN address on every packet this reader sees
     * -- which is all a 68000 needs, an address error being an odd-address
     * fault and not an unaligned one.  netdev_rx() reads the same field the
     * same way (netdev_device.c) and this side had not caught up.
     *
     * THE HOST TIER COMPILES THIS FILE AND THE HOST IS LITTLE-ENDIAN, which is
     * what the byte form was for: the first version of this read the two bytes
     * as a native USHORT and turned every ethertype round on x86 -- twelve
     * demux checks failed at once.  Same shape as N68K_RDW16 in
     * src/net68k/n68k_rx_verify.c, and for the same two reasons.
     */
#if defined(__mc68000__) || defined(__m68k__)
    type = (UINT)*(const USHORT *)(CONST_APTR)
                 (packet->nx_packet_prepend_ptr + 12);
#else
    type = (((UINT)packet->nx_packet_prepend_ptr[12]) << 8) |
           ((UINT)packet->nx_packet_prepend_ptr[13]);
#endif

    packet->nx_packet_address.nx_packet_interface_ptr = iface->interface_ptr;

    /* Clean off the link header before handing the frame upwards. */
    packet->nx_packet_prepend_ptr += AMI_ETH_HEADER_SIZE;
    packet->nx_packet_length      -= AMI_ETH_HEADER_SIZE;

    /*
     * IPv4 AHEAD OF THE SWITCH, BECAUSE IT IS EVERY FRAME AND WAS THE LAST
     * COMPARE REACHED.  GCC builds a binary search over the four case
     * values, and 0x0800 sits on the far side of it: the generated demux ran
     * `cmp.w #-32715` then `jhi` then `cmp.w #2048` before taking the arm a
     * bulk receive takes every time.  Testing it first costs the other three
     * one compare each, which they can afford -- ARP is a handful a minute
     * and the rest are rarer still.
     *
     * A straight hoist, not a restructure: every case here is self-contained
     * and ends in break, the switch is the last statement in the function,
     * and this arm already returns early on a dropped frame.
     */
    if (type == AMI_ETHERTYPE_IPV4)
    {
#ifdef AMINETXDUO_RX_VERIFY
        /*
         * Checked here and reported to the stack so it does not walk the
         * payload again.  A declined frame carries no bits and the stack checks
         * it as always; a rejected one never reaches the stack.
         */
        {
            UINT    drop =  NX_FALSE;
            ULONG   caps;

            /*
             * The copy hook already summed this frame out of the loads the copy
             * was doing.  A slot that did not sum (misaligned, or no slot at
             * all) passes zero and the verifier walks.
             */
#ifdef AMINETXDUO_RXPROBE
            {
                /*
                 * Both entries, because which one runs is the question the
                 * from_copy counter answers and this leg must not depend on
                 * the answer.  See the note beside AmiBudgetLeg verify.
                 */
                ULONG vt0 = ami_budget_clock();

                if ((sum != NULL) && (sum->summed != FALSE))
                    caps = n68k_rx_verify_sum(packet, sum->sum, sum->copied,
                                              &drop);
                else
                    caps = n68k_rx_verify(packet, &drop);

                ami_budget_verify(ami_budget_clock() - vt0);
            }
#else
            if ((sum != NULL) && (sum->summed != FALSE))
                caps = n68k_rx_verify_sum(packet, sum->sum, sum->copied,
                                          &drop);
            else
                caps = n68k_rx_verify(packet, &drop);
#endif

            if (drop != NX_FALSE)
            {
#ifdef AMINETXDUO_RXPROBE
                ami_sana2_rxprobe_drop(packet, sum);
#endif
                nx_packet_release(packet);
                iface->stats.rx_errors++;
                iface->stats.rx_err_verify++;
                return;
            }

            packet->nx_packet_interface_capability_flag = caps;
        }
#endif
#ifdef AMINETXDUO_RXPROBE
        {
            const UCHAR *ip4 = packet->nx_packet_prepend_ptr;

            if (packet->nx_packet_length >= 40UL && (ip4[0] >> 4) == 4U &&
                ip4[9] == 6U)
            {
                UINT  ihl   = (UINT)((ip4[0] & 0x0FU) << 2);
                ULONG total = ((ULONG)ip4[2] << 8) | ip4[3];

                if (ihl >= 20U && total > (ULONG)ihl + 20UL &&
                    total <= packet->nx_packet_length)
                    ami_budget_deliver(ami_budget_clock());
            }
        }
#endif
        iface->stats.packets_received++;
        ami_sana2_rx_dispatch(iface->ip, packet, AMI_ETHERTYPE_IPV4);
        return;
    }

    switch (type)
    {
    case AMI_ETHERTYPE_IPV6:
#if defined(AMINETXDUO_RX_VERIFY) && defined(FEATURE_NX_IPV6)
        /* Same two entries as IPv4 above: no header checksum exists to claim,
           so a cleared frame carries the transport bit only. */
        {
            UINT    drop =  NX_FALSE;
            ULONG   caps;

            if ((sum != NULL) && (sum->summed != FALSE))
                caps = n68k_rx_verify_sum(packet, sum->sum, sum->copied,
                                          &drop);
            else
                caps = n68k_rx_verify(packet, &drop);

            if (drop != NX_FALSE)
            {
                nx_packet_release(packet);
                iface->stats.rx_errors++;
                iface->stats.rx_err_verify++;
                return;
            }

            packet->nx_packet_interface_capability_flag = caps;
        }
#endif
        iface->stats.packets_received++;
        ami_sana2_rx_dispatch(iface->ip, packet, AMI_ETHERTYPE_IPV6);
        break;

#ifndef NX_DISABLE_IPV4
    case AMI_ETHERTYPE_ARP:
        iface->stats.packets_received++;
        ami_sana2_rx_dispatch(iface->ip, packet, AMI_ETHERTYPE_ARP);
        break;

    case AMI_ETHERTYPE_RARP:
        iface->stats.packets_received++;
        ami_sana2_rx_dispatch(iface->ip, packet, AMI_ETHERTYPE_RARP);
        break;
#endif /* !NX_DISABLE_IPV4 */

    default:
        iface->stats.unknown_types++;
        nx_packet_release(packet);
        break;
    }
}

/*
 * Six bytes as a longword and a word rather than a call into the tuned copy,
 * whose prologue costs more than the move for a length this short. Both ends
 * are even: nx_packet_data_start is a multiple of NX_PACKET_ALIGNMENT and
 * AMI_SANA2_RX_PAD is 2 (see the assertions below), and ios2_SrcAddr and
 * ios2_DstAddr sit at even offsets in the request. A 68000 takes an address
 * error on an odd address, not on an unaligned-but-even one. Same shape as
 * nd_addr6() in netdev_device.c.
 */
static VOID ami_sana2_addr6(UCHAR *to, const UCHAR *from)
{
    *(ULONG *)(APTR)to       = *(const ULONG *)(CONST_APTR)from;
    *(UWORD *)(APTR)(to + 4) = *(const UWORD *)(CONST_APTR)(from + 4);
}

/* ------------------------------------------------------------ slot arming */

/*
 * nx_packet_data_start is a multiple of NX_PACKET_ALIGNMENT (4) and the IP
 * header sits at data_start + AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE, so that
 * sum must be a multiple of 4 or a 68000 takes an address error.  2 + 14 == 16.
 */
_Static_assert(((AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE) & 3) == 0,
               "RX pad must land the IP header on a longword boundary");
_Static_assert((NX_PACKET_ALIGNMENT % 4) == 0,
               "packet payloads are not longword aligned");

/* Position the packet and work out where the copy hook must write. */
static VOID ami_sana2_rx_arm(AmiSana2If *iface, AmiRxSlot *slot)
{
    NX_PACKET *packet = slot->packet;
    UCHAR     *base   = packet->nx_packet_data_start + AMI_SANA2_RX_PAD;

    packet->nx_packet_prepend_ptr = base;
    packet->nx_packet_append_ptr  = base;
    packet->nx_packet_length      = 0;

    /* Cooked: leave room for the synthesised header. Raw: the device supplies
       it.  The offset is settled by the open (sana2_internal.h rx_dst_off), so
       this is an add and not a test and a branch. */
    slot->dst = base + iface->rx_dst_off;

    /*
     * A POOL CONSTANT, ARRIVED AT PER FRAME.  Every packet in a pool has the
     * same payload size, and dst is data_start plus a fixed 2 + 14 (or 2 in
     * raw mode), so data_end - dst is the same number for every packet this
     * interface will ever arm.  Computed on the first arm and reused: what
     * changes between packets is the ADDRESS, which is recomputed above.
     */
    if (iface->rx_capacity == 0UL)
        iface->rx_capacity = (ULONG)(packet->nx_packet_data_end - slot->dst);

    slot->capacity    = iface->rx_capacity;
    slot->copied      = 0;
    slot->hdr_written = FALSE;
#ifdef AMINETXDUO_RX_VERIFY
    /* ami_sana2_copy_to_buff() clears this on entry and sets it only on the
       aligned path.  A driver that never calls the copy hook -- it is optional
       in SANA-II -- would otherwise leave the previous frame's verdict here. */
    slot->summed   = FALSE;
#endif
}

/* ---- BEGIN posted-flag owner (tools/check-rx-posted.sh) ---------------- *
 *
 * `slot->posted` and `rx->unposted` are ONE fact, so they are written in one
 * place.  ami_sana2_rx_post() skips its whole sweep when the count is zero,
 * and that is sound only while the count is exact: a stale nonzero costs one
 * wasted sweep, a stale zero silently stops refilling the ring and receive
 * collapses.  Three lines in this file used to set the flag directly;
 * check-rx-posted.sh now refuses any assignment to it outside this block, so
 * a fourth cannot be added without the counter.
 *
 * Reader-thread only.  The device writes the packet and replies, but it never
 * touches these two: the flag is cleared where the reply is dequeued, in the
 * drain, and in the teardown reap -- both on this thread.
 */
static VOID ami_sana2_rx_mark(AmiSana2Rx *rx, AmiRxSlot *slot, BOOL posted)
{
    if ((slot->posted != FALSE) == (posted != FALSE))
        return;

    slot->posted = posted;

    if (posted)
        rx->unposted--;
    else
        rx->unposted++;
}

/* Every slot idle and the count agreeing with it.  Called once, before the
   first post: a reader that has been stopped and started again comes back
   through here, and its slots were cleared by the teardown reap. */
static VOID ami_sana2_rx_mark_reset(AmiSana2Rx *rx)
{
    UWORD i;

    for (i = 0; i < rx->depth; i++)
        rx->slot[i].posted = FALSE;

    rx->unposted = rx->depth;
}

/* ---- END posted-flag owner --------------------------------------------- */

/*
 * Give one idle slot a packet and hand it back to the device.
 *
 * Separate from the sweep below because the interesting caller is
 * ami_sana2_rx_complete(), which re-posts the slot it has just taken a frame
 * out of BEFORE delivering that frame. Every re-arm has to happen here, in
 * front of the BeginIO(), and nowhere after it: the copy hook runs at
 * interrupt level and the device may complete this read before a store made
 * after the BeginIO() would have landed.
 */
static BOOL ami_sana2_rx_post_slot(AmiSana2Rx *rx, AmiRxSlot *slot)
{
    AmiSana2If *iface = rx->iface;

    if (slot->posted)
        return TRUE;

    if (rx->stop || !iface->online)
        return FALSE;

    if (slot->packet == NULL)
    {
        if (nx_packet_allocate(iface->pool, &slot->packet,
                               NX_RECEIVE_PACKET, NX_NO_WAIT) != NX_SUCCESS)
        {
            slot->packet = NULL;
            iface->stats.alloc_failures++;
            return FALSE;
        }
    }

    ami_sana2_rx_arm(iface, slot);

    /*
     * FIVE STORES, NOT NINE.  Only what the round trip actually disturbs is
     * written back here; the rest is established once, where the slot is built
     * (ami_sana2_rx_start below), and nothing between then and here touches
     * it.
     *
     * ln_Type      Exec's ReplyMsg() leaves NT_REPLYMSG behind.  Our own
     *              device puts NT_MESSAGE back at queue time
     *              (netdev_queue.c:15), but a third-party device need not, so
     *              this side restores it.
     * io_Flags     BeginIO() and the device both use IOF_QUICK.  The VALUE is
     *              a constant of the open, so the raw_mode test that used to
     *              build it is gone -- see sana2_internal.h rx_io_flags.
     * io_Error,
     * WireError,
     * PacketType,  the device's four answers about the frame that just
     * DataLength   arrived.  PacketType is an OUT parameter of a cooked
     *              CMD_READ, so it is an input again only once reset.
     *
     * Hoisted, because no code path writes any of them after the slot is
     * built: mn_ReplyPort, io_Command and ios2_Data.  Checked across
     * src/netdev -- the only request field the device assigns is ln_Type.
     */
    slot->req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    slot->req.ios2_Req.io_Flags   = iface->rx_io_flags;
    slot->req.ios2_Req.io_Error   = 0;
    slot->req.ios2_WireError      = 0;
    slot->req.ios2_PacketType     = rx->packet_type;
    slot->req.ios2_DataLength     = 0;
    ami_sana2_rx_mark(rx, slot, TRUE);

    /* BeginIO(), not SendIO(): SendIO() zeroes io_Flags and drops the
       SANA2IOF_RAW just set. Both lines it runs are above. */
#ifdef AMINETXDUO_RXPROBE
    {
        /*
         * The receive side of the transmit path's `post` leg.  `drain` is
         * about 1,620 us a frame and `settle`, the whole IP-to-notify chain
         * inside it, is 462 -- so eleven hundred a frame of the reader's loop
         * has never been timed, and this call is the largest thing in it that
         * is not NetX.  The stamp goes round the whole re-arm, allocate
         * included, because a pool round trip is exactly what it might be.
         */
        ULONG rp0 = ami_budget_clock();

        BeginIO((struct IORequest *)&slot->req);
        ami_budget_repost(ami_budget_clock() - rp0);
    }
    rx->probe.posts++;
    rx->probe.live++;
#else
    BeginIO((struct IORequest *)&slot->req);
#endif

    return TRUE;
}

/* Post every idle slot that has, or can get, a packet. Returns how many reads
   are in flight afterwards. */
static UWORD ami_sana2_rx_post(AmiSana2Rx *rx)
{
    UWORD i;
    UWORD live = 0;

    /*
     * THE SWEEP IS THE STEADY STATE'S DEAD WORK.  This runs once per drain,
     * and ami_sana2_rx_complete() has already re-posted every slot it took a
     * frame out of, so with a full ring all AMI_SANA2_RX_MAX_DEPTH calls fall
     * straight out of ami_sana2_rx_post_slot()'s first line.  At 32 deep and
     * ~4.7 frames a drain that is ~7 wasted calls for every frame received,
     * and _ami_sana2_rx_post_slot carries 2.4% of the real-path profile.
     *
     * Only a slot whose read completed, or whose re-post failed, is idle, and
     * both are counted.  Zero means every slot is in the device's hands.
     *
     * MEASURED on playhouse3/a2065, clean build per arm with the library md5
     * printed before a round ran, six rounds alternating which arm went first,
     * all twelve rc=0:
     *
     *     medians      before       after       delta
     *     tcp-rx       5,496,274    5,669,851   +3.16%
     *       position 1 5,508,966    5,625,025   +2.11%
     *       position 2 5,492,957    5,715,119   +4.04%
     *     tcp-tx       2,975,931    3,133,761   +5.30%
     *
     * AND THE TRANSMIT NUMBER IS REAL, WHICH IS THE SURPRISE.  A null control
     * -- origin/main built clean in BOTH worktrees, md5 7522050095ef on each,
     * the same six alternating rounds -- reads rx +0.22% and tx -0.00%
     * (2,966,517 against 2,966,516).  The null's transmit never leaves
     * 2.94-3.01M in either directory while this arm sits at 3.10-3.15M, six
     * samples against twelve with no overlap.
     *
     * The rig's ~4.5 ms acknowledgement round trip is still there and still
     * dominates; what it does not do is make our share of that trip free.  The
     * sweep sat between an arriving ACK and the window reopening, and during a
     * bulk SEND the drains are shallow -- one or two frames -- so the ring walk
     * was a larger share of each wake than it is on receive.  Read
     * "transmit is closed" as "the rig sets the floor", not as "nothing we do
     * can move it".
     *
     * CONFIRMED ON THE RIG, not just by arithmetic.  An RXPROBE build of this
     * commit, one bulk receive, IPv4 reader:
     *
     *     rxprobe 0: sweeps run 1 skipped 927, unposted 0 recount 0
     *
     * The sweep runs ONCE, at startup, when every slot really is idle, and is
     * skipped every time after -- 927 x 32 = 29,664 calls that no longer
     * happen in one eight second transfer.  `unposted` matched a recount of
     * the posted flags on every sample of every reader, with no mismatch.
     *
     * And under loss, which is the only place a re-post FAILS and the count
     * goes nonzero: run-lossgate.sh, counters build, nine arms, PASS --
     * read_kbs 379.0 against a 385.0 baseline (-1.6%, ok), write_kbs 2,516
     * against 2,138 (+17.7%, ok), retransmitted 0.
     *
     * THE TRANSMIT HALF IS CORROBORATED ON A SECOND WORKLOAD.  The +5.30%
     * above is iperf; the day's four changes measured together on Fitz, six
     * sittings an arm alternated, give fitz_write 2,541.5 -> 2,607.0, +2.58%,
     * with the two arms' ranges DISJOINT -- before [2527..2552] against after
     * [2591..2622].  The payload-alignment change measured fitz_write -0.12%
     * on its own, so that gain is not its; this sweep skip is the only one of
     * the four that moved transmit.
     *
     * CONFIRMED BY A SECOND INDEPENDENT SET: fitz_write 2,546.5 -> 2,603.5,
     * +2.24%, ranges disjoint again (before max 2,558 against after min
     * 2,597).  Two sets agreeing to 0.34pp with no overlap either time.
     *
     * fitz_read IS NOT USABLE AT THIS SIZE AND THE TWO SETS PROVE IT: +2.45%
     * and then -3.38%, opposite signs on the same pair of commits.  Its before
     * arm spanned [2729..3722] in the first set, a 36% range.  Use fitz_write
     * for an effect of a few per cent and do not quote fitz_read for one.
     */
    if (rx->unposted == 0)
    {
#ifdef AMINETXDUO_RXPROBE
        rx->probe.sweep_skip++;
#endif
        return rx->depth;
    }

#ifdef AMINETXDUO_RXPROBE
    rx->probe.sweep_run++;
#endif

    for (i = 0; i < rx->depth; i++)
    {
        if (ami_sana2_rx_post_slot(rx, &rx->slot[i]))
            live++;
    }

#ifdef AMINETXDUO_RXPROBE
    if (live == 0)
        rx->probe.post_zero++;
    else if (live < rx->depth)
        rx->probe.post_partial++;
#endif

    return live;
}

/* ------------------------------------------------------------ completion */

/* Reconcile the device's ios2_DataLength answer with the bytes its CopyToBuff
   call actually initialized.  A smaller reported length invalidates the carried
   checksum; a larger one would expose stale packet-pool bytes. */
/*
 * How long the packet is once the link header in front of the payload is
 * counted.  Non-static and separate from the synthesis above it for the same
 * reason ami_sana2_rx_resolve_length() is: it is the arithmetic that got this
 * wrong, and the host tier can only pin arithmetic it can call.
 *
 * It deliberately does NOT take the slot.  Who wrote the header -- this file
 * or the device answering ANXD_S2_RX_LINK_HDR -- cannot change how long the
 * packet is, and making that impossible to express is the fix.
 */
ULONG ami_sana2_rx_frame_length(const AmiSana2If *iface, ULONG payload)
{
    /*
     * NOT `payload + iface->rx_dst_off`, WHICH IS THE SAME TWO ANSWERS AND
     * WAS TRIED.  rx_dst_off is derived from raw_mode by ami_sana2_open(), so
     * reading it here would make this function's answer depend on whether
     * that open has run -- and the whole reason this arithmetic lives in its
     * own non-static function is that the host tier can call it directly, on
     * an interface it builds itself.  It does, and three checks in
     * test_sana2_rx_host.c caught it at once: a cooked frame silently lost
     * its fourteen bytes.  Two instructions a frame is not worth a function
     * that is only correct after an initialiser it does not name.
     */
    return iface->raw_mode ? payload : (payload + AMI_ETH_HEADER_SIZE);
}

BOOL ami_sana2_rx_resolve_length(AmiRxSlot *slot, ULONG *length)
{
    if (slot == NULL || length == NULL)
        return FALSE;

    if (slot->copied == 0)
        return FALSE;

    if (*length == 0)
        *length = slot->copied;

    if (*length > slot->capacity || *length > slot->copied)
        return FALSE;

#ifdef AMINETXDUO_RX_VERIFY
    if (*length != slot->copied)
        slot->summed = FALSE;
#endif

    return TRUE;
}

/*
 * Turn one completed read into a frame, put the slot back on the wire, and
 * then deliver.
 *
 * The order is the point. The ring used to be re-armed at the top of the outer
 * loop, once the whole drain was done, so every frame in a burst was delivered
 * with one fewer read outstanding than the interface was configured for, and a
 * burst of N left the device N short for the length of N deliveries. A SANA-II
 * device has no buffers: a frame arriving with no CMD_READ outstanding is
 * gone. AmiTCP_NG re-arms inside its per-frame dispatch for the same reason
 * (net/if_sana.c:1620), before the mbuf goes upstream.
 *
 * Re-arming clears the copy hook's accumulator, which the delivery still
 * needs, so what belongs to the frame in hand is lifted into an AmiRxSum
 * first. The failure path re-posts too: the packet is still in the slot, and a
 * slot held back until the next sweep is a read the device does not have.
 */
static VOID ami_sana2_rx_complete(AmiSana2Rx *rx, AmiRxSlot *slot)
{
    AmiSana2If *iface  = rx->iface;
    NX_PACKET  *packet = slot->packet;
    ULONG       length = slot->req.ios2_DataLength;
    AmiRxSum    sum;
    UCHAR      *eth;

    if (packet == NULL)
        return;

    /* ios2_DataLength is the documented answer. Fall back to what the copy
       hook took, for devices that fill only one of the two, but never extend
       the packet past the bytes the hook initialized. */
    if (!ami_sana2_rx_resolve_length(slot, &length))
    {
        /* Keep the packet: rearming is cheaper than a pool round trip. */
        iface->stats.rx_errors++;
        iface->stats.rx_err_length++;
        (VOID)ami_sana2_rx_post_slot(rx, slot);
        return;
    }

    /*
     * THE FOURTEEN BYTES USED TO TRAVEL A LONG WAY FOR WHAT THEY ARE.  The
     * device lifted both addresses out of the frame into ios2_SrcAddr and
     * ios2_DstAddr, and this rebuilt them into the packet and added the type:
     * four six-byte moves and a word, per frame, for bytes the device was
     * holding when it started.  ANXD_S2_RX_LINK_HDR asks it to write the
     * header where the payload's fourteen leading bytes belong instead, and
     * slot->hdr_written says it did.
     *
     * A driver that does not know the tag never sets it, so the synthesis
     * below is still the answer for every other SANA-II device.
     */
    if (!iface->raw_mode && !slot->hdr_written)
    {
        eth = packet->nx_packet_prepend_ptr;

        if (iface->addr_bytes == AMI_ETH_ADDR_SIZE)
        {
            if ((slot->req.ios2_Req.io_Flags & SANA2IOF_BCAST) != 0)
            {
                /* Drivers are inconsistent about what they leave in DstAddr
                   for a broadcast. The flag is the authority. */
                *(ULONG *)(APTR)&eth[0] = 0xFFFFFFFFUL;
                *(UWORD *)(APTR)&eth[4] = 0xFFFFU;
            }
            else
            {
                ami_sana2_addr6(&eth[0], slot->req.ios2_DstAddr);
            }

            ami_sana2_addr6(&eth[6], slot->req.ios2_SrcAddr);
        }
        else
        {
            UWORD i;

            /* No address field on this wire: keep the shape, zero the bytes. */
            for (i = 0; i < 12; i++)
                eth[i] = 0;
        }

        eth[12] = (UCHAR)(slot->req.ios2_PacketType >> 8);
        eth[13] = (UCHAR)(slot->req.ios2_PacketType);
    }

    /*
     * OUTSIDE THE SYNTHESIS, AND 1bbb3803 LEFT IT INSIDE.  The fourteen bytes
     * are in front of the payload in cooked mode whether this function wrote
     * them or the device did; the length is a fact about the PACKET, not about
     * who filled it in.  Narrowing the guard to `&& !slot->hdr_written` took
     * the addition with it, so a device that answers ANXD_S2_RX_LINK_HDR
     * delivered every frame FOURTEEN BYTES SHORT: ami_sana2_rx_deliver() reads
     * the type at prepend_ptr[12], then advances prepend_ptr by fourteen and
     * subtracts fourteen from a length that never had them.
     *
     * NOT REACHED ON THE RIG, WHICH IS WHY IT SURVIVED THE DAY.  hdr_written
     * is set only from ami_sana2_rx_filled(), which is the DIRECT path, and
     * the a2065 does not use it -- lance.c hands up a whole frame out of board
     * SRAM and never claims (lance.c:296).  dp8390 and el3 do claim
     * (dp8390.c:366, el3.c:693), so this was every frame on an ne2000, an
     * X-Surf or an Ariadne II.
     */
    length = ami_sana2_rx_frame_length(iface, length);

    packet->nx_packet_length     = length;
    packet->nx_packet_append_ptr = packet->nx_packet_prepend_ptr + length;

    /*
     * Frame arrival time is the only thing this machine knows that is not in
     * its own boot image; see the arrival section of src/common/ami_random.c.
     *
     * THE WIRE PROFILE'S `_sha256_k 0.7%` IS NOT THIS CALL, and the arithmetic
     * says so without a rig.  ami_random.c batches 16 arrivals and credits 8
     * bits (ARRIVAL_BATCH, ARRIVAL_BITS_KEPT), and arrival_done latches at 64
     * bits (AMI_RANDOM_ARRIVAL_MAX_BITS) -- so the pool is mixed EIGHT times,
     * inside the first 128 frames, and every arrival after that returns on the
     * first line.  A ten second transfer is about 4,300 frames, so this path
     * hashes for under 3% of them and eight SHA-256 mixes cannot be 0.7% of a
     * profile.  Those samples are a symbol that did not resolve, landing on
     * the nearest one below it -- a constant table.  Do not spend an
     * AMINETXDUO_LOG build chasing it; the counter that would confirm the
     * latch is an AMI_INFO and none of the 514 measurement logs on the rig
     * carry it.
     */
    ami_random_arrival();

    /* Everything the delivery needs out of the slot, before the slot is
       re-armed under it. */
    sum.copied = slot->copied;
#ifdef AMINETXDUO_RX_VERIFY
    sum.sum    = slot->sum;
    sum.summed = slot->summed;
#else
    sum.sum    = 0;
    sum.summed = FALSE;
#endif

    slot->packet = NULL;     /* ownership passes to NetX Duo */

    /* Back on the wire before the frame goes upstream, not after. */
    (VOID)ami_sana2_rx_post_slot(rx, slot);

#ifdef AMINETXDUO_RXPROBE
    {
        ULONG t0 = ami_budget_clock();

        ami_sana2_rx_deliver(iface, packet, &sum);
        ami_budget_drain(ami_budget_clock() - t0);
    }
#else
    ami_sana2_rx_deliver(iface, packet, &sum);
#endif
}

/*
 * Take up to `budget` replies off the port and return how many there were.
 *
 * Bounded, and not by accident.  ami_sana2_rx_complete() puts each slot back on
 * the wire before it delivers the frame, so a reply that arrives while this
 * loop is running joins the queue it is draining: on a saturated link the
 * device can keep it fed for as long as the far end has a window, and this
 * thread outranks everything and now runs TCP itself.  The budget is what
 * returns it to the top of the loop, where the baton bracket lets the task
 * that empties the socket run.
 *
 * Errors and control frames count against it: they cost reader time too.
 */
static UWORD ami_sana2_rx_drain(AmiSana2Rx *rx, UWORD budget)
{
    NX_IP          *ip = rx->iface->ip;
    struct Message *msg;
    TX_THREAD      *outer;
    UWORD           took = 0;

#ifdef AMINETXDUO_RXPROBE
    {
        AmiRxProbe *pr      = &rx->probe;
        UWORD       backlog = ami_rxprobe_backlog(rx->port);
        UWORD       avail;

        if (backlog > AMI_SANA2_RX_MAX_DEPTH)
            backlog = AMI_SANA2_RX_MAX_DEPTH;

        avail = (UWORD)((pr->live > (ULONG)backlog)
                            ? (pr->live - (ULONG)backlog) : 0UL);
        if (avail > AMI_SANA2_RX_MAX_DEPTH)
            avail = AMI_SANA2_RX_MAX_DEPTH;

        if (backlog != 0)
        {
            ULONG now  = ami_rxprobe_clock();
            ULONG span = now - pr->last_wake;

            pr->last_wake = now;
            pr->drains++;
            pr->backlog_hist[backlog]++;
            pr->avail_hist[avail]++;
            if (avail == 0)
                pr->dry++;

            /* Keep the deepest, smallest first, so one pass is enough. */
            if (backlog > pr->worst_backlog[0])
            {
                UWORD k = 0;

                while (k + 1 < AMI_RXPROBE_WORST &&
                       backlog > pr->worst_backlog[k + 1])
                {
                    pr->worst_backlog[k] = pr->worst_backlog[k + 1];
                    pr->worst_avail[k]   = pr->worst_avail[k + 1];
                    pr->worst_when[k]    = pr->worst_when[k + 1];
                    pr->worst_baton[k]   = pr->worst_baton[k + 1];
                    k++;
                }

                pr->worst_backlog[k] = backlog;
                pr->worst_avail[k]   = avail;
                pr->worst_when[k]    = span;
                pr->worst_baton[k]   = pr->baton_last;
            }
        }

        rx->iface->seq.avail = avail;
    }
#endif

    /*
     * One lock for the whole run, not one per frame.
     *
     * nx_ip_thread_entry.c holds nx_ip_protection across its entire event loop
     * and gives it back only when it is about to wait, and this loop is the
     * same shape: every frame it takes walks the same fragment list, the same
     * ARP cache and the same counters, and taking and giving back the mutex
     * between each of them buys nothing but the round trips.  The run is
     * bounded by `budget` above, which is what stops the hold from becoming
     * open-ended.
     *
     * Claiming the IP thread's seat goes with it: _nx_tcp_packet_receive()
     * then processes each segment here instead of queueing it and waking a
     * thread that is blocked on the mutex this one holds.  See
     * NX_TCP_PACKET_RECEIVE_DIRECT in port/netxduo-amiga/inc/nx_user.h.  The
     * saved outer value is not decoration: a loopback send from inside input
     * re-enters, and the nested call must not give the seat up on its way out.
     *
     * What the reader must never do while holding this is block in exec, which
     * would release the ThreadX baton with the IP mutex held.  Nothing on the
     * path does: the only device call it reaches is BeginIO(), which queues
     * and returns, and the packet allocation is NX_NO_WAIT.
     */
    /*
     * NOT TX_WAIT_FOREVER.  ami_sana2_rx_stop() runs inside
     * _nx_ip_driver_interface_direct_command(), which holds this same mutex
     * across the whole NX_LINK_DISABLE and then waits seconds for this thread
     * to put its "exited" semaphore.  A reader woken by that stop always
     * reaches this line before it retests rx->stop, so waiting forever here IS
     * the deadlock: the reader waits for the mutex, the stopper waits for the
     * reader, and the interface comes back orphaned.  A stop is also the one
     * thing this run has nothing left to do for -- the loop below drops every
     * message on rx->stop anyway -- so give the mutex up and let the loop end.
     */
    while (tx_mutex_get(&ip->nx_ip_protection,
                        AMI_SANA2_RX_LOCK_TICKS) != TX_SUCCESS)
    {
        if (rx->stop)
            return took;
    }

    /* Set while this was waiting: the same, minus the round trip below. */
    if (rx->stop)
    {
        tx_mutex_put(&ip->nx_ip_protection);
        return took;
    }

    outer = _nx_ip_input_thread;
    _nx_ip_input_thread = tx_thread_identify();

    while (took < budget && (msg = GetMsg(rx->port)) != NULL)
    {
#ifdef AMINETXDUO_RXPROBE
        if (rx->probe.live != 0)
            rx->probe.live--;
#endif
        /* The reply message is the slot: ios2_Req.io_Message is its first
           member's first member. */
        AmiRxSlot *slot = (AmiRxSlot *)msg;
        /*
         * THE BYTE IS TESTED, THE SIGN EXTENSION IS NOT ON THIS PATH.  It was
         * `LONG err = (LONG)(BYTE)...` before the branch, and the drain loop
         * then ran `move.b d6,d1; extb.l d1` on EVERY frame for a value only
         * the error arms read -- the branch below tests the byte either way.
         * The widening still happens where io_Error is compared against the
         * negative Exec codes, which is what it is for.
         */
        UBYTE      raw  = slot->req.ios2_Req.io_Error;

        ami_sana2_rx_mark(rx, slot, FALSE);
        took++;

        if (rx->stop)
            continue;

        if (raw == 0)
        {
            ami_sana2_rx_complete(rx, slot);
        }
        else if ((LONG)(BYTE)raw == (LONG)IOERR_ABORTED)
        {
            /* Asked for here, so nothing to count. */
        }
        else if ((LONG)(BYTE)raw == (LONG)S2ERR_OUTOFSERVICE)
        {
            /*
             * NetX Duo learns link state only from NX_LINK_ENABLE/DISABLE, so
             * an out-of-service device must be marked down here or the
             * interface stays up with every send failing.  `Online` recovers it.
             */
            rx->iface->online = FALSE;

            if (rx->iface->interface_ptr != NULL)
                rx->iface->interface_ptr->nx_interface_link_up = NX_FALSE;

            ami_event(NETEVENT_OUT_OF_SERVICE, (UWORD)rx->iface->index, 0UL);

            AMI_WARN("sana2: %s went out of service. The link is marked down",
                     rx->iface->device);
        }
        else
        {
            rx->iface->stats.rx_errors++;
            rx->iface->stats.rx_err_io++;
        }
    }

    _nx_ip_input_thread = outer;
    tx_mutex_put(&ip->nx_ip_protection);

    return took;
}

#ifndef AMINETXDUO_GREEN_REALM
/*
 * Is a completion already queued?  One pointer read of the port's list, and a
 * miss costs nothing that matters: a reply landing right after it sets the
 * port's signal, so the Wait() this answer sends the reader into returns at
 * once.
 */
static BOOL ami_sana2_rx_queued(const AmiSana2Rx *rx)
{
    const struct List *list = &rx->port->mp_MsgList;

    return (BOOL)(list->lh_Head->ln_Succ != NULL);
}

/*
 * Must the reader block?  THE PORT DECIDES AND NOTHING ELSE.
 *
 * `taken` is how many completions this turn has already delivered, and it is
 * deliberately not consulted: an Exec signal is one bit and not a count, so
 * the arrival that set it was consumed by the Wait() that woke this thread
 * and the completions still on the port carry no wake of their own.  A rule
 * that blocks on a batch bound therefore sleeps on data the reader is
 * holding, and nothing releases it until the NEXT frame arrives -- which at
 * the end of a response is the peer's retransmit timer.  0.26.0 and 0.26.1
 * shipped that rule and read throughput fell to about a quarter.
 *
 * It stays in the signature so this is the one place the question is asked,
 * and so a test can pin the answer against every batch count.
 */
BOOL ami_sana2_rx_should_block(const AmiSana2Rx *rx, UWORD taken)
{
    (VOID)taken;

    return (BOOL)(!ami_sana2_rx_queued(rx));
}
#endif

/* --------------------------------------------------------------- shutdown */

/*
 * CMD_FLUSH: "abort and return all queued I/O requests for this unit."
 * Unit-wide rather than per-request, so it is tried second: it takes the other
 * reader's queued reads with it.
 */
static VOID ami_sana2_rx_flush(AmiSana2Rx *rx)
{
    struct MsgPort   *port;
    struct IOSana2Req req;

    port = CreateMsgPort();
    if (port == NULL)
        return;

    req = rx->iface->templ;
    req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req.ios2_Req.io_Message.mn_ReplyPort    = port;
    req.ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(struct IOSana2Req);
    req.ios2_Req.io_Command                 = CMD_FLUSH;
    req.ios2_Req.io_Error                   = 0;
    req.ios2_WireError                      = 0;

    /* Runs on the reader.  ami_sana2_do_io() picks the shape: DoIO() inside the
       baton bracket, or SendIO() plus a green-thread sleep. */
    (VOID)ami_sana2_do_io((struct IORequest *)&req);

    DeleteMsgPort(port);
}

/*
 * Collect whatever the device has given back, waiting up to `tries` ticks of
 * 40 ms, and return how many requests it still owns.  AbortIO() is only a
 * request: a2065.device 2.16 does not honour it on a queued CMD_READ.
 */
static UWORD ami_sana2_rx_reap(AmiSana2Rx *rx, UWORD tries)
{
    UWORD outstanding;
    UWORD i;
    UWORD t = 0;

    for (;;)
    {
        struct Message *msg;

        while ((msg = GetMsg(rx->port)) != NULL)
        {
            ami_sana2_rx_mark(rx, (AmiRxSlot *)msg, FALSE);
#ifdef AMINETXDUO_RXPROBE
            if (rx->probe.live != 0)
                rx->probe.live--;
#endif
        }

        outstanding = 0;
        for (i = 0; i < rx->depth; i++)
        {
            if (rx->slot[i].posted)
                outstanding++;
        }

        if (outstanding == 0 || t >= tries)
            break;

        t++;
        tx_thread_sleep(2);
    }

    return outstanding;
}

/*
 * Reclaim every outstanding read, or report how many are still held: AbortIO(),
 * then CMD_FLUSH, then give up having freed nothing the device can still write
 * into.  Freeing the port, the packets or the interface corrupts memory.
 */
static VOID ami_sana2_rx_teardown(AmiSana2Rx *rx)
{
    UWORD outstanding;
    UWORD i;

    for (i = 0; i < rx->depth; i++)
    {
        if (rx->slot[i].posted)
            AbortIO((struct IORequest *)&rx->slot[i].req);
    }

    outstanding = ami_sana2_rx_reap(rx, AMI_SANA2_RX_REAP_TRIES);

    if (outstanding != 0)
    {
        AMI_WARN("sana2: %ld read(s) survived AbortIO; trying CMD_FLUSH",
                 (long)outstanding);
        ami_sana2_rx_flush(rx);
        outstanding = ami_sana2_rx_reap(rx, AMI_SANA2_RX_REAP_TRIES);
    }

    rx->orphans = outstanding;

    if (outstanding != 0)
    {
        /* Nothing below this line can run: each of those is a pointer the
           device still holds. */
        AMI_ERROR("sana2: %ld read(s) still owned by the device. The "
                  "reader leaks. A free here corrupts memory",
                  (long)outstanding);
        return;
    }

    for (i = 0; i < rx->depth; i++)
    {
        if (rx->slot[i].packet != NULL)
        {
            nx_packet_release(rx->slot[i].packet);
            rx->slot[i].packet = NULL;
        }
    }

    if (rx->port != NULL)
    {
        DeleteMsgPort(rx->port);
        rx->port = NULL;
    }
}

/* ----------------------------------------------------------- reader thread */

static VOID ami_sana2_rx_thread(ULONG argument)
{
    AmiSana2Rx *rx    = (AmiSana2Rx *)argument;
    AmiSana2If *iface = rx->iface;
    UWORD       i;

    rx->task = FindTask(NULL);
    rx->port = CreateMsgPort();

    if (rx->port == NULL)
    {
        rx->failed = TRUE;
        tx_semaphore_put(&rx->ready);
        tx_semaphore_put(&rx->exited);
        return;
    }

    rx->wake_mask = 1UL << rx->port->mp_SigBit;

    /*
     * TX reaping duty.  One reader takes it, which gives the transmit ring a
     * context that runs when nothing is being sent.  Not fatal if it fails: the
     * interface falls back to reaping on the next transmit.
     */
    rx->reap_sigbit = -1;
    rx->reap_mask   = 0;

    if (rx->reap_tx)
    {
        BYTE bit = AllocSignal(-1);

        if (bit >= 0)
        {
            rx->reap_sigbit = bit;
            rx->reap_mask   = 1UL << (ULONG)bit;
            ami_sana2_tx_reap_bind(iface, rx->task, bit);
        }
        else
        {
            AMI_WARN("sana2: no signal for TX reaping. Retransmission will "
                     "wait for the next send");
        }
    }

    /*
     * Every request is a copy of the opened one, which carries io_Device,
     * io_Unit and the device's own ios2_BufferManagement cookie.
     */
    for (i = 0; i < rx->depth; i++)
    {
        rx->slot[i].req   = iface->templ;
        rx->slot[i].owner = rx;
        rx->slot[i].stats = &iface->stats;
        rx->slot[i].req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        rx->slot[i].req.ios2_Req.io_Message.mn_ReplyPort    = rx->port;
        rx->slot[i].req.ios2_Req.io_Message.mn_Length =
            (UWORD)sizeof(struct IOSana2Req);
        /* Invariants of the slot, so that the re-arm on the hot path does not
           write them once a frame.  ami_sana2_rx_post_slot() names them. */
        rx->slot[i].req.ios2_Req.io_Command = CMD_READ;
        rx->slot[i].req.ios2_Data           = &rx->slot[i];
    }

    ami_sana2_rx_mark_reset(rx);

#ifdef AMINETXDUO_RXPROBE
    /* TimerBase is opened lazily. The probe's clock needs it before the first
       drain, not after. */
    (VOID)ami_millis();
#endif

    rx->running = TRUE;
    tx_semaphore_put(&rx->ready);

    while (!rx->stop)
    {
        /*
         * At the top of the loop, not after the Wait(): the pool-empty path
         * below continues without reaching a drain, and releasing finished
         * writes is what returns packets to the pool it is waiting for.
         */
        if (rx->reap_mask != 0)
        {
            /*
             * Reap here rather than ami_sana2_tx_defer(). Deferring sets the
             * driver event flag, which wakes the IP thread, which takes
             * nx_ip_protection, enters the driver at
             * NX_LINK_DEFERRED_PROCESSING and calls exactly this function
             * (sana2_driver.c:496) -- a thread wake, a mutex round trip and a
             * dispatch, per completion signal, to run a walk this thread can
             * run itself. ami_sana2_tx_reap() is documented callable from any
             * thread and from several at once (sana2_tx.c:188): GetMsg() is
             * atomic and nx_packet_transmit_release() does its own TX_DISABLE.
             * ami_sana2_tx_send() already calls it directly for that reason.
             *
             * It also removes a race with the comment above: the pool-empty
             * path below needs the finished writes released BEFORE it retries
             * ami_sana2_rx_post(), and a deferred reap had not necessarily run
             * by then.
             */
            ami_sana2_tx_reap(iface);
        }

        if (ami_sana2_rx_post(rx) == 0)
        {
            /* Either the pool is empty or the interface is down. Back off
               rather than spin. ami_sana2_rx_stop() signals out of this. */
            tx_thread_sleep(2);
            continue;
        }

#ifdef AMINETXDUO_GREEN_REALM
        /*
         * The reader is a green thread: only IT sleeps, while the realm keeps
         * running the IP thread.  There is no baton bracket on this path, so
         * the probe's baton leg reads zero here by design.
         */
        /* EITHER WAY: the mask that comes back is not read.  The loop
           re-tests the port and the reap list on the next pass, and they are
           the authority whether this returned the signals, none of them, or
           refused outright. */
        AMI_NX_EITHER_WAY(tx_amiga_green_wait(rx->wake_mask | rx->reap_mask));
#else
        /*
         * Block ONLY when there is nothing to take.  The bracket is a
         * Forbid(), a ThreadX suspend, a dispatch and the reverse of all
         * three, and during a burst the device has usually replied again
         * while the last frame was being delivered, so the Wait() it wraps
         * would have returned at once; one pointer read of the port's message
         * list decides, and a miss costs nothing because the reply that lands
         * right after it sets the port's signal.
         *
         * What may NOT join that condition is a fairness bound.  An Exec
         * signal is one bit and not a count: the arrival that set it was
         * consumed by the Wait() that woke this thread, so completions
         * already sitting on the port carry no wake of their own.  Blocking
         * with the port non-empty sleeps on data this thread is holding, and
         * nothing releases it until the NEXT frame arrives -- which at the
         * end of a response is the peer's retransmit timer.  0.26.0 and
         * 0.26.1 shipped that bound and read throughput fell from 938 to 257
         * KB/s on a request/response workload, transmit untouched.
         *
         * The bound survives where it is correct and cheap: as the batch size
         * of one drain, below, so the loop re-reaches ami_sana2_rx_post() and
         * re-arms every AMI_SANA2_RX_RUN_MAX frames instead of running the
         * ring dry.  Fairness cannot be bought here with a blocking call --
         * a ThreadX tick is 20 ms (NX_IP_PERIODIC_RATE 50), which is worse
         * than the monopoly it would prevent.
         */
        if (ami_sana2_rx_should_block(rx, AMI_SANA2_RX_RUN_MAX))
        {
            ami_sana2_block_enter();
            Wait(rx->wake_mask | rx->reap_mask);
#ifdef AMINETXDUO_RXPROBE
            {
                AmiRxProbe *pr = &rx->probe;
                ULONG       t0 = ami_rxprobe_clock();
                ULONG       dt;

                ami_sana2_block_leave();

                dt = ami_rxprobe_clock() - t0;
                pr->baton_sum += dt;
                pr->baton_last = dt;
                if (dt > pr->baton_max)
                    pr->baton_max = dt;
                pr->baton_hist[ami_rxprobe_bucket(dt)]++;
            }
#else
            ami_sana2_block_leave();
#endif
        }
#endif /* AMINETXDUO_GREEN_REALM */

        (VOID)ami_sana2_rx_drain(rx, (UWORD)AMI_SANA2_RX_RUN_MAX);

#ifdef AMINETXDUO_RXPROBE
        /* Keep probe_dev_rx within a few hundred frames of the truth: the
           report runs from NetStat, which cannot issue a device command. */
        if (rx->reap_tx && (rx->probe.drains & 31UL) == 0UL)
            ami_sana2_refresh_stats(iface);
#endif
    }

    /*
     * Hand the reply port back before anything else in the teardown: after this
     * returns no completion can signal this task, which makes freeing the
     * signal bit, and shortly the Task, safe.
     */
    if (rx->reap_mask != 0)
    {
        ami_sana2_tx_reap_unbind(iface);
        rx->reap_mask = 0;
    }

    if (rx->reap_sigbit >= 0)
    {
        FreeSignal(rx->reap_sigbit);
        rx->reap_sigbit = -1;
    }

    ami_sana2_rx_teardown(rx);

    rx->running = FALSE;
    tx_semaphore_put(&rx->exited);
}

/* ------------------------------------------------------------ start / stop */

static const ULONG ami_sana2_rx_types[AMI_SANA2_RX_READERS] =
{
    AMI_ETHERTYPE_IPV4,
    AMI_ETHERTYPE_ARP
#ifdef AMINETXDUO_IPV6
, AMI_ETHERTYPE_IPV6
#endif
};

static const CHAR *const ami_sana2_rx_names[AMI_SANA2_RX_READERS] =
{
    "sana2 rx ip",
    "sana2 rx arp"
#ifdef AMINETXDUO_IPV6
, "sana2 rx ip6"
#endif
};

/* ------------------------------------------------------------- read depth */

/*
 * The most reads a wire this slow can ever need.  This table caps rather than
 * scales with the reported line rate: the CPU, not the wire, is the binding
 * constraint at every Ethernet rate these machines see.
 */
typedef struct AmiRxSpeedStep
{
    ULONG   bps;        /* the fastest wire this row covers */
    UWORD   depth;      /* the most reads it can ever need  */
} AmiRxSpeedStep;

static const AmiRxSpeedStep ami_sana2_rx_ladder[] =
{
    {     4000000UL,  AMI_SANA2_RX_DEPTH_IPV4 },
    { 0xFFFFFFFFUL,   AMI_SANA2_RX_MAX_DEPTH  }
};

static UWORD ami_sana2_rx_wire_depth(ULONG bps)
{
    UWORD i;
    UWORD last = (UWORD)(sizeof(ami_sana2_rx_ladder) /
                         sizeof(ami_sana2_rx_ladder[0]) - 1);

    /* 0 is "the device did not say": it left BPS alone, or supplied a short
       block that stopped before it.  The bottom step covers every nonsense
       value; 0xFFFFFFFF falls off the top and is treated as very fast. */
    if (bps == 0)
        bps = (ULONG)AMI_SANA2_BPS_DEFAULT;

    for (i = 0; i < last; i++)
    {
        if (bps <= ami_sana2_rx_ladder[i].bps)
            return ami_sana2_rx_ladder[i].depth;
    }

    return ami_sana2_rx_ladder[last].depth;
}

/*
 * What each reader gets: the smaller of what the wire asks for and what the
 * pool can spare.  Each outstanding read pins one NX_PACKET, so the budget is
 * over all readers together; the per-reader floors are never given up.
 */
static UWORD ami_sana2_rx_window_depth(ULONG pool_total)
{
    ULONG frames = pool_total / (ULONG)AMI_SANA2_RX_POOL_SHARE;

    if (frames < (ULONG)AMI_SANA2_RX_DEPTH_IPV4)
        frames = (ULONG)AMI_SANA2_RX_DEPTH_IPV4;
    if (frames > (ULONG)AMI_SANA2_RX_MAX_DEPTH)
        frames = (ULONG)AMI_SANA2_RX_MAX_DEPTH;

    return (UWORD)frames;
}

VOID ami_sana2_rx_plan(ULONG bps, ULONG pool_total, BOOL dual_stack,
                       UWORD ifaces, AmiRxDepths *out)
{
    UWORD want;
    UWORD cap;
    ULONG budget;
    ULONG floors;
    ULONG spare;
    UWORD give;

    if (out == NULL)
        return;

    /* Nought interfaces is the caller that has not counted, and it is this
       interface asking, so there is at least one. */
    if (ifaces == 0)
        ifaces = 1;

    want = ami_sana2_rx_window_depth(pool_total);
    cap  = ami_sana2_rx_wire_depth(bps);
    if (want > cap)
        want = cap;

    out->ipv4 = (UWORD)AMI_SANA2_RX_DEPTH_IPV4;
    out->arp  = (UWORD)AMI_SANA2_RX_DEPTH_ARP;
    out->ipv6 = dual_stack ? (UWORD)AMI_SANA2_RX_DEPTH_IPV6 : (UWORD)0;

    /* The share is the machine's and it is divided here, not multiplied
       elsewhere: one interface gets exactly what it always got. */
    budget = pool_total /
             ((ULONG)AMI_SANA2_RX_BUDGET_SHARE * (ULONG)ifaces);
    floors = (ULONG)out->ipv4 + (ULONG)out->arp + (ULONG)out->ipv6;
    spare  = (budget > floors) ? (budget - floors) : 0UL;

    give = (want > out->ipv4) ? (UWORD)(want - out->ipv4) : (UWORD)0;
    if ((ULONG)give > spare)
        give = (UWORD)spare;
    out->ipv4 = (UWORD)(out->ipv4 + give);
    spare    -= (ULONG)give;

    if (dual_stack)
    {
        UWORD want6 = want;

        if (want6 > (UWORD)AMI_SANA2_RX_WANT_IPV6)
            want6 = (UWORD)AMI_SANA2_RX_WANT_IPV6;

        give = (want6 > out->ipv6) ? (UWORD)(want6 - out->ipv6) : (UWORD)0;
        if ((ULONG)give > spare)
            give = (UWORD)spare;
        out->ipv6 = (UWORD)(out->ipv6 + give);
    }
}

/*
 * tx_thread_create() refuses a stack that lies inside one it still has on its
 * created list, and a dead Task's adopted thread stays on that list holding a
 * range the allocator has since given back.  Refused blocks are held, not freed.
 */
#define AMI_SANA2_STACK_TRIES   8

static APTR ami_sana2_alloc_stack(ULONG size)
{
    APTR  held[AMI_SANA2_STACK_TRIES];
    APTR  stack = NULL;
    UWORD n     = 0;
    UWORD i;

    while (n < (UWORD)AMI_SANA2_STACK_TRIES)
    {
        stack = ami_alloc_flags(size, MEMF_PUBLIC);
        if (stack == NULL)
            break;

        if (tx_amiga_stack_in_use(stack, size) != (UINT)TX_TRUE)
            break;

        held[n++] = stack;
        stack     = NULL;
    }

    for (i = 0; i < n; i++)
        ami_free(held[i]);

    if (stack == NULL && n != 0)
        AMI_ERROR("sana2: no reader stack clear of a dead task's registration "
                  "in %ld tries", (long)n);

    return stack;
}

LONG ami_sana2_rx_start(AmiSana2If *iface)
{
    UWORD       i;
    AmiRxDepths depths;
    UINT        txstatus;

    if (iface->rx_running)
        return 0;

    /* A reader the device still owns cannot be re-created on top of. */
    if (iface->rx_orphaned)
    {
        AMI_ERROR("sana2: interface has orphaned readers. "
                  "It is not restarted");
        return -1;
    }

    if (iface->pool == NULL || iface->ip == NULL)
        return -1;

    ami_sana2_rx_plan(iface->bps, iface->pool->nx_packet_pool_total,
                      (BOOL)(AMI_SANA2_RX_READERS == 3),
                      ami_sana2_bound_count(), &depths);

    AMI_INFO("sana2: read queues ip %ld arp %ld ip6 %ld "
             "(pool %ld packets, %ld bps, %ld interface(s))",
             (long)depths.ipv4, (long)depths.arp, (long)depths.ipv6,
             (long)iface->pool->nx_packet_pool_total, (long)iface->bps,
             (long)ami_sana2_bound_count());

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        rx->iface       = iface;
        rx->packet_type = ami_sana2_rx_types[i];
        if (ami_sana2_rx_types[i] == AMI_ETHERTYPE_IPV4)
            rx->depth = depths.ipv4;
        else if (ami_sana2_rx_types[i] == AMI_ETHERTYPE_ARP)
            rx->depth = depths.arp;
        else
            rx->depth = depths.ipv6;
        rx->stop        = FALSE;
        rx->failed      = FALSE;
        rx->running     = FALSE;
        rx->started     = FALSE;
        rx->reap_sigbit = -1;
        rx->reap_mask   = 0;
        /* Stale from the previous run: ami_sana2_rx_stop() Signal()s this mask
           at rx->task, and a reader that then fails to get a MsgPort is
           signalled on a bit it does not hold. */
        rx->wake_mask   = 0;
        rx->orphans     = 0;

        /* The first reader carries the TX reaping duty. It is the IPv4 one,
           the reader that always exists, but nothing depends on which: any
           thread that blocks in exec Wait() serves. */
        rx->reap_tx     = (i == 0) ? TRUE : FALSE;

        if (rx->depth > AMI_SANA2_RX_MAX_DEPTH)
            rx->depth = AMI_SANA2_RX_MAX_DEPTH;

        rx->stack = ami_sana2_alloc_stack((ULONG)AMI_SANA2_RX_STACK_SIZE);
#ifdef AMINETXDUO_RXPROBE
        /*
         * Lay a pattern in before the thread exists, so the report below can
         * say how deep the reader actually went.  The figure in the header
         * over AMI_SANA2_RX_STACK_SIZE came from an ad-hoc probe that was
         * never committed, and it was taken with OUR driver bound -- the
         * board's own .device runs its BeginIO() on this stack too, and no
         * static pass can see that call.  There is no MMU, so an overrun is
         * silent corruption somewhere unrelated.
         */
        if (rx->stack != NULL)
        {
            ULONG w;

            for (w = 0; w < (ULONG)AMI_SANA2_RX_STACK_SIZE; w++)
                ((UBYTE *)rx->stack)[w] = 0xA5;
        }
#endif
        if (rx->stack == NULL)
        {
            AMI_ERROR("sana2: no memory for reader stack");
            ami_sana2_rx_stop(iface);
            return -1;
        }

        /*
         * One at a time, not `a() || b()`: the control blocks live inside the
         * ami_alloc()ed AmiSana2If, so a `ready` left created while `exited`
         * failed stays on ThreadX's created list after the interface is freed.
         */
        if (tx_semaphore_create(&rx->ready, (CHAR *)"s2rxrdy", 0) != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader semaphores");
            ami_sana2_rx_stop(iface);
            return -1;
        }

        if (tx_semaphore_create(&rx->exited, (CHAR *)"s2rxend", 0) != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader semaphores");
            tx_semaphore_delete(&rx->ready);
            ami_sana2_rx_stop(iface);
            return -1;
        }

        txstatus = tx_thread_create(&rx->thread, (CHAR *)ami_sana2_rx_names[i],
                                    ami_sana2_rx_thread, (ULONG)rx,
                                    rx->stack, AMI_SANA2_RX_STACK_SIZE,
                                    AMI_SANA2_RX_PRIORITY,
                                    AMI_SANA2_RX_PRIORITY,
                                    TX_NO_TIME_SLICE, TX_AUTO_START);
        if (txstatus != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader %ld (%ld), stack %lx",
                      (long)i, (long)txstatus, (unsigned long)rx->stack);
            tx_semaphore_delete(&rx->ready);
            tx_semaphore_delete(&rx->exited);
            ami_sana2_rx_stop(iface);
            return -1;
        }

        rx->started = TRUE;

        /* Wait for the reader to own a MsgPort before posting anything. */
        if (tx_semaphore_get(&rx->ready, NX_IP_PERIODIC_RATE) != TX_SUCCESS ||
            rx->failed)
        {
            AMI_ERROR("sana2: reader %ld failed to start", (long)i);
            ami_sana2_rx_stop(iface);
            return -1;
        }
    }

#ifdef AMINETXDUO_TX_LAZY_COLLECT
    /* The readers exist and the first carries the reaping duty, so the lazy
       parking's safety net can go live. Not fatal if it cannot: parking
       stays disengaged and completions signal as shipped. */
    ami_sana2_tx_lazy_start(iface);
#endif

    iface->rx_running = TRUE;
    return 0;
}

VOID ami_sana2_rx_stop(AmiSana2If *iface)
{
    UWORD i;
    ULONG zombies;

#ifdef AMINETXDUO_TX_LAZY_COLLECT
    /* Before the readers unwind: the timer defers into iface->ip and holds a
       pointer into this interface, so it goes first, and the port goes back
       to signalling for whatever completions the teardown still collects. */
    ami_sana2_tx_lazy_stop(iface);
#endif

#ifdef AMINETXDUO_RXPROBE
    ami_sana2_rxprobe_report(iface);
#endif

    /*
     * Three phases, in this order: set `stop` so the reader stops posting, then
     * S2_OFFLINE (the only thing that returns queued reads on a device that
     * ignores AbortIO), then join.  Offline first orphans the reader.
     */
    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (!rx->started)
            continue;

        rx->stop = TRUE;

        /* Wake the reader out of Wait(). A signal on a bit the reader has
           already freed with its MsgPort is harmless. The Task stays alive
           until it has put the "exited" semaphore. */
        if (rx->task != NULL && rx->wake_mask != 0)
            Signal(rx->task, rx->wake_mask);
    }

    (VOID)ami_sana2_offline(iface);

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (rx->started)
        {
            if (tx_semaphore_get(&rx->exited,
                                 5 * NX_IP_PERIODIC_RATE) != TX_SUCCESS)
            {
                /*
                 * The thread is running on `rx->stack` and its control block is
                 * live, so neither can be freed: a 4 KB leak is recoverable, a
                 * thread that runs on freed memory is not.
                 */
                AMI_ERROR("sana2: reader %ld did not stop. Its stack leaks. A "
                          "free here corrupts memory the reader runs on",
                          (long)i);
                iface->rx_orphaned = TRUE;
                continue;
            }

            /*
             * ami_sana2_rx_teardown() kept the reply port because the device
             * would not give every read back.  That port's mp_SigTask is this
             * Task, so the Task must outlive it: no terminate, delete or free.
             */
            if (rx->orphans != 0)
            {
                AMI_ERROR("sana2: reader %ld left %ld read(s) with the "
                          "device. Its thread and stack leak. The reply "
                          "port they "
                          "will complete through signals that Task",
                          (long)i, (long)rx->orphans);
                iface->rx_orphaned = TRUE;
                continue;
            }

            /*
             * Give the thread time to run off the end of its entry function
             * before the control block and stack go away.
             */
            tx_thread_sleep(5);

            /* The total is monotonic.  The live gauge can stay unchanged if
               an older zombie exits while this delete creates a new one,
               which would make this code free the new zombie's live stack. */
            zombies = tx_amiga_zombie_tasks();

            tx_thread_terminate(&rx->thread);
            tx_thread_delete(&rx->thread);

            /*
             * tx_thread_delete() gives up after two seconds, and a failure
             * leaves a zombie running on rx->stack.  The monotonic zombie count
             * is the signal; the live gauge can be cancelled by an older exit.
             */
            if (tx_amiga_zombie_tasks() != zombies)
            {
                AMI_ERROR("sana2: reader %ld cannot be removed. Its stack "
                          "leaks. A free here corrupts memory the reader "
                          "runs on",
                          (long)i);
                iface->rx_orphaned = TRUE;
                continue;
            }

            tx_semaphore_delete(&rx->ready);
            tx_semaphore_delete(&rx->exited);
        }

        /*
         * Outside the started gate: a reader whose semaphores or thread would
         * not create has a stack and nothing else, and ami_sana2_rx_start()
         * unwinds by calling this.
         */
        if (rx->stack != NULL)
        {
            ami_free(rx->stack);
            rx->stack = NULL;
        }

        rx->started = FALSE;
        rx->task    = NULL;
    }

    iface->rx_running = FALSE;
}
