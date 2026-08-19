/*
 * AmiNetXDuo, DHCP option-12 hostname ownership.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_DHCP_HOSTNAME_H
#define AMINETXDUO_NETSTACK_DHCP_HOSTNAME_H

#include "aminetxduo/config.h"

typedef struct AmiNsDhcpHostnameState
{
    char  lease[AMI_CFG_MAX_INTERFACES][AMI_CFG_NAME_LEN];
    char  owner[AMI_CFG_NAME_LEN];
    char  fallback[AMI_CFG_NAME_LEN];
    UWORD fallback_source;
} AmiNsDhcpHostnameState;

VOID ami_ns_dhcp_hostname_update(AmiNsDhcpHostnameState *state,
                                 UWORD interface_index,
                                 const char *hostname);
BOOL ami_ns_dhcp_hostname_decode(char out[AMI_CFG_NAME_LEN],
                                 const UBYTE *raw, ULONG length);
BOOL ami_ns_dhcp_hostname_reconcile(AmiConfig *config,
                                    AmiNsDhcpHostnameState *state);
VOID ami_ns_dhcp_hostname_displace(AmiNsDhcpHostnameState *state);

#endif /* AMINETXDUO_NETSTACK_DHCP_HOSTNAME_H */
