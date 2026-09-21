/*
 * The ZZ9000 receive window is stable until its serial is acknowledged.
 * Verify that the ordinary SANA-II path consumes that window directly and
 * that the acknowledgement follows the synchronous receive callback.  This
 * is the lifetime rule which makes one-copy RX possible without an extension.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include "netdev_nic.h"

/* The attach path is discarded from this fixture at link time, but the whole
   core must still compile against the two Exec allocation names it uses. */
#ifndef MEMF_PUBLIC
#define MEMF_PUBLIC (1UL << 0)
#endif
#ifndef MEMF_CLEAR
#define MEMF_CLEAR  (1UL << 16)
#endif
static APTR test_alloc_mem(ULONG bytes, ULONG flags)
{
    (VOID)bytes;
    (VOID)flags;
    return NULL;
}
#define AllocMem test_alloc_mem

/* Include the core so this test can exercise its private one-frame drain. */
#include "zz9000.c"

static union
{
    ULONG align;
    UBYTE bytes[0x10000];
} board;

static NetdevNic nic;
static ZzCore    core;
static int       failures;
static const UBYTE *seen_frame;
static UWORD     seen_len;
static UWORD     ack_during_callback;
static UBYTE     received[NETDEV_RXBUF_MAX];

static VOID expect(int ok, const char *what)
{
    if (ok)
        printf("ok   %s\n", what);
    else
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static VOID receive(APTR unused, const UBYTE *frame, UWORD len)
{
    (VOID)unused;
    seen_frame = frame;
    seen_len = len;
    ack_during_callback = *(volatile UWORD *)(volatile void *)
                          (board.bytes + ZZ_REG_RX_ACK);
    memcpy(received, frame, len);
}

static VOID direct_claim_is_not_available(VOID)
{
    /* Deliberately empty: a NULL hook selects plain SANA-II CopyToBuff. */
    nic.rx_claim = NULL;
}

static VOID ordinary_receive_uses_window_until_callback_returns(VOID)
{
    volatile UWORD *length = (volatile UWORD *)(volatile void *)
                             (board.bytes + ZZ_RX_WINDOW);
    volatile UWORD *serial = length + 1;
    UBYTE *frame = board.bytes + ZZ_RX_WINDOW + ZZ_RX_PAD;
    UWORD i;

    memset(&board, 0, sizeof(board));
    memset(&nic, 0, sizeof(nic));
    memset(&core, 0, sizeof(core));
    memset(received, 0, sizeof(received));
    seen_frame = NULL;
    seen_len = 0;
    ack_during_callback = 0xffff;

    nic.board = board.bytes;
    nic.core = &core;
    nic.rx = receive;
    direct_claim_is_not_available();

    /* Broadcast is accepted without depending on a configured station MAC. */
    for (i = 0; i < 6; i++)
        frame[i] = 0xff;
    for (; i < 60; i++)
        frame[i] = (UBYTE)i;
    *length = 60;
    *serial = 0x1234;

    expect(zz_rint(&nic), "one presented frame is consumed");
    expect(seen_frame == frame, "callback source is the mapped RX window");
    expect(seen_frame != (const UBYTE *)(const void *)nic.rxbuf,
           "payload is not staged in rxbuf");
    expect(seen_len == 60, "callback receives the complete frame length");
    expect(ack_during_callback == 0,
           "slot remains owned while the callback reads it");
    expect(*(volatile UWORD *)(volatile void *)
           (board.bytes + ZZ_REG_RX_ACK) == 0x1234,
           "slot is acknowledged after the callback returns");
    expect(memcmp(received, frame, 60) == 0,
           "callback can read every byte before acknowledgement");
    expect(nic.rx_packets == 1, "receive statistics count the frame");
}

int main(void)
{
    ordinary_receive_uses_window_until_callback_returns();
    printf("%s: zz9000 receive window (%d failure%s)\n",
           failures == 0 ? "PASS" : "FAIL", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
