/*
 * AmiNetXDuo, S2_CopyFromBuff with NetX Duo's own deferred checksum, on the
 * host.
 *
 * test_sana2_copy_host.c counts _nx_ip_packet_checksum_compute() calls; this
 * one links the real routine, so a device that reads part of a frame before
 * copying all of it is checked against the checksum the peer will verify.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include <stdio.h>
#include <string.h>
#include "aminetxduo/asm_abi.h"


static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
    }
}


/* src/net68k/n68k_copy.S and n68k_checksum.c on the target; the same
   contracts here, as in test_sana2_copy_host.c. */
AMIGA_ASM_ARGS VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{
    if (len != 0)
        memcpy(to, from, (size_t)len);
}

AMIGA_ASM_ARGS ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count)
{
    ULONG acc = 0;

    /* Summed as the 68k loads them, big-endian: the fold in sana2_copy.c
       takes the sum as a network-order value. */
    while (count != 0UL)
    {
        const UCHAR *b = (const UCHAR *)from;
        ULONG w = ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
                  ((ULONG)b[2] << 8) | (ULONG)b[3];

        *to++ = *from++;
        acc += w;
        if (acc < w)
            acc++;
        count--;
    }

    return acc;
}


/* nx_ip_checksum_compute.c references it on a path these packets do not
   take. */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (VOID)timer_ticks;
    return 0;
}


/* RFC 1071 over the pseudo-header and segment, independent of both paths. */
static unsigned tcp_checksum(const UCHAR *ip, ULONG total)
{
    ULONG ihl = (ULONG)(ip[0] & 0x0F) * 4UL;
    ULONG tcp_len = total - ihl;
    ULONG sum = 0;
    ULONG i;

    for (i = 12; i < 20; i += 2)
        sum += ((ULONG)ip[i] << 8) | ip[i + 1];
    sum += 6UL + tcp_len;

    for (i = 0; i + 1 < tcp_len; i += 2)
    {
        if (i == 16)
            continue;                       /* the checksum field itself    */
        sum += ((ULONG)ip[ihl + i] << 8) | ip[ihl + i + 1];
    }
    if (tcp_len & 1UL)
        sum += (ULONG)ip[ihl + tcp_len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);
    sum = (~sum) & 0xFFFFUL;
    return (unsigned)(sum == 0 ? 0xFFFFUL : sum);
}


#define DGRAM_LEN   60

static UCHAR     dgram[DGRAM_LEN];
static NX_PACKET pkt;

/* A cooked IPv4 TCP segment as NetX Duo hands it to the interface: checksum
   field zero, the TCP checksum owed by the interface. */
static void packet_init(void)
{
    ULONG i;

    memset(dgram, 0, sizeof(dgram));
    dgram[0]  = 0x45;
    dgram[3]  = DGRAM_LEN;
    dgram[8]  = 64;
    dgram[9]  = 6;
    dgram[12] = 10;  dgram[15] = 1;
    dgram[16] = 10;  dgram[19] = 2;
    dgram[20] = 0x30; dgram[21] = 0x39;
    dgram[22] = 0x00; dgram[23] = 0x50;
    dgram[32] = 0x50;
    dgram[33] = 0x10;                       /* ACK                          */
    for (i = 40; i < DGRAM_LEN; i++)
        dgram[i] = (UCHAR)((i * 11 + 3) & 0xFF);

    memset(&pkt, 0, sizeof(pkt));
    pkt.nx_packet_prepend_ptr = dgram;
    pkt.nx_packet_append_ptr  = dgram + DGRAM_LEN;
    pkt.nx_packet_length      = DGRAM_LEN;
    pkt.nx_packet_ip_version  = NX_IP_VERSION_V4;
    pkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
}

static void slot_init(AmiTxSlot *slot, AmiSana2If *iface)
{
    memset(iface, 0, sizeof(*iface));
    iface->raw_mode = FALSE;
    memset(slot, 0, sizeof(*slot));
    slot->packet = &pkt;
    slot->total  = DGRAM_LEN;
    slot->iface  = iface;
}

static unsigned field(const UCHAR *ip)
{
    return ((unsigned)ip[36] << 8) | ip[37];
}


/* 1. The whole frame in one call: the fused path. */
static void test_whole(void)
{
    static AmiSana2If iface;
    AmiTxSlot         slot;
    UCHAR             out[DGRAM_LEN];
    unsigned          want;

    printf("sana2: whole-frame copy, fused checksum\n");
    packet_init();
    want = tcp_checksum(dgram, DGRAM_LEN);
    slot_init(&slot, &iface);

    h_check(ami_sana2_copy_from_buff(out, &slot, DGRAM_LEN) == TRUE,
            "whole: copied");
    h_check(field(out) == want, "whole: the wire checksum is correct");
}

/* 2. A device reads the first 34 bytes (an Ethernet-less header peek: IPv4
   header and the TCP ports), then copies the whole frame. */
static void test_peek_then_whole(ULONG peek)
{
    static AmiSana2If iface;
    AmiTxSlot         slot;
    UCHAR             head[DGRAM_LEN];
    UCHAR             out[DGRAM_LEN];
    unsigned          want;
    char              what[96];

    printf("sana2: %lu-byte peek, then the whole frame\n", (unsigned long)peek);
    packet_init();
    want = tcp_checksum(dgram, DGRAM_LEN);
    slot_init(&slot, &iface);

    h_check(ami_sana2_copy_from_buff(head, &slot, peek) == TRUE,
            "peek: copied");
    h_check(memcmp(head, dgram, 20) == 0 || peek < 20,
            "peek: the IPv4 header is as sent");
    h_check(ami_sana2_copy_from_buff(out, &slot, DGRAM_LEN) == TRUE,
            "then whole: copied");
    snprintf(what, sizeof(what),
             "then whole: the wire checksum is correct (got %04x, want %04x)",
             field(out), want);
    h_check(field(out) == want, what);
    h_check((pkt.nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) == 0,
            "then whole: the checksum is no longer owed");
}

/* 3. The same frame copied twice whole (a device that rebuilds a write). */
static void test_whole_twice(void)
{
    static AmiSana2If iface;
    AmiTxSlot         slot;
    UCHAR             out[DGRAM_LEN];
    UCHAR             again[DGRAM_LEN];
    unsigned          want;

    printf("sana2: whole-frame copy twice\n");
    packet_init();
    want = tcp_checksum(dgram, DGRAM_LEN);
    slot_init(&slot, &iface);

    h_check(ami_sana2_copy_from_buff(out, &slot, DGRAM_LEN) == TRUE &&
            ami_sana2_copy_from_buff(again, &slot, DGRAM_LEN) == TRUE,
            "twice: copied");
    h_check(field(out) == want && field(again) == want,
            "twice: both copies carry the correct checksum");
}


/* 4. A bare ACK: a 40-byte datagram, which ami_sana2_tx_pad() lengthens by
   6 zero bytes to the 60-byte Ethernet minimum (46 cooked).  nx_packet_length
   and the slot's total then count the pad; the TCP length does not. */
#define ACK_LEN     40
#define ACK_PAD     6

static UCHAR     ack[ACK_LEN + ACK_PAD];
static NX_PACKET ackpkt;

static void ack_init(void)
{
    memset(ack, 0, sizeof(ack));
    ack[0]  = 0x45;
    ack[3]  = ACK_LEN;
    ack[8]  = 64;
    ack[9]  = 6;
    ack[12] = 10;  ack[15] = 1;
    ack[16] = 10;  ack[19] = 2;
    ack[20] = 0x30; ack[21] = 0x39;
    ack[22] = 0x00; ack[23] = 0x50;
    ack[24] = 0x12; ack[27] = 0x34;         /* sequence                     */
    ack[28] = 0x56; ack[31] = 0x78;         /* acknowledgement              */
    ack[32] = 0x50;
    ack[33] = 0x10;                         /* ACK                          */
    ack[34] = 0xFA; ack[35] = 0xF0;         /* window                       */

    memset(&ackpkt, 0, sizeof(ackpkt));
    ackpkt.nx_packet_prepend_ptr = ack;
    ackpkt.nx_packet_append_ptr  = ack + ACK_LEN + ACK_PAD;
    ackpkt.nx_packet_length      = ACK_LEN + ACK_PAD;
    ackpkt.nx_packet_ip_version  = NX_IP_VERSION_V4;
    ackpkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;
}

static void test_padded_ack(ULONG peek)
{
    static AmiSana2If iface;
    AmiTxSlot         slot;
    UCHAR             head[ACK_LEN + ACK_PAD];
    UCHAR             out[ACK_LEN + ACK_PAD];
    unsigned          want;
    char              what[112];

    printf("sana2: padded 40-byte ACK, %lu-byte peek first\n",
           (unsigned long)peek);
    ack_init();
    want = tcp_checksum(ack, ACK_LEN);

    memset(&iface, 0, sizeof(iface));
    iface.raw_mode = FALSE;
    memset(&slot, 0, sizeof(slot));
    slot.packet  = &ackpkt;
    slot.total   = ACK_LEN + ACK_PAD;
    slot.pad_len = ACK_PAD;
    slot.iface   = &iface;

    if (peek != 0)
        h_check(ami_sana2_copy_from_buff(head, &slot, peek) == TRUE,
                "padded ACK: peek copied");
    h_check(ami_sana2_copy_from_buff(out, &slot, ACK_LEN + ACK_PAD) == TRUE,
            "padded ACK: whole frame copied");
    snprintf(what, sizeof(what),
             "padded ACK: the wire checksum is correct (got %04x, want %04x)",
             field(out), want);
    h_check(field(out) == want, what);
}

/* 5. The padded ACK taken in 8-byte chunks, as a device feeding a FIFO. */
static void test_padded_ack_chunked(void)
{
    static AmiSana2If iface;
    AmiTxSlot         slot;
    UCHAR             out[ACK_LEN + ACK_PAD];
    ULONG             at;
    BOOL              ok = TRUE;
    unsigned          want;
    char              what[112];

    printf("sana2: padded 40-byte ACK in 8-byte chunks\n");
    ack_init();
    want = tcp_checksum(ack, ACK_LEN);

    memset(&iface, 0, sizeof(iface));
    iface.raw_mode = FALSE;
    memset(&slot, 0, sizeof(slot));
    slot.packet  = &ackpkt;
    slot.total   = ACK_LEN + ACK_PAD;
    slot.pad_len = ACK_PAD;
    slot.iface   = &iface;

    for (at = 0; at < ACK_LEN + ACK_PAD; at += 8)
    {
        ULONG n = (ACK_LEN + ACK_PAD - at < 8) ? ACK_LEN + ACK_PAD - at : 8;

        if (ami_sana2_copy_from_buff(out + at, &slot, n) != TRUE)
            ok = FALSE;
    }
    h_check(ok, "chunked ACK: every chunk copied");
    snprintf(what, sizeof(what),
             "chunked ACK: the wire checksum is correct (got %04x, want %04x)",
             field(out), want);
    h_check(field(out) == want, what);
}

/* 6. A 52-byte ACK with 12 bytes of TCP options: long enough to need no pad,
   the shape that stayed correct on the A1200. */
#define OPT_LEN     52

static UCHAR     opt[OPT_LEN];
static NX_PACKET optpkt;

static void test_unpadded_option_ack(void)
{
    static AmiSana2If iface;
    AmiTxSlot         slot;
    UCHAR             head[OPT_LEN];
    UCHAR             out[OPT_LEN];
    unsigned          want;
    char              what[112];

    printf("sana2: 52-byte ACK with options, 34-byte peek first\n");
    memset(opt, 0, sizeof(opt));
    opt[0]  = 0x45;
    opt[3]  = OPT_LEN;
    opt[8]  = 64;
    opt[9]  = 6;
    opt[12] = 10;  opt[15] = 1;
    opt[16] = 10;  opt[19] = 2;
    opt[20] = 0x30; opt[21] = 0x39;
    opt[22] = 0x00; opt[23] = 0x50;
    opt[27] = 0x34; opt[31] = 0x78;
    opt[32] = 0x80;                         /* data offset 8: 12 of options */
    opt[33] = 0x10;
    opt[34] = 0xFA; opt[35] = 0xF0;
    opt[40] = 1; opt[41] = 1;               /* NOP NOP                      */
    opt[42] = 8; opt[43] = 10;              /* timestamps                   */
    opt[44] = 0x11; opt[47] = 0x22; opt[48] = 0x33; opt[51] = 0x44;
    want = tcp_checksum(opt, OPT_LEN);

    memset(&optpkt, 0, sizeof(optpkt));
    optpkt.nx_packet_prepend_ptr = opt;
    optpkt.nx_packet_append_ptr  = opt + OPT_LEN;
    optpkt.nx_packet_length      = OPT_LEN;
    optpkt.nx_packet_ip_version  = NX_IP_VERSION_V4;
    optpkt.nx_packet_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM;

    memset(&iface, 0, sizeof(iface));
    iface.raw_mode = FALSE;
    memset(&slot, 0, sizeof(slot));
    slot.packet = &optpkt;
    slot.total  = OPT_LEN;
    slot.iface  = &iface;

    h_check(ami_sana2_copy_from_buff(head, &slot, 34) == TRUE &&
            ami_sana2_copy_from_buff(out, &slot, OPT_LEN) == TRUE,
            "option ACK: copied");
    snprintf(what, sizeof(what),
             "option ACK: the wire checksum is correct (got %04x, want %04x)",
             field(out), want);
    h_check(field(out) == want, what);
}

int main(void)
{
    test_whole();
    test_peek_then_whole(34);
    test_peek_then_whole(20);
    test_peek_then_whole(4);
    test_whole_twice();
    test_padded_ack(0);
    test_padded_ack(34);
    test_padded_ack_chunked();
    test_unpadded_option_ack();

    printf("%lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}
