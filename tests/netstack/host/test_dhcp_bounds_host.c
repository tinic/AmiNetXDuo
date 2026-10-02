/*
 * AmiNetXDuo, DHCP option-buffer bounds (N-060 truncated-option boundaries).
 *
 * nxd_dhcp_client.c's _nx_dhcp_search_buffer walks a DHCP option area and
 * returns the size byte of a requested option.  It had three defects:
 *
 *   (1) match guard: the presence test was `if ((i + size + 1) > length)`.  An
 *       option at code byte i with size s occupies bytes i .. i + s + 1 (last
 *       data byte at i + 1 + s), so it is present only when i + s + 2 <= length.
 *       The +1 form accepted the case i + s + 2 == length + 1, i.e. the option's
 *       final data byte would lie one past the options area, which the caller
 *       then reads (stale pool content).
 *
 *   (2) skip branch: after passing an unrelated option the index advanced
 *       `i += size + 1`, one short of the option's true size + 2, so the loop
 *       guard and the match guard both under-counted, admitting options
 *       truncated by one MORE byte than (1) alone.
 *
 *   (3) zero-length underflow: the loop entered on `while (i < length - 1)`,
 *       which underflows to UINT_MAX for length == 0 and then walks the (empty)
 *       area as if it held an option.
 *
 * This drives the real _nx_dhcp_search_buffer (compiled with -Dstatic= and
 * linked with --gc-sections) at the exact boundaries, and the real
 * _nx_dhcp_get_option_value end-to-end where the over-read is an actual
 * heap-buffer-overflow caught by ASan.  The corrected source rejects every
 * truncated option, reads nothing past the area, and still accepts the
 * zero-, one-, and exact-fit options; the buggy source admits the truncated
 * options and walks an empty area.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The unit under test is static in the source; the harness compiles it with
   -Dstatic= so these externs bind to the real bodies. */
UCHAR *_nx_dhcp_search_buffer(UCHAR *option_message, UINT option, UINT length);
UINT   _nx_dhcp_get_option_value(UCHAR *bootp_message, UINT option,
                                 ULONG *value, UINT length);

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

/* ---- link-only stubs ---- */
/*
 * The source is compiled with -Dstatic= (so the static unit-under-test links)
 * and --gc-sections (so the unused DHCP machinery is discarded).  ASan's global
 * registration keeps a few of those sections alive even though nothing in main
 * reaches them; these stubs satisfy the linker for their nx and tx externs.
 * None of them is ever called: main reaches only _nx_dhcp_search_buffer and
 * _nx_dhcp_get_option_value, which call no external symbol.
 */
UINT   _nx_arp_probe_send(NX_IP *ip_ptr, UINT interface_index, ULONG probe_address)
{ (void)ip_ptr; (void)interface_index; (void)probe_address; return 0; }
USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol, UINT data_length,
                               ULONG *src, ULONG *dst)
{ (void)packet_ptr; (void)protocol; (void)data_length; (void)src; (void)dst; return 0; }
UINT   _nx_ip_interface_address_get(NX_IP *ip_ptr, UINT interface_index,
                                    ULONG *ip_address, ULONG *network_mask)
{ (void)ip_ptr; (void)interface_index; (void)ip_address; (void)network_mask; return 0; }
UINT   _nx_ip_interface_address_set(NX_IP *ip_ptr, UINT interface_index,
                                    ULONG ip_address, ULONG network_mask)
{ (void)ip_ptr; (void)interface_index; (void)ip_address; (void)network_mask; return 0; }
UINT   _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                           ULONG packet_type, ULONG wait_option)
{ (void)pool_ptr; (void)packet_ptr; (void)packet_type; (void)wait_option; return 0; }
UINT   _nx_packet_release(NX_PACKET *packet_ptr)
{ (void)packet_ptr; return 0; }
UINT   _nx_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                                  ULONG ip_address, UINT port, UINT address_index)
{ (void)socket_ptr; (void)packet_ptr; (void)ip_address; (void)port; (void)address_index; return 0; }
UINT   _nx_utility_string_length_check(CHAR *input_string, UINT *string_length,
                                       UINT max_string_length)
{ (void)input_string; (void)string_length; (void)max_string_length; return 0; }
UINT   _tx_thread_interrupt_disable(void)
{ return 0; }
VOID   _tx_thread_interrupt_restore(UINT previous_posture)
{ (void)previous_posture; }
UINT   _txe_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set, UINT set_option)
{ (void)group_ptr; (void)flags_to_set; (void)set_option; return 0; }
UINT   _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{ (void)mutex_ptr; (void)wait_option; return 0; }
UINT   _txe_mutex_put(TX_MUTEX *mutex_ptr)
{ (void)mutex_ptr; return 0; }

int main(void)
{
    UCHAR  sbuf[16];
    UCHAR *r;

    /* Unbuffered so the failing checks are visible even when the buggy build
       aborts in the ASan over-read cases below. */
    setbuf(stdout, NULL);

    printf("DHCP option-buffer bounds, truncated-option and empty-area boundaries\n");

    /* ---- Case 1: exact-fit control.  subnet-mask(1) size 4, 4 data bytes;
       option occupies bytes 0..5 of a 6-byte area.  Present: accept. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK; /* 1 */
    sbuf[1] = 4;
    sbuf[2] = 0x0A; sbuf[3] = 0x0B; sbuf[4] = 0x0C; sbuf[5] = 0x0D;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 6);
    h_check(r == &sbuf[1], "exact-fit: size byte returned for a fully present option");

    /* ---- Case 2: match-guard off-by-one.  subnet-mask(1) size 4 but only 3
       data bytes present (area length 5): the option's 4th data byte would sit
       at index 5, one past the area.  Reject; the buggy +1 guard admits it. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK;
    sbuf[1] = 4;
    sbuf[2] = 0x0A; sbuf[3] = 0x0B; sbuf[4] = 0x0C;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 5);
    h_check(r == NULL, "match-guard: option truncated by one byte is rejected");

    /* ---- Case 3: skip-branch index lag.  A gateways(3) option (size 4) fills
       bytes 0..5, then subnet-mask(1) size 4 starts at byte 6 but only 2 of its
       4 data bytes are present (area length 10).  The lagged `i += size + 1`
       lets the buggy source still match the truncated option. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_GATEWAYS;   /* 3 */
    sbuf[1] = 4;
    sbuf[2] = 0; sbuf[3] = 0; sbuf[4] = 0; sbuf[5] = 0;
    sbuf[6] = NX_DHCP_OPTION_SUBNET_MASK; /* 1 */
    sbuf[7] = 4;
    sbuf[8] = 0x0A; sbuf[9] = 0x0B;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 10);
    h_check(r == NULL, "skip-lag: option after a skip, truncated, is rejected");

    /* ---- Case 4: skip-lag alone (truncated by exactly one byte, one prior
       skip).  Correct i would reject via i + s + 2 == length + 1; the lagged i
       under-counts by one more and admits it.  Only the full fix rejects. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_GATEWAYS;
    sbuf[1] = 4;
    sbuf[2] = 0; sbuf[3] = 0; sbuf[4] = 0; sbuf[5] = 0;
    sbuf[6] = NX_DHCP_OPTION_SUBNET_MASK;
    sbuf[7] = 4;
    sbuf[8] = 0x0A; sbuf[9] = 0x0B; sbuf[10] = 0x0C;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 11);
    h_check(r == NULL, "skip-lag: exactly-one-byte-truncated option after a skip is rejected");

    /* ---- Case 5: zero-length area underflow.  An empty option area must not
       be read at all.  A heap buffer keeps ASan honest: the buggy `i < length-1`
       underflows and reads buf[0..] past the two allocated bytes. ---- */
    {
        UCHAR *z = (UCHAR *)malloc(2);
        z[0] = NX_DHCP_OPTION_GATEWAYS;   /* not END/PAD, so the buggy walk advances */
        z[1] = 4;
        r = _nx_dhcp_search_buffer(z, NX_DHCP_OPTION_SUBNET_MASK, 0);
        h_check(r == NULL, "zero-length: empty area returns NULL without reading");
        free(z);
    }

    /* ---- Case 6: one-byte area (a bare code byte, no size byte).  There is
       no complete option; reject. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 1);
    h_check(r == NULL, "one-byte: code byte with no size byte is rejected");

    /* ---- Case 7: zero-byte option control.  size=0 with no data occupies two
       bytes; present. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK;
    sbuf[1] = 0;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 2);
    h_check(r == &sbuf[1], "zero-byte option: size=0 with no data is present");

    /* ---- Case 8: one-byte option control.  size=1 with its data byte occupies
       three bytes; present. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK;
    sbuf[1] = 1;
    sbuf[2] = 0xAA;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 3);
    h_check(r == &sbuf[1], "one-byte option: size=1 with its data byte is present");

    /* ---- Case 9: truncated one-byte option.  size=1 claims one data byte but
       none is present (area length 2); reject. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK;
    sbuf[1] = 1;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 2);
    h_check(r == NULL, "truncated one-byte option: size=1 with no data is rejected");

    /* ---- Case 10: truncated multi-byte option (zero data present).  size=4
       claims four data bytes but none is present (area length 2); reject. ---- */
    memset(sbuf, 0, sizeof(sbuf));
    sbuf[0] = NX_DHCP_OPTION_SUBNET_MASK;
    sbuf[1] = 4;
    r = _nx_dhcp_search_buffer(sbuf, NX_DHCP_OPTION_SUBNET_MASK, 2);
    h_check(r == NULL, "truncated option: size=4 with zero data bytes is rejected");

    /* ---- Case 11: end-to-end over-read.  _nx_dhcp_get_option_value reads the
       size byte then _nx_dhcp_get_data reads `size` data bytes; for a truncated
       option the buggy source reads past the allocated buffer (ASan).  The
       corrected source returns NX_OPTION_ERROR and never touches the data. ---- */
    {
        UCHAR *bootp = (UCHAR *)malloc(NX_BOOTP_OFFSET_OPTIONS + 5);
        ULONG  value = 0xDEADBEEF;

        memset(bootp, 0, NX_BOOTP_OFFSET_OPTIONS + 5);
        bootp[NX_BOOTP_OFFSET_OPTIONS + 0] = NX_DHCP_OPTION_SUBNET_MASK;
        bootp[NX_BOOTP_OFFSET_OPTIONS + 1] = 4;              /* claims 4 data bytes */
        bootp[NX_BOOTP_OFFSET_OPTIONS + 2] = 0x0A;
        bootp[NX_BOOTP_OFFSET_OPTIONS + 3] = 0x0B;
        bootp[NX_BOOTP_OFFSET_OPTIONS + 4] = 0x0C;           /* only 3 present */

        UINT st = _nx_dhcp_get_option_value(bootp, NX_DHCP_OPTION_SUBNET_MASK,
                                            &value, NX_BOOTP_OFFSET_OPTIONS + 5);
        h_check(st == NX_OPTION_ERROR,
                "end-to-end: truncated option returns NX_OPTION_ERROR");
        free(bootp);
    }

    /* ---- Case 12: end-to-end empty option area.  A bootp message with no
       option bytes at all (length == NX_BOOTP_OFFSET_OPTIONS) yields a
       zero-length area, which must return NX_OPTION_ERROR, not read the byte
       past the buffer. ---- */
    {
        UCHAR *bootp = (UCHAR *)malloc(NX_BOOTP_OFFSET_OPTIONS);
        ULONG  value = 0xDEADBEEF;

        memset(bootp, 0, NX_BOOTP_OFFSET_OPTIONS);
        UINT st = _nx_dhcp_get_option_value(bootp, NX_DHCP_OPTION_SUBNET_MASK,
                                            &value, NX_BOOTP_OFFSET_OPTIONS);
        h_check(st == NX_OPTION_ERROR,
                "end-to-end: empty option area returns NX_OPTION_ERROR");
        free(bootp);
    }

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
