/*
 * AmiNetXDuo, SANA-II <-> NetX Duo driver shim.  SANA-II is cooked: the device
 * owns the link header, so the shim builds none on TX and synthesises one on RX.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_H
#define AMINETXDUO_SANA2_H

/*
 * tx_api.h has to come first: exec/types.h turns VOID into a macro, which
 * breaks tx_port.h's `typedef void VOID`.  The undef/restore pair keeps this
 * header safe to include after an exec header too.
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

/* The NX_IP driver entry for nx_ip_create().  The shim finds its AmiSana2If
   through nx_interface_additional_link_info, which ami_sana2_attach() sets. */
VOID ami_sana2_driver_entry(NX_IP_DRIVER *driver_req);

/* NX_LINK_DISABLE without the S2_OFFLINE, so other stacks on the same device
   keep the wire.  Values past NX_LINK_USER_COMMAND are application-reserved. */
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
 * Whether the stack has been told to use this interface, not whether the wire
 * is there.  NX_LINK_ENABLE sets it and the three disables clear it; a pulled
 * cable clears nx_interface_link_up instead and never touches this.
 */
BOOL        ami_sana2_admin_up(const AmiSana2If *iface);

/*
 * Put one frame on the wire that the IP stack did not build.  `dst` is six
 * bytes, or NULL for a wire with no address field.  The payload is copied, so
 * the caller's buffer may go away.  0, or -1 on MTU/pool/link failure.
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
    /* rx_errors is the sum of these four. */
    ULONG   rx_err_runt;        /* shorter than the link header             */
    ULONG   rx_err_verify;      /* IP/TCP/UDP checksum rejected it          */
    ULONG   rx_err_length;      /* driver reported 0 or over slot capacity  */
    ULONG   rx_err_io;          /* CMD_READ completed with io_Error         */
    /* The field names are ABI-stable and predate direct receive. */
    ULONG   rx_copy_hook;       /* frames filled by either receive hook     */
    ULONG   rx_copy_summed;     /* of those, summed while being filled      */
    /* Of rx_copy_hook, those that came through the private direct-receive
       pair; rx_copy_summed cannot answer this, both paths bump it. */
    ULONG   rx_direct_fill;
    /* anxnet.device recovery counters read through S2_GETSPECIALSTATS.  Other
       drivers leave them zero. */
    ULONG   tick_polls;
    ULONG   rx_kicks;
} AmiSana2Stats;

VOID ami_sana2_get_stats(const AmiSana2If *iface, AmiSana2Stats *out);

/*
 * What QueryInterfaceTagList() asks for by name, read out of the shim's own
 * state: nothing here touches the device, so it is safe from any task.
 * `address_bits` is in BITS, and comes from S2_DEVICEQUERY.
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
 * TRUE when the device kept one or more of this interface's CMD_READs at
 * teardown.  The requests point into the AmiSana2If and its reply port, so
 * nothing here may be freed while it holds; ami_sana2_close() refuses to.
 */
BOOL ami_sana2_orphaned(const AmiSana2If *iface);

/* Raw framing is only used once the caller has opted in with
   ami_sana2_set_raw_allowed(); the default is cooked. */
BOOL ami_sana2_raw_mode(const AmiSana2If *iface);
VOID ami_sana2_set_raw_allowed(BOOL allowed);

/*
 * A ThreadX thread must release the baton before blocking in exec Wait(); the
 * ThreadX port registers the pair here.  Both default to no-ops, which is
 * correct when Exec does the scheduling.
 */
typedef VOID (*AmiSana2BlockHook)(VOID);
VOID ami_sana2_set_block_hooks(AmiSana2BlockHook before_wait,
                               AmiSana2BlockHook after_wait);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_SANA2_H */
