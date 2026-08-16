/*
 * AmiNetXDuo, SANA-II device setup, control commands and statistics.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/anxnet.h"
#include "aminetxduo/netstack.h"

#include <exec/memory.h>
#include <proto/exec.h>
/* BeginIO(): a macro over the device's own vector, no amiga.lib to link. */
#include <inline/alib.h>

/* ------------------------------------------------------------------ state */

/*
 * Raw framing is off unless the caller asks for it. SANA-II has no capability
 * query: a device that does not implement SANA2IOF_RAW is free to ignore the
 * flag and hand back cooked frames, which looks like silent corruption rather
 * than an error. See ami_sana2_probe_raw() below.
 */
static BOOL ami_raw_allowed = (AMI_SANA2_RAW_DEFAULT != 0) ? TRUE : FALSE;

/*
 * The reader threads block in exec Wait() for IORequest completion, which
 * ThreadX knows nothing about. Under the baton scheduling model
 * (docs/RESEARCH.md §6.2) a ThreadX thread must release the baton before
 * blocking that way. The ThreadX port registers these. Both default to no-ops,
 * which suits a model that lets Exec schedule.
 */
static AmiSana2BlockHook ami_block_enter;
static AmiSana2BlockHook ami_block_leave;

VOID ami_sana2_set_block_hooks(AmiSana2BlockHook before_wait,
                               AmiSana2BlockHook after_wait)
{
    ami_block_enter = before_wait;
    ami_block_leave = after_wait;
}

/*
 * Both hooks are called unconditionally. Pairing them is the hook's job.
 *
 * Do not guard them on tx_thread_identify() != TX_NULL. The before-wait hook
 * releases the baton, which clears _tx_thread_current_ptr, which makes
 * tx_thread_identify() return TX_NULL, so the matching after-wait hook is
 * skipped and the thread runs on without the baton. Seen under FS-UAE,
 * 2026-07-25: the NX_IP thread came back from S2_ONLINE without the baton and
 * every ThreadX service it called afterwards returned garbage.
 *
 * src/netstack/netstack_baton.c keeps a per-Exec-Task record of whether the
 * enter hook did anything, which is the only place that can know.
 */
VOID ami_sana2_block_enter(VOID)
{
    if (ami_block_enter != NULL)
        ami_block_enter();
}

VOID ami_sana2_block_leave(VOID)
{
    if (ami_block_leave != NULL)
        ami_block_leave();
}

VOID ami_sana2_set_raw_allowed(BOOL allowed)
{
    ami_raw_allowed = allowed;
}

/* -------------------------------------------------------------- utilities */

static VOID ami_str_copy(char *dst, const char *src, ULONG size)
{
    ULONG i = 0;

    if (size == 0)
        return;

    while (src != NULL && src[i] != '\0' && i < (size - 1))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* NewList() lives in amiga.lib, which a shared library cannot reach. */
VOID ami_sana2_port_init(struct MsgPort *port, struct Task *task, BYTE sigbit,
                         UBYTE flags)
{
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Node.ln_Name = NULL;
    port->mp_Node.ln_Pri  = 0;
    port->mp_Flags        = flags;
    port->mp_SigBit       = (UBYTE)sigbit;
    port->mp_SigTask      = task;

    port->mp_MsgList.lh_Head     = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail     = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;
}

/*
 * Run one synchronous device command. A fresh reply port is created for the
 * calling task each time: control commands come from whichever task is driving
 * (startup task at open, IP thread from the driver entry) and DoIO() waits on
 * mn_ReplyPort->mp_SigTask, so a cached port signals the wrong task.
 * Control traffic is rare enough for the cost not to matter.
 */
LONG ami_sana2_command(AmiSana2If *iface, struct IOSana2Req *req, UWORD command)
{
    struct MsgPort *port;
    LONG            err;

    if (iface == NULL || !iface->device_open || req == NULL)
        return (LONG)IOERR_OPENFAIL;

    port = CreateMsgPort();
    if (port == NULL)
        return (LONG)S2ERR_NO_RESOURCES;

    req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->ios2_Req.io_Message.mn_ReplyPort    = port;
    req->ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(struct IOSana2Req);
    req->ios2_Req.io_Command                 = command;
    req->ios2_Req.io_Flags                   = 0;
    req->ios2_Req.io_Error                   = 0;
    req->ios2_WireError                      = 0;

    /* DoIO() blocks in exec Wait(), so the same baton rule as the readers
       applies. NX_LINK_ENABLE reaches here on the IP thread. */
    ami_sana2_block_enter();
    DoIO((struct IORequest *)req);
    ami_sana2_block_leave();

    err = (LONG)(BYTE)req->ios2_Req.io_Error;

    req->ios2_Req.io_Message.mn_ReplyPort = NULL;
    DeleteMsgPort(port);

    return err;
}

/* ------------------------------------------------------------ device query */

static LONG ami_sana2_query(AmiSana2If *iface)
{
    struct IOSana2Req       req;
    struct Sana2DeviceQuery query;
    LONG                    err;

    req   = iface->templ;
    query = (struct Sana2DeviceQuery){ 0 };

    query.SizeAvailable = (ULONG)sizeof(query);
    req.ios2_StatData   = &query;

    err = ami_sana2_command(iface, &req, S2_DEVICEQUERY);
    if (err != 0)
        return err;

    if (query.SizeSupplied < (ULONG)sizeof(query))
    {
        /* Level-0 devices must supply the whole common block. Anything missing
           falls back to the Ethernet defaults below. */
        AMI_WARN("sana2: short S2_DEVICEQUERY (%ld bytes)",
                 (long)query.SizeSupplied);
    }

    iface->hw_type   = query.HardwareType;
    iface->addr_bits = query.AddrFieldSize;
    iface->mtu       = query.MTU;
    iface->bps       = query.BPS;

    if (iface->addr_bits == 0 && iface->hw_type == S2WireType_Ethernet)
        iface->addr_bits = AMI_ETH_ADDR_SIZE * 8;

    iface->addr_bytes = (UWORD)(iface->addr_bits / 8);
    if (iface->addr_bytes > AMI_ETH_ADDR_SIZE)
        iface->addr_bytes = AMI_ETH_ADDR_SIZE;

    if (iface->mtu == 0)
        iface->mtu = 1500;

    /*
     * SANA-II's MTU is the maximum packet data size, the IP payload, with no
     * link header in it, so it maps straight onto NetX Duo's
     * nx_interface_ip_mtu_size. Clamp to what one packet buffer holds once the
     * alignment pad and the synthesised Ethernet header are accounted for.
     */
    {
        ULONG limit = AMI_POOL_PAYLOAD - AMI_SANA2_RX_PAD - AMI_ETH_HEADER_SIZE;

        if (iface->mtu > limit)
        {
            AMI_WARN("sana2: MTU %ld clamped to %ld", (long)iface->mtu,
                     (long)limit);
            iface->mtu = limit;
        }
    }

    return 0;
}

/* --------------------------------------------------------------- addressing */

/*
 * S2_GETSTATIONADDRESS returns the current address in ios2_SrcAddr and the
 * factory address, if any, in ios2_DstAddr. S2_CONFIGINTERFACE then commits an
 * address and can only be run once per unit. A device already configured by
 * another opener answers S2WERR_IS_CONFIGURED, treated here as success.
 */
static LONG ami_sana2_configure(AmiSana2If *iface, const AmiIfConfig *cfg)
{
    struct IOSana2Req req;
    LONG              err;
    UWORD             i;
    BOOL              have_factory = FALSE;

    req = iface->templ;
    err = ami_sana2_command(iface, &req, S2_GETSTATIONADDRESS);
    if (err != 0)
    {
        AMI_ERROR("sana2: S2_GETSTATIONADDRESS failed (%ld)", (long)err);
        return err;
    }

    for (i = 0; i < iface->addr_bytes; i++)
    {
        if (req.ios2_DstAddr[i] != 0)
            have_factory = TRUE;
    }

    /* Prefer the factory address, and fall back to whatever is configured. */
    if (!have_factory)
    {
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            req.ios2_DstAddr[i] = req.ios2_SrcAddr[i];
    }
    else
    {
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            req.ios2_SrcAddr[i] = req.ios2_DstAddr[i];
    }

    /*
     * HARDWAREADDRESS overrides both. S2_CONFIGINTERFACE commits whatever is
     * in ios2_SrcAddr, so the override goes in after the factory address has
     * been read and before it is committed -- and only for as many bytes as
     * this device's addresses have, so a 6-byte value cannot overwrite the
     * end of a wider one.
     *
     * Two emulated guests on one bridge carry the same factory address and
     * answer each other's ARP. So do two of some real cards. Roadshow has had
     * this since 1.0, and this stack ignored the keyword.
     */
    if (cfg != NULL && cfg->have_hw_address && iface->addr_bytes > 0)
    {
        UWORD n = (iface->addr_bytes < (UWORD)AMI_CFG_MAC_SIZE)
                      ? iface->addr_bytes : (UWORD)AMI_CFG_MAC_SIZE;

        for (i = 0; i < n; i++)
            req.ios2_SrcAddr[i] = cfg->hw_address[i];

        AMI_INFO("sana2: %s unit %lu: address set from HARDWAREADDRESS",
                 iface->device, (unsigned long)iface->unit);
    }

    err = ami_sana2_command(iface, &req, S2_CONFIGINTERFACE);
    if (err != 0)
    {
        if (req.ios2_WireError == S2WERR_IS_CONFIGURED)
        {
            /* Already up under another opener, re-read the live address. */
            struct IOSana2Req again = iface->templ;

            err = ami_sana2_command(iface, &again, S2_GETSTATIONADDRESS);
            if (err != 0)
                return err;

            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                req.ios2_SrcAddr[i] = again.ios2_SrcAddr[i];
        }
        else
        {
            AMI_ERROR("sana2: S2_CONFIGINTERFACE failed (%ld/%ld)", (long)err,
                      (long)req.ios2_WireError);
            return err;
        }
    }

    for (i = 0; i < AMI_ETH_ADDR_SIZE; i++)
        iface->mac[i] = (i < iface->addr_bytes) ? req.ios2_SrcAddr[i] : 0;

    return 0;
}

/* --------------------------------------------------------------- raw probe */

#if AMI_SANA2_PROBE_RAW
/*
 * There is no capability query for raw framing, so this posts one raw CMD_READ
 * and immediately takes it back. A device that does not implement the flag
 * refuses the request in BeginIO (IOERR_NOCMD, S2ERR_BAD_ARGUMENT or
 * S2ERR_NOT_SUPPORTED). One that does implement it queues the request, and
 * AbortIO returns it with IOERR_ABORTED.
 *
 * This cannot detect a device that accepts the flag and then ignores it,
 * handing back cooked frames that are then parsed as though they carried a
 * link header. The result therefore only enables raw when
 * ami_sana2_set_raw_allowed(TRUE) has been called.
 */
static BOOL ami_sana2_probe_raw(AmiSana2If *iface)
{
    struct MsgPort   *port;
    struct IOSana2Req req;
    /* A slot with no packet: the copy hook rejects anything aimed at it. On
       the stack rather than at file scope, so it is per-probe and lives only
       until the WaitIO() below. A shared one is a shared write target for any
       driver that writes ios2_Data itself instead of calling the copy hooks. */
    AmiRxSlot         slot;
    LONG              err;
    ULONG             i;

    if (iface->hw_type != S2WireType_Ethernet)
        return FALSE;

    for (i = 0; i < sizeof(slot); i++)
        ((UBYTE *)&slot)[i] = 0;

    port = CreateMsgPort();
    if (port == NULL)
        return FALSE;

    req = iface->templ;
    req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req.ios2_Req.io_Message.mn_ReplyPort    = port;
    req.ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(struct IOSana2Req);
    req.ios2_Req.io_Command                 = CMD_READ;
    req.ios2_Req.io_Flags                   = SANA2IOF_RAW;
    req.ios2_Req.io_Error                   = 0;
    req.ios2_WireError                      = 0;
    req.ios2_PacketType                     = AMI_ETHERTYPE_ARP;
    req.ios2_DataLength                     = 0;
    req.ios2_Data                           = &slot;

    /* BeginIO(), not SendIO(): SendIO() zeroes io_Flags, so the probe asks
       every device for a cooked read and takes that as raw support. */
    BeginIO((struct IORequest *)&req);
    AbortIO((struct IORequest *)&req);
    WaitIO((struct IORequest *)&req);

    err = (LONG)(BYTE)req.ios2_Req.io_Error;
    DeleteMsgPort(port);

    /* IOERR_ABORTED means the device queued a raw read. 0 means one was
       satisfied. Everything else, including S2ERR_OUTOFSERVICE from a unit
       that is not up yet, counts as no support. */
    return (err == 0 || err == (LONG)IOERR_ABORTED) ? TRUE : FALSE;
}
#endif /* AMI_SANA2_PROBE_RAW */

/* ------------------------------------------------------------ online state */

LONG ami_sana2_online(AmiSana2If *iface)
{
    struct IOSana2Req req = iface->templ;
    LONG              err;

    err = ami_sana2_command(iface, &req, S2_ONLINE);
    if (err != 0 && req.ios2_WireError != S2WERR_UNIT_ONLINE)
    {
        AMI_ERROR("sana2: S2_ONLINE failed (%ld/%ld)", (long)err,
                  (long)req.ios2_WireError);
        return err;
    }

    iface->online = TRUE;

    /* S2_ONLINE resets the device's counters, so ours restart with them. */
    iface->stats.bad_data        = 0;
    iface->stats.overruns        = 0;
    iface->stats.unknown_types   = 0;
    iface->stats.reconfigurations = 0;

    return 0;
}

LONG ami_sana2_offline(AmiSana2If *iface)
{
    struct IOSana2Req req = iface->templ;
    LONG              err;

    if (!iface->online)
        return 0;

    iface->online = FALSE;

    err = ami_sana2_command(iface, &req, S2_OFFLINE);
    if (err != 0 && req.ios2_WireError != S2WERR_UNIT_OFFLINE)
    {
        AMI_WARN("sana2: S2_OFFLINE failed (%ld/%ld)", (long)err,
                 (long)req.ios2_WireError);
        return err;
    }

    return 0;
}

/* ---------------------------------------------------------------- multicast */

/*
 * The bucket an address lands in on a DP8390-class receive filter: the top six
 * bits of the CRC-32 of the six address bytes. Every NE2000 derivative hashes
 * this way, so neither the driver nor this code has any freedom in it.
 */
static UBYTE ami_sana2_mcast_bucket(const UBYTE *addr)
{
    ULONG crc = 0xFFFFFFFFUL;
    WORD  i;
    WORD  j;

    for (i = 0; i < 6; i++)
    {
        UBYTE b = addr[i];

        for (j = 0; j < 8; j++)
        {
            ULONG carry = ((crc & 0x80000000UL) ? 1UL : 0UL) ^ (ULONG)(b & 1);

            crc <<= 1;
            b >>= 1;
            if (carry != 0)
            {
                crc = (crc ^ 0x04C11DB6UL) | carry;
            }
        }
    }

    return (UBYTE)(crc >> 26);
}

/*
 * One address per bucket, all of the form 80:00:00:00:00:xx. Bit 7 of the
 * first octet is what the X-Surf drivers demand, and the group bit is clear so
 * none of these can ever deliver a unicast frame: the hash filter is only
 * consulted for frames that already have the group bit set.
 */
static const UBYTE ami_sana2_mcast_alias[64] =
{
    0x89, 0x09, 0xC9, 0x49, 0x82, 0x02, 0xC2, 0x42,
    0x00, 0x80, 0x40, 0xC0, 0x0B, 0x8B, 0x4B, 0xCB,
    0x41, 0xC1, 0x01, 0x81, 0x4A, 0xCA, 0x0A, 0x8A,
    0xC8, 0x48, 0x88, 0x08, 0xC3, 0x43, 0x83, 0x03,
    0xC6, 0x46, 0x86, 0x06, 0xCD, 0x4D, 0x8D, 0x0D,
    0x4F, 0xCF, 0x0F, 0x8F, 0x44, 0xC4, 0x04, 0x84,
    0x0E, 0x8E, 0x4E, 0xCE, 0x05, 0x85, 0x45, 0xC5,
    0x87, 0x07, 0xC7, 0x47, 0x8C, 0x0C, 0xCC, 0x4C
};

LONG ami_sana2_multicast(AmiSana2If *iface, UWORD command,
                         ULONG addr_msw, ULONG addr_lsw)
{
    struct IOSana2Req req = iface->templ;
    LONG              err;

    /* The spec puts the multicast address in ios2_SrcAddr for both commands. */
    req.ios2_SrcAddr[0] = (UBYTE)(addr_msw >> 8);
    req.ios2_SrcAddr[1] = (UBYTE)(addr_msw);
    req.ios2_SrcAddr[2] = (UBYTE)(addr_lsw >> 24);
    req.ios2_SrcAddr[3] = (UBYTE)(addr_lsw >> 16);
    req.ios2_SrcAddr[4] = (UBYTE)(addr_lsw >> 8);
    req.ios2_SrcAddr[5] = (UBYTE)(addr_lsw);

    err = ami_sana2_command(iface, &req, command);

    /*
     * x-surf.device and x-surf-100.device 1.16 test bit 7 of ios2_SrcAddr[0],
     * where the Ethernet group bit is bit 0. They refuse every address a real
     * group has, and the hash filter stays empty. IPv6 then fails silently:
     * the router solicits the guest at its solicited-node address, the card
     * drops it, and every off-link answer is discarded one hop away while ping
     * and SLAAC keep working. The card is asked for a bucket rather than an
     * address, so the same bucket is asked for with an address the driver
     * takes. The IP layer drops whatever else that bucket lets in, as it
     * always had to with a hash filter.
     */
    if (err == (LONG)S2ERR_BAD_ADDRESS
        && req.ios2_WireError == (ULONG)S2WERR_BAD_MULTICAST
        && (req.ios2_SrcAddr[0] & 0x80) == 0)
    {
        struct IOSana2Req alias  = iface->templ;
        UBYTE             bucket = ami_sana2_mcast_bucket(req.ios2_SrcAddr);

        alias.ios2_SrcAddr[0] = 0x80;
        alias.ios2_SrcAddr[1] = 0x00;
        alias.ios2_SrcAddr[2] = 0x00;
        alias.ios2_SrcAddr[3] = 0x00;
        alias.ios2_SrcAddr[4] = 0x00;
        alias.ios2_SrcAddr[5] = ami_sana2_mcast_alias[bucket];

        err = ami_sana2_command(iface, &alias, command);
        if (err == 0)
        {
            AMI_DEBUG("sana2: %s took bucket %ld as 80:00:00:00:00:%02lx",
                      iface->device, (long)bucket,
                      (unsigned long)alias.ios2_SrcAddr[5]);
        }
        else
        {
            req = alias;
        }
    }

    if (err != 0)
    {
        AMI_WARN("sana2: multicast cmd %ld failed (%ld/%ld)", (long)command,
                 (long)err, (long)req.ios2_WireError);
    }

    return err;
}

/* --------------------------------------------------------------- statistics */

/*
 * Refresh the device-derived half of AmiSana2Stats. Called only from the IP
 * thread (via the driver entry) or from open/close, because it borrows the
 * calling task for its reply port.
 */
VOID ami_sana2_refresh_stats(AmiSana2If *iface)
{
    struct IOSana2Req       req;
    struct Sana2DeviceStats stats;

    if (iface == NULL || !iface->device_open)
        return;

    req   = iface->templ;
    stats = (struct Sana2DeviceStats){ 0 };

    req.ios2_StatData = &stats;

    if (ami_sana2_command(iface, &req, S2_GETGLOBALSTATS) != 0)
        return;

    iface->stats.bad_data         = stats.BadData;
    iface->stats.overruns         = stats.Overruns;
    iface->stats.unknown_types    = stats.UnknownTypesReceived;
    iface->stats.reconfigurations = stats.Reconfigurations;

#ifdef AMINETXDUO_RXPROBE
    iface->probe_dev_rx = stats.PacketsReceived;
    iface->probe_dev_tx = stats.PacketsSent;
#endif
}

VOID ami_sana2_get_stats(const AmiSana2If *iface, AmiSana2Stats *out)
{
    if (out == NULL)
        return;

    if (iface == NULL)
    {
        *out = (AmiSana2Stats){ 0 };
        return;
    }

    /* Software counters only: the device-derived fields are refreshed on the
       IP thread (NX_LINK_GET_*_COUNT, NX_LINK_DEFERRED_PROCESSING), because
       S2_GETGLOBALSTATS here needs a reply port on whichever task called
       GetNetworkStatistics(). */
    *out = iface->stats;

#ifdef AMINETXDUO_RXPROBE
    /* NetStat is what reaches this, and it brackets the transfer, so the pair
       of reports subtracts. The shutdown path is never reached while a DOS
       handler holds the library open. */
    ami_sana2_rxprobe_report(iface);
#endif
}

/*
 * The non-counter facts. Every field is a plain read of shim state. The
 * `posted` and `busy` flags are written by the readers and the TX reaper, so a
 * count taken here is a sample, not a snapshot.
 */
VOID ami_sana2_get_info(const AmiSana2If *iface, AmiSana2Info *out)
{
    UWORD r, i;

    if (out == NULL)
        return;

    *out = (AmiSana2Info){ 0 };

    if (iface == NULL)
        return;

    out->hardware_type = iface->hw_type;
    out->address_bits  = (ULONG)iface->addr_bits;

    for (r = 0; r < (UWORD)AMI_SANA2_RX_READERS; r++)
    {
        const AmiSana2Rx *rx = &iface->rx[r];

        out->read_requests += (ULONG)rx->depth;

        for (i = 0; i < rx->depth; i++)
        {
            if (rx->slot[i].posted)
                out->read_pending++;
        }
    }

    out->write_requests = (ULONG)AMI_SANA2_TX_SLOTS;

    for (i = 0; i < (UWORD)AMI_SANA2_TX_SLOTS; i++)
    {
        if (iface->tx[i].busy)
            out->write_pending++;
    }
}

/* ------------------------------------------------------------- open / close */

AmiSana2If *ami_sana2_open(const AmiIfConfig *cfg, LONG *err)
{
    AmiSana2If *iface;
    LONG        status;
    UWORD       tag = 0;

    if (err != NULL)
        *err = AMI_NET_OK;

    if (cfg == NULL || cfg->device[0] == '\0')
    {
        if (err != NULL)
            *err = AMI_NET_ERR_CONFIG;
        return NULL;
    }

    iface = (AmiSana2If *)ami_alloc((ULONG)sizeof(AmiSana2If));
    if (iface == NULL)
    {
        if (err != NULL)
            *err = AMI_NET_ERR_NOMEM;
        return NULL;
    }

    ami_str_copy(iface->device, cfg->device, (ULONG)sizeof(iface->device));
    ami_str_copy(iface->card, cfg->card, (ULONG)sizeof(iface->card));
    iface->unit = cfg->unit;

    /*
     * The buffer-management tag list is an input to OpenDevice and must
     * outlive the unit, so it lives in the interface, not on the stack.
     *
     * S2_DMACopyToBuff32 is not offered.  sana2.h defines
     * S2_CopyToBuff32/FromBuff32 (S2_Dummy + 6, + 7) and
     * S2_DMACopyToBuff32/FromBuff32 (+ 8, + 9), and the DMA pair is the only
     * thing that can remove the receive copy, which is 18.6% of an A1200 wire
     * transfer with n68k_copy_bytes at 179.5 ns/B.  Every SANA-II driver on
     * hand was scanned for the tag constants it looks up, 2026-08-07:
     *
     *     a2060, eb920, eb920-i6, ppp-serial, rs485      base pair
     *     a2065, ariadne, ariadne_ii, hydra, slip,       base pair + filter
     *       x-surf, x-surf-100
     *     cnet, cnet16                                   + DMA pair
     *     ppp-ethernet                                   + DMA pair, FromBuff16
     *
     * Three of fifteen, none of them a card in general use, and not the a2065
     * every measurement in this tree is taken on.  Nothing asks for the
     * non-DMA 32 variants at all.  Offering the DMA pair means a packet pool
     * in DMA-reachable memory with the alignment and coherency that implies,
     * for cnet alone.  The copy stays.
     *
     * A driver can compute the tag rather than carry the constant, so this is
     * evidence rather than proof.  None of the fifteen appears to.
     */
    iface->buffer_tags[tag].ti_Tag  = S2_CopyToBuff;
    iface->buffer_tags[tag].ti_Data = (ULONG)ami_sana2_copy_to_buff;
    tag++;
    iface->buffer_tags[tag].ti_Tag  = S2_CopyFromBuff;
    iface->buffer_tags[tag].ti_Data = (ULONG)ami_sana2_copy_from_buff;
    tag++;
#if AMI_SANA2_OFFER_COPY16
    iface->buffer_tags[tag].ti_Tag  = S2_CopyToBuff16;
    iface->buffer_tags[tag].ti_Data = (ULONG)ami_sana2_copy_to_buff;
    tag++;
    iface->buffer_tags[tag].ti_Tag  = S2_CopyFromBuff16;
    iface->buffer_tags[tag].ti_Data = (ULONG)ami_sana2_copy_from_buff;
    tag++;
#endif
    /*
     * CARD=, when the file said one.  A driver that covers a family of boards
     * cannot tell which one is wanted from UNIT alone, and anxnet.device fails
     * the open when the name does not match what is in the machine rather than
     * binding to the next board along.  Every other driver ignores an unknown
     * tag, so this costs nothing on the ones that open a single card.
     */
    if (iface->card[0] != '\0')
    {
        iface->buffer_tags[tag].ti_Tag  = S2_AnxCardType;
        iface->buffer_tags[tag].ti_Data = (ULONG)iface->card;
        tag++;
    }
    iface->buffer_tags[tag].ti_Tag  = TAG_DONE;
    iface->buffer_tags[tag].ti_Data = 0;

    iface->templ.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    iface->templ.ios2_Req.io_Message.mn_Length = (UWORD)sizeof(struct IOSana2Req);
    iface->templ.ios2_BufferManagement         = iface->buffer_tags;

    {
        struct MsgPort *port = CreateMsgPort();

        if (port == NULL)
        {
            ami_free(iface);
            if (err != NULL)
                *err = AMI_NET_ERR_NOMEM;
            return NULL;
        }

        iface->templ.ios2_Req.io_Message.mn_ReplyPort = port;

        status = ami_sana2_open_device(iface->device, iface->unit,
                                       (struct IORequest *)&iface->templ);

        /* The template is never sent again, everything is cloned from it. */
        iface->templ.ios2_Req.io_Message.mn_ReplyPort = NULL;
        DeleteMsgPort(port);
    }

    if (status != 0)
    {
        /* The card is in the message when one was pinned: a driver that
           covers a family refuses the open when the name does not match what
           is in the machine, and "cannot open anxnet.device unit 0" alone
           reads as a missing driver rather than as the wrong board. */
        if (iface->card[0] != '\0')
            AMI_ERROR("sana2: cannot open %s unit %ld CARD=%s (%ld): no such "
                      "card in this machine", iface->device,
                      (long)iface->unit, iface->card, (long)status);
        else
            AMI_ERROR("sana2: cannot open %s unit %ld (%ld)", iface->device,
                      (long)iface->unit, (long)status);
        ami_free(iface);
        if (err != NULL)
            *err = AMI_NET_ERR_NODEV;
        return NULL;
    }

    iface->device_open = TRUE;

    /* The open worked, so a failure past here is a driver that answered badly
       and not a missing card. AMI_NET_ERR_DEVBAD keeps the two apart all the
       way out to what the user is told. */
    if (ami_sana2_query(iface) != 0)
    {
        AMI_ERROR("sana2: %s unit %ld opened but S2_DEVICEQUERY failed",
                  iface->device, (long)iface->unit);
        ami_sana2_close(iface);
        if (err != NULL)
            *err = AMI_NET_ERR_DEVBAD;
        return NULL;
    }

    /*
     * MTU= from the interface configuration, applied where the tag list path
     * applies IFC_LimitMTU: after the driver has been asked, and downwards
     * only.  What S2_DEVICEQUERY reports is what the hardware can carry, so a
     * larger number in the file is a mistake rather than a request.
     */
    if (cfg->mtu != 0)
    {
        if (cfg->mtu < iface->mtu)
        {
            iface->mtu = cfg->mtu;
        }
        else if (cfg->mtu > iface->mtu)
        {
            AMI_WARN("sana2: MTU %ld exceeds what %s reported (%ld), ignored",
                     (long)cfg->mtu, iface->device, (long)iface->mtu);
        }
    }

    if (ami_sana2_configure(iface, cfg) != 0)
    {
        ami_sana2_close(iface);
        if (err != NULL)
            *err = AMI_NET_ERR_DEVBAD;
        return NULL;
    }

    /*
     * REQUIRESINITDELAY. Some cards lose data if the first packet follows the
     * configure too closely, and Roadshow's manual names the original Ariadne.
     * The delay goes after the configure and before anything is sent or the
     * readers are started, which is the window the option is about.
     *
     * A second, as Roadshow specifies, and only when asked for: it is paid at
     * every bring-up, and the cards available for testing do not need it.
     */
    if (cfg != NULL && cfg->requires_init_delay)
    {
        AMI_INFO("sana2: %s unit %lu: REQUIRESINITDELAY, settling for a second",
                 iface->device, (unsigned long)iface->unit);

        /* tx_thread_sleep() and not dos.library's Delay(): this runs inside
           the ThreadX bracket, the same way the readers in sana2_rx.c wait,
           and it needs no library base of its own. */
        tx_thread_sleep((ULONG)NX_IP_PERIODIC_RATE);
    }

#if AMI_SANA2_PROBE_RAW
    iface->raw_supported = ami_sana2_probe_raw(iface);
#endif
    iface->raw_mode = (iface->raw_supported && ami_raw_allowed) ? TRUE : FALSE;

    if (iface->raw_mode && iface->addr_bytes != AMI_ETH_ADDR_SIZE)
    {
        /* Raw framing only means anything when the link header shape is
           known. */
        iface->raw_mode = FALSE;
    }

    ami_sana2_tx_init(iface);

    AMI_INFO("sana2: %s unit %ld hw %ld mtu %ld bps %ld addr %ld bits %s",
             iface->device, (long)iface->unit, (long)iface->hw_type,
             (long)iface->mtu, (long)iface->bps, (long)iface->addr_bits,
             iface->raw_mode ? "raw" : "cooked");

    return iface;
}

VOID ami_sana2_close(AmiSana2If *iface)
{
    if (iface == NULL)
        return;

    /* ami_sana2_rx_stop() takes the wire offline itself, and does so first:
       S2_OFFLINE is what returns the readers' queued CMD_READs on a device
       that ignores AbortIO(). Offlining after the stop instead costs ten
       seconds of reader timeouts. */
    ami_sana2_rx_stop(iface);
    ami_sana2_tx_drain(iface);

    if (iface->device_open)
    {
        ami_sana2_offline(iface);

        /*
         * A device that still owns one of these requests must not be closed
         * and this memory must not be freed: the request points into this
         * allocation and into a reply port inside it, so the next completion
         * is written over whatever took their place. That is true of a queued
         * CMD_WRITE as much as a CMD_READ: tx_port and the tx ring are fields
         * of AmiSana2If.
         */
        if (iface->rx_orphaned || iface->tx_orphaned)
        {
            AMI_ERROR("sana2: leaking the interface, the device still holds "
                      "requests inside it");
            return;
        }

        iface->templ.ios2_Req.io_Message.mn_ReplyPort = NULL;
        CloseDevice((struct IORequest *)&iface->templ);
        iface->device_open = FALSE;
    }

    if (iface->rx_orphaned || iface->tx_orphaned)
    {
        AMI_ERROR("sana2: leaking the interface, requests unreclaimed");
        return;
    }

    if (iface->interface_ptr != NULL)
        iface->interface_ptr->nx_interface_additional_link_info = NULL;

    /* Drop the (NX_IP, index) -> iface binding before the memory goes away. */
    ami_sana2_unbind(iface);

    ami_free(iface);
}

/* ------------------------------------------------------------- accessors */

VOID ami_sana2_get_mac(const AmiSana2If *iface, UCHAR mac[AMI_ETH_ADDR_SIZE])
{
    UWORD i;

    if (mac == NULL)
        return;

    for (i = 0; i < AMI_ETH_ADDR_SIZE; i++)
        mac[i] = (iface != NULL) ? iface->mac[i] : 0;
}

ULONG ami_sana2_get_mtu(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->mtu : 0;
}

ULONG ami_sana2_get_bps(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->bps : 0;
}

BOOL ami_sana2_is_online(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->online : FALSE;
}

BOOL ami_sana2_admin_up(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->admin_up : FALSE;
}

BOOL ami_sana2_orphaned(const AmiSana2If *iface)
{
    if (iface == NULL)
        return FALSE;

    return (iface->rx_orphaned || iface->tx_orphaned) ? TRUE : FALSE;
}

BOOL ami_sana2_raw_mode(const AmiSana2If *iface)
{
    return (iface != NULL) ? iface->raw_mode : FALSE;
}
