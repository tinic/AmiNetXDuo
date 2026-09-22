/*
 * AmiNetXDuo, the DHCPv4 bytes at the client boundary.
 *
 * Option 61 is what a server with a reservation for this machine matches on,
 * so its nine bytes are pinned here one by one.  The address-list and text
 * readers are what a lease's routers, name servers, static routes, host name
 * and domain go through before anything shows them; the state name is what
 * the boot marks carry and the state class is what netstatus reports.
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h and nx_api.h before any exec header, as netstack_dhcp_wire.c. */
#include "tx_api.h"
#include "nx_api.h"

#include "netstack_dhcp_wire.h"

#include <stdio.h>
#include <string.h>

#include "aminetxduo/netstack.h"
#include "aminetxduo/netstatus.h"


static unsigned long h_checks;
static unsigned long h_failures;


static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


static void h_case_client_id_bytes(void)
{
    /* 00:80:10:12:34:56, an A2065, as NetX Duo keeps it: the top two bytes in
       the msw, the low four in the lsw. */
    static const UCHAR want[9] = { 61, 7, 0x01,
                                   0x00, 0x80, 0x10, 0x12, 0x34, 0x56 };
    UCHAR option[16];
    UINT  n;

    memset(option, 0xEE, sizeof(option));
    n = ami_ns_dhcp_client_id_build(0x00000080UL, 0x10123456UL, option, 9U);

    h_check(n == 9U, "option 61 is nine bytes");
    h_check(option[0] == 61, "byte 0 is the option code, 61");
    h_check(option[1] == 7, "byte 1 is the length, 7");
    h_check(option[2] == 0x01, "byte 2 is hardware type 1, Ethernet");
    h_check(memcmp(option + 3, want + 3, 6) == 0,
            "bytes 3..8 are the MAC, most significant first");
    h_check(memcmp(option, want, 9) == 0, "the whole option matches");
    h_check(option[9] == 0xEE, "nothing is written past the option");

    /* A MAC with every byte distinct and the high bit set, so a sign or a
       shift mistake would show. */
    memset(option, 0, sizeof(option));
    n = ami_ns_dhcp_client_id_build(0x0000FEDCUL, 0xBA987654UL, option, 16U);
    h_check(n == 9U && option[3] == 0xFE && option[4] == 0xDC &&
            option[5] == 0xBA && option[6] == 0x98 && option[7] == 0x76 &&
            option[8] == 0x54,
            "fe:dc:ba:98:76:54 comes out in wire order");

    /* Only the low sixteen bits of the msw are the MAC. */
    n = ami_ns_dhcp_client_id_build(0xFFFF0080UL, 0x10123456UL, option, 16U);
    h_check(n == 9U && option[3] == 0x00 && option[4] == 0x80,
            "bits above the MAC in the msw are not written");
}


static void h_case_client_id_room(void)
{
    UCHAR option[16];
    UINT  n;

    memset(option, 0xEE, sizeof(option));
    n = ami_ns_dhcp_client_id_build(0x00000080UL, 0x10123456UL, option, 8U);
    h_check(n == 0U, "eight bytes of room is not enough");
    h_check(option[0] == 0xEE, "a short buffer is left alone");

    n = ami_ns_dhcp_client_id_build(0x00000080UL, 0x10123456UL, option, 0U);
    h_check(n == 0U, "no room, no option");

    n = ami_ns_dhcp_client_id_build(0x00000080UL, 0x10123456UL, NULL, 16U);
    h_check(n == 0U, "no buffer, no option");
}


static void h_case_addr_list(void)
{
    static const UCHAR two[8] = { 192, 168, 1, 1,  10, 0, 0, 1 };
    static const UCHAR zero_between[12] = { 192, 168, 1, 1,  0, 0, 0, 0,
                                            10, 0, 0, 1 };
    static const UCHAR ragged[7] = { 192, 168, 1, 1,  10, 0, 0 };
    ULONG out[AMI_DHCP_MAX_ADDRS];
    UCHAR many[AMI_DHCP_MAX_ADDRS * 4 + 4];
    UWORD n;
    UWORD i;

    memset(out, 0xEE, sizeof(out));
    n = ami_ns_dhcp_addr_list_decode(two, 8U, out, AMI_DHCP_MAX_ADDRS);
    h_check(n == 2U, "eight bytes are two addresses");
    h_check(out[0] == 0xC0A80101UL, "the first is 192.168.1.1, host order");
    h_check(out[1] == 0x0A000001UL, "the second is 10.0.0.1");
    h_check(out[2] == 0xEEEEEEEEUL, "nothing is written past the count");

    n = ami_ns_dhcp_addr_list_decode(zero_between, 12U, out,
                                     AMI_DHCP_MAX_ADDRS);
    h_check(n == 2U && out[0] == 0xC0A80101UL && out[1] == 0x0A000001UL,
            "0.0.0.0 is dropped and the list closes up");

    n = ami_ns_dhcp_addr_list_decode(ragged, 7U, out, AMI_DHCP_MAX_ADDRS);
    h_check(n == 1U && out[0] == 0xC0A80101UL,
            "a trailing partial address is dropped");

    n = ami_ns_dhcp_addr_list_decode(two, 3U, out, AMI_DHCP_MAX_ADDRS);
    h_check(n == 0U, "under four bytes is no address");

    n = ami_ns_dhcp_addr_list_decode(two, 0U, out, AMI_DHCP_MAX_ADDRS);
    h_check(n == 0U, "an empty option is no address");

    /* One more than fits: the cap holds and the first `max` are kept. */
    for (i = 0; i < (UWORD)sizeof(many); i++)
        many[i] = (UCHAR)(i + 1);
    memset(out, 0xEE, sizeof(out));
    n = ami_ns_dhcp_addr_list_decode(many, (UINT)sizeof(many), out,
                                     AMI_DHCP_MAX_ADDRS);
    h_check(n == (UWORD)AMI_DHCP_MAX_ADDRS, "the count stops at max");
    h_check(out[0] == 0x01020304UL, "the first of a long list is kept");
    h_check(out[AMI_DHCP_MAX_ADDRS - 1] ==
                ((ULONG)many[(AMI_DHCP_MAX_ADDRS - 1) * 4] << 24 |
                 (ULONG)many[(AMI_DHCP_MAX_ADDRS - 1) * 4 + 1] << 16 |
                 (ULONG)many[(AMI_DHCP_MAX_ADDRS - 1) * 4 + 2] << 8 |
                 (ULONG)many[(AMI_DHCP_MAX_ADDRS - 1) * 4 + 3]),
            "the last kept is the max-th on the wire");

    n = ami_ns_dhcp_addr_list_decode(two, 8U, out, 0U);
    h_check(n == 0U, "room for none keeps none");

    n = ami_ns_dhcp_addr_list_decode(NULL, 8U, out, AMI_DHCP_MAX_ADDRS);
    h_check(n == 0U, "no buffer, no addresses");
}


static void h_case_text(void)
{
    static const UCHAR amiga[5] = { 'a', 'm', 'i', 'g', 'a' };
    char out[8];

    memset(out, 'x', sizeof(out));
    ami_ns_dhcp_text_decode(amiga, 5U, out, sizeof(out));
    h_check(strcmp(out, "amiga") == 0, "five bytes come out as \"amiga\"");
    h_check(out[6] == 'x', "nothing past the NUL is touched");

    /* The wire has no terminator; a name as long as the buffer is cut. */
    memset(out, 'x', sizeof(out));
    ami_ns_dhcp_text_decode((const UCHAR *)"workbench", 9U, out, 4U);
    h_check(strcmp(out, "wor") == 0, "a long name is cut to outlen - 1");
    h_check(out[3] == '\0' && out[4] == 'x', "the cut leaves the rest alone");

    memset(out, 'x', sizeof(out));
    ami_ns_dhcp_text_decode((const UCHAR *)"work", 4U, out, 5U);
    h_check(strcmp(out, "work") == 0, "a name of exactly outlen - 1 fits");

    memset(out, 'x', sizeof(out));
    ami_ns_dhcp_text_decode((const UCHAR *)"work", 4U, out, 4U);
    h_check(strcmp(out, "wor") == 0, "a name of exactly outlen loses a byte");

    memset(out, 'x', sizeof(out));
    ami_ns_dhcp_text_decode(amiga, 0U, out, sizeof(out));
    h_check(out[0] == '\0', "an empty option is an empty string");

    memset(out, 'x', sizeof(out));
    ami_ns_dhcp_text_decode(NULL, 5U, out, sizeof(out));
    h_check(out[0] == '\0', "no buffer is an empty string");
}


static void h_case_state_name(void)
{
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_NOT_STARTED),
                   "dhcp-notstarted") == 0, "0 is dhcp-notstarted");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_BOOT),
                   "dhcp-boot") == 0, "1 is dhcp-boot");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_INIT),
                   "dhcp-init") == 0, "2 is dhcp-init");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_SELECTING),
                   "dhcp-selecting") == 0, "3 is dhcp-selecting");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_REQUESTING),
                   "dhcp-requesting") == 0, "4 is dhcp-requesting");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_BOUND),
                   "dhcp-bound") == 0, "5 is dhcp-bound");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_RENEWING),
                   "dhcp-renewing") == 0, "6 is dhcp-renewing");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_REBINDING),
                   "dhcp-rebinding") == 0, "7 is dhcp-rebinding");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_FORCERENEW),
                   "dhcp-forcerenew") == 0, "8 is dhcp-forcerenew");
    h_check(strcmp(ami_ns_dhcp_state_name(NETSTATUS_DHCPRAW_PROBING),
                   "dhcp-probing") == 0, "9 is dhcp-probing");
    h_check(strcmp(ami_ns_dhcp_state_name(10), "dhcp-other") == 0,
            "10 is past the table");
    h_check(strcmp(ami_ns_dhcp_state_name(255), "dhcp-other") == 0,
            "255 is past the table");
}


static void h_case_state_class(void)
{
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_NOT_STARTED)
                == AMI_DHCP_IDLE, "not started is idle");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_BOOT)
                == AMI_DHCP_WORKING, "boot is working");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_INIT)
                == AMI_DHCP_WORKING, "init is working");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_SELECTING)
                == AMI_DHCP_WORKING, "selecting is working");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_REQUESTING)
                == AMI_DHCP_WORKING, "requesting is working");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_BOUND)
                == AMI_DHCP_BOUND, "bound is bound");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_RENEWING)
                == AMI_DHCP_BOUND, "renewing still holds the lease");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_REBINDING)
                == AMI_DHCP_BOUND, "rebinding still holds the lease");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_FORCERENEW)
                == AMI_DHCP_WORKING, "forcerenew is working");
    h_check(ami_ns_dhcp_state_class(NETSTATUS_DHCPRAW_PROBING)
                == AMI_DHCP_WORKING, "probing is working");
    h_check(ami_ns_dhcp_state_class(200) == AMI_DHCP_WORKING,
            "an unknown state is working, not idle and not bound");
}


int main(void)
{
    h_case_client_id_bytes();
    h_case_client_id_room();
    h_case_addr_list();
    h_case_text();
    h_case_state_name();
    h_case_state_class();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
