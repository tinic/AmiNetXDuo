/*
 * AmiNetXDuo, SANA-II <-> NetX Duo driver shim.
 *
 * NetX Duo does not prepend the Ethernet header: it reserves headroom and
 * leaves the link header to the driver. On RX the driver must present an
 * Ethernet-shaped packet before calling _nx_ip_packet_receive().
 *
 * SANA-II is "cooked": the device owns the link header. On CMD_WRITE we supply
 * ios2_PacketType + ios2_DstAddr + payload; on CMD_READ we get back
 * ios2_PacketType + ios2_SrcAddr + ios2_DstAddr + payload. The shim therefore
 * builds no Ethernet header on TX and synthesises one on RX. Only the
 * default-off raw path builds one; see src/sana2/sana2_tx.c.
 *
 * See docs/RESEARCH.md §3.4.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_H
#define AMINETXDUO_SANA2_H

/*
 * tx_api.h has to come first: exec/types.h turns VOID into a macro, which
 * breaks tx_port.h's `typedef void VOID`. The undef/restore pair makes this
 * header safe to include after an exec header too, whichever of the two
 * spellings of VOID survives, it still means `void`.
 */
#undef VOID
#include "tx_api.h"
#include "nx_api.h"
#ifndef VOID
#define VOID void
#endif

#include <exec/types.h>
#include "aminetxduo/config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmiSana2If AmiSana2If;

/* Ethernet framing constants shared with the shim's users. */
#define AMI_ETH_HEADER_SIZE     14
#define AMI_ETH_ADDR_SIZE       6
/* The minimum Ethernet frame, header and payload, without the FCS the
   hardware appends.  46 bytes of payload behind a 14-byte header. */
#define AMI_ETH_MIN_FRAME       60
#define AMI_ETHERTYPE_IPV4      0x0800
#define AMI_ETHERTYPE_ARP       0x0806
#define AMI_ETHERTYPE_IPV6      0x86DD

/*
 * The NX_IP driver entry. Pass this to nx_ip_create(); the shim finds its
 * AmiSana2If through nx_interface_additional_link_info, which
 * ami_sana2_attach() sets when the interface is registered.
 */
VOID ami_sana2_driver_entry(NX_IP_DRIVER *driver_req);

/*
 * NX_LINK_DISABLE without the S2_OFFLINE. Roadshow's SM_Down is "the stack
 * will no longer attempt to transmit messages through this interface. However,
 * the underlying SANA-II device driver may still be connected to the network",
 * which NX_LINK_DISABLE cannot express: it takes the wire down for Envoy and
 * ACS as well. Values past NX_LINK_USER_COMMAND are reserved for the
 * application, which is what this is.
 */
#define AMI_LINK_STACK_DISABLE  (NX_LINK_USER_COMMAND + 1)

/*
 * Open the SANA-II device named in cfg and prepare it for use. Does not bring
 * the link online, NX_LINK_ENABLE does that. Returns NULL on failure and sets
 * *err to an AMI_NET_ERR_* code.
 */
AmiSana2If *ami_sana2_open(const AmiIfConfig *cfg, LONG *err);
/* TRUE when the allocation was released. FALSE means a device still owns a
   request inside it, so the caller must retain every packet pool that request
   can still reach. */
BOOL        ami_sana2_close(AmiSana2If *iface);

/* Register with an NX_IP as interface `index`. Sets additional_link_info. */
LONG        ami_sana2_attach(AmiSana2If *iface, NX_IP *ip, UINT index);

/* Hardware facts, valid after ami_sana2_open(). */
VOID        ami_sana2_get_mac(const AmiSana2If *iface, UCHAR mac[AMI_ETH_ADDR_SIZE]);
ULONG       ami_sana2_get_mtu(const AmiSana2If *iface);
ULONG       ami_sana2_get_bps(const AmiSana2If *iface);
BOOL        ami_sana2_is_online(const AmiSana2If *iface);

/*
 * Whether the stack has been told to use this interface, as opposed to whether
 * the wire is there. NX_LINK_ENABLE sets it, the three disables clear it, and
 * a cable pulled out from under a running interface does not, that clears
 * nx_interface_link_up from the reader (sana2_rx.c) and nothing else. IFQ_State
 * is defined in these terms: "the stack will attempt to transmit messages
 * through this interface. However, the underlying SANA-II device driver may not
 * be connected to the network yet."
 */
BOOL        ami_sana2_admin_up(const AmiSana2If *iface);

/*
 * Put one frame on the wire that the IP stack did not build.
 *
 * This exists for bpf_write(): a capture consumer hands over a complete
 * DLT_EN10MB frame, src/bpf/ takes the fourteen bytes apart (cooked SANA-II
 * builds them itself and would otherwise put two link headers on the wire) and
 * arrives here with the destination, the EtherType and the payload.
 *
 * `dst` is six bytes or NULL for a wire with no address field. The payload is
 * copied into a fresh NX_PACKET, because the caller's buffer is an application
 * buffer that goes away when the call returns and the write completes
 * asynchronously. Returns 0, or -1 if the payload exceeds the MTU, the pool is
 * empty or the interface is down.
 */
LONG ami_sana2_inject(AmiSana2If *iface, UWORD ether_type, const UBYTE *dst,
                      const UBYTE *payload, ULONG len);

/* Counters for GetNetworkStatistics()/netstat. */
typedef struct AmiSana2Stats {
    ULONG   packets_received;
    ULONG   packets_sent;
    ULONG   bad_data;
    ULONG   overruns;
    ULONG   unknown_types;
    ULONG   reconfigurations;
    ULONG   tx_errors;
    ULONG   rx_errors;
    ULONG   alloc_failures;     /* NX_PACKET allocation failed on RX */
    /*
     * rx_errors is the sum of these four, and they are what it is worth
     * knowing. A frame the checksum rejected and a frame the driver refused
     * to hand over are the same number to a caller counting rx_errors, and
     * telling them apart twice needed a purpose-built probe build: an
     * emulator delivering unfilled offload checksums and one delivering
     * oversized coalesced frames both read as "receive errors" and are
     * nothing alike.
     */
    ULONG   rx_err_runt;        /* shorter than the link header             */
    ULONG   rx_err_verify;      /* IP/TCP/UDP checksum rejected it          */
    ULONG   rx_err_length;      /* driver reported 0 or over slot capacity  */
    ULONG   rx_err_io;          /* CMD_READ completed with io_Error         */
    /*
     * Whether the driver fills frames through our standard copy hook or the
     * private direct-receive pair, and whether that fill also produced the
     * receive checksum. A device is free to use neither; one that does not
     * produce a sum gets its frames walked again for the checksum -- invisible
     * without this, and it decides whether the fused path is active on a
     * given card. The field names are ABI-stable and predate direct receive.
     */
    ULONG   rx_copy_hook;       /* frames filled by either receive hook     */
    ULONG   rx_copy_summed;     /* of those, summed while being filled      */
    /*
     * Of rx_copy_hook, how many came through the private direct-receive pair
     * -- the device drained the wire straight into the packet, no staging
     * copy.  rx_copy_summed cannot answer this: the staging hook also fuses
     * its checksum, so both paths bump it.  Zero on a card whose core never
     * claims, and the one number that says the single-copy path is live.
     */
    ULONG   rx_direct_fill;
} AmiSana2Stats;

VOID ami_sana2_get_stats(const AmiSana2If *iface, AmiSana2Stats *out);

/*
 * The non-counter facts QueryInterfaceTagList() is asked for by name:
 * IFQ_HardwareType, IFQ_HardwareAddressSize and the four I/O request tags. All
 * of it is read out of the shim's own state; nothing here touches the device,
 * so it is safe from any task.
 *
 * `address_bits` is in bits, the unit the published API asks for ("for an
 * Ethernet interface the number 48 would be returned"), and it comes from
 * S2_DEVICEQUERY rather than from an assumption about Ethernet.
 */
typedef struct AmiSana2Info {
    ULONG   hardware_type;      /* SANA-II wire type, S2WireType_*          */
    ULONG   address_bits;       /* hardware address size, in bits           */
    ULONG   read_requests;      /* CMD_READs allocated across all readers   */
    ULONG   read_pending;       /* of those, outstanding at the device now  */
    ULONG   write_requests;     /* CMD_WRITE slots allocated                */
    ULONG   write_pending;      /* of those, in flight now                  */
} AmiSana2Info;

VOID ami_sana2_get_info(const AmiSana2If *iface, AmiSana2Info *out);

/*
 * Raw-frame fast path. SANA-II expresses it as SANA2IOF_RAW in io_Flags on
 * CMD_READ/CMD_WRITE rather than as separate commands, and offers no way to
 * ask a device whether it implements the flag. The shim probes at open time
 * (post a raw read, take it straight back) and reports the answer here, but a
 * device that accepts the flag and then ignores it is indistinguishable from
 * one that honours it, and would silently mis-frame every packet. So raw is
 * only *used* when the caller has opted in with ami_sana2_set_raw_allowed();
 * the default is cooked.
 */
/*
 * TRUE when the device kept one or more of this interface's CMD_READs at
 * teardown. The requests point into the AmiSana2If and into a reply port inside
 * it, so nothing here may be freed while it holds; ami_sana2_close() refuses to
 * free such an interface and says so. A caller deciding whether an interface
 * can be removed needs this answer before it detaches anything.
 */
BOOL ami_sana2_orphaned(const AmiSana2If *iface);

BOOL ami_sana2_raw_mode(const AmiSana2If *iface);
VOID ami_sana2_set_raw_allowed(BOOL allowed);

/*
 * The SANA-II readers block in exec Wait() for IORequest completion, which is
 * outside ThreadX's view of the world. Under the baton scheduling model
 * (docs/RESEARCH.md §6.2) a ThreadX thread must release the baton before
 * blocking that way; the ThreadX port registers the pair here. Both default to
 * no-ops, which is correct when Exec does the scheduling.
 */
typedef VOID (*AmiSana2BlockHook)(VOID);
VOID ami_sana2_set_block_hooks(AmiSana2BlockHook before_wait,
                               AmiSana2BlockHook after_wait);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_SANA2_H */
