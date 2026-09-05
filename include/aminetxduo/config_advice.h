/* Configuration advice codes.  The words are in src/config/config_advice.c,
 * which only the C: commands link: bsdsocket.library installs no reporter, so
 * it can never print advice, and it stays resident.  Same rule as the event
 * ring in tools/check-no-diag-strings.sh -- a code here, the words there.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_CONFIG_ADVICE_H
#define AMINETXDUO_CONFIG_ADVICE_H

#define AMI_CFG_ADVICE_NONE                               0

#define AMI_CFG_ADVICE_RUN_NETSETUP_IT_ASKS              1
#define AMI_CFG_ADVICE_THIS_IS_MEMORY_NOT                2
#define AMI_CFG_ADVICE_THE_PROBLEMS_LISTED_ABOVE         3
#define AMI_CFG_ADVICE_ONE_FILE_PER_NETWORK              4
#define AMI_CFG_ADVICE_ROADSHOW_ACTS_ON_IT               5
#define AMI_CFG_ADVICE_RENAME_THE_FILE_IN                6
#define AMI_CFG_ADVICE_DEVICE_NAMES_THE_DRIVER           7
#define AMI_CFG_ADVICE_ADD_A_LINE_SUCH                   8
#define AMI_CFG_ADVICE_ADD_CONFIGURE_DHCP_TO             9
#define AMI_CFG_ADVICE_THIS_FILE_HOLDS_NAMESERVER        10
#define AMI_CFG_ADVICE_A_ROUTES_FILE_HOLDS               11
#define AMI_CFG_ADVICE_THIS_FILE_SWITCHES_THE            12
#define AMI_CFG_ADVICE_KEEP_IT_UNDER_SIXTY               13
#define AMI_CFG_ADVICE_A_TXT_RECORD_HOLDS                14
#define AMI_CFG_ADVICE_AT_MOST_EIGHT_ARE                 15
#define AMI_CFG_ADVICE_UNIT_IS_A_PLAIN                   16
#define AMI_CFG_ADVICE_AN_ADDRESS_IS_FOUR                17
#define AMI_CFG_ADVICE_A_NETMASK_LOOKS_LIKE              18
#define AMI_CFG_ADVICE_THE_GATEWAY_IS_THE                19
#define AMI_CFG_ADVICE_MTU_IS_A_PLAIN                    20
#define AMI_CFG_ADVICE_CONFIGURE_IS_DHCP_LET             21
#define AMI_CFG_ADVICE_IPTYPE_IS_EITHER_A                22
#define AMI_CFG_ADVICE_MDNS_IS_YES_OR                    23
#define AMI_CFG_ADVICE_DOWNGOESOFFLINE_IS_YES_OR         24
#define AMI_CFG_ADVICE_REQUIRESINITDELAY_IS_YES_OR       25
#define AMI_CFG_ADVICE_HARDWAREADDRESS_IS_SIX_HEXADECIMAL 26
#define AMI_CFG_ADVICE_STATE_IS_UP_OR                    27
#define AMI_CFG_ADVICE_AN_INTERFACE_CARRIES_AT           28
#define AMI_CFG_ADVICE_CONFIGURE6_IS_AUTO_FOLLOW         29
#define AMI_CFG_ADVICE_A_NAME_SERVER_IS                  30
#define AMI_CFG_ADVICE_THIS_IS_THE_ADDRESS               31
#define AMI_CFG_ADVICE_WRITE_ON_OR_OFF                   32
#define AMI_CFG_ADVICE_A_LINE_IS_TYPE                    33
#define AMI_CFG_ADVICE_A_PORT_IS_A                       34
#define AMI_CFG_ADVICE_A_SERVICE_NAME_IS                 35
#define AMI_CFG_ADVICE_THE_KEYWORDS_AN_INTERFACE         36

/* "what is wrong" sentences, same table: a code here, the words in the
   command that prints them. */
#define AMI_CFG_SAYS_THERE_IS_NO_DEVS                    37
#define AMI_CFG_SAYS_THE_DEVS_NETINTERFACES_DRAWER       38
#define AMI_CFG_SAYS_DEVICE_HAS_NO_VALUE                 39
#define AMI_CFG_SAYS_THERE_IS_NO_DEVICE                  40
#define AMI_CFG_SAYS_THE_INTERFACE_HAS_NO                41
#define AMI_CFG_SAYS_THE_SERVICE_NAME_IS                 42
#define AMI_CFG_SAYS_THE_TXT_FIELD_IS                    43
#define AMI_CFG_SAYS_THERE_ARE_MORE_SERVICES             44

#endif /* AMINETXDUO_CONFIG_ADVICE_H */
