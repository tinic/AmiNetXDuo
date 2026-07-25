/*
 * AmiNetXDuo -- SANA-II <-> NetX Duo driver shim.
 *
 * NetX Duo expects an Ethernet-shaped driver: on TX it hands the driver a
 * packet with a 14-byte Ethernet header already prepended, and on RX the driver
 * must present the same shape before calling _nx_ip_packet_receive().
 *
 * SANA-II is "cooked": the device owns the link header. On CMD_WRITE we supply
 * ios2_PacketType + ios2_DstAddr + payload; on CMD_READ we get back
 * ios2_PacketType + ios2_SrcAddr + ios2_DstAddr + payload. The shim therefore
 * strips the Ethernet header on TX and synthesises one on RX.
 *
 * See docs/RESEARCH.md §3.4 and §6.5.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_H
#define AMINETXDUO_SANA2_H

#include <exec/types.h>
#include "aminetxduo/config.h"

#include "tx_api.h"
#include "nx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmiSana2If AmiSana2If;

/* Ethernet framing constants shared with the shim's users. */
#define AMI_ETH_HEADER_SIZE     14
#define AMI_ETH_ADDR_SIZE       6
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
 * Open the SANA-II device named in cfg and prepare it for use. Does not bring
 * the link online -- NX_LINK_ENABLE does that. Returns NULL on failure and sets
 * *err to an AMI_NET_ERR_* code.
 */
AmiSana2If *ami_sana2_open(const AmiIfConfig *cfg, LONG *err);
VOID        ami_sana2_close(AmiSana2If *iface);

/* Register with an NX_IP as interface `index`. Sets additional_link_info. */
LONG        ami_sana2_attach(AmiSana2If *iface, NX_IP *ip, UINT index);

/* Hardware facts, valid after ami_sana2_open(). */
VOID        ami_sana2_get_mac(const AmiSana2If *iface, UCHAR mac[AMI_ETH_ADDR_SIZE]);
ULONG       ami_sana2_get_mtu(const AmiSana2If *iface);
ULONG       ami_sana2_get_bps(const AmiSana2If *iface);
BOOL        ami_sana2_is_online(const AmiSana2If *iface);

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
} AmiSana2Stats;

VOID ami_sana2_get_stats(const AmiSana2If *iface, AmiSana2Stats *out);

/*
 * Raw-frame fast path. Probed at open time: when the device supports
 * S2_RAWREAD/S2_RAWWRITE the shim moves whole frames and skips header
 * synthesis. Reported here so tools and tests can tell which path is live.
 */
BOOL ami_sana2_raw_mode(const AmiSana2If *iface);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_SANA2_H */
