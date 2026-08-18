/*
 * The Am7990 LANCE core against a small CSR and shared-memory model.
 *
 * Real LANCE parts clear CSR0.TXON after a transmit underflow or buffer
 * error.  Emulators generally do not produce that condition, so this test
 * makes the stopped state explicit and verifies that the core reinitialises
 * instead of remaining "running" but silent.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include "netdev_nic.h"
#include "lancereg.h"

static UWORD mock_csr[4];
static int   failures;

static UWORD mock_csr_get(NetdevNic *nic, UWORD csr);
static VOID  mock_csr_put(NetdevNic *nic, UWORD csr, UWORD value);

#define LANCE_CSR_GET(nic, csr)       mock_csr_get((nic), (csr))
#define LANCE_CSR_PUT(nic, csr, val)  mock_csr_put((nic), (csr), (val))

#include "lance.c"

static union
{
    ULONG align;
    UBYTE bytes[LE_END + 16];
} mock_board;

static NetdevCard card;
static NetdevNic  nic;

static VOID expect_u32(const char *what, ULONG got, ULONG want)
{
    if (got == want)
    {
        printf("ok   %s = %lu\n", what, (unsigned long)got);
        return;
    }

    printf("FAIL %s: got %lu, want %lu\n", what,
           (unsigned long)got, (unsigned long)want);
    failures++;
}

/* The transmit copy is linked into lance.c but these interrupt tests never
   call it.  Keep the definition honest for any later transmit fixture. */
VOID n68k_copy_longs(volatile void *to, const volatile void *from, ULONG longs)
{
    volatile ULONG       *dst = (volatile ULONG *)to;
    const volatile ULONG *src = (const volatile ULONG *)from;

    while (longs-- != 0)
        *dst++ = *src++;
}

static UWORD mock_csr_get(NetdevNic *unused, UWORD csr)
{
    (VOID)unused;
    return (csr < 4) ? mock_csr[csr] : 0;
}

static VOID mock_csr_put(NetdevNic *unused, UWORD csr, UWORD value)
{
    UWORD ack = (UWORD)(LE_C0_BABL | LE_C0_CERR | LE_C0_MISS |
                        LE_C0_MERR | LE_C0_RINT | LE_C0_TINT |
                        LE_C0_IDON);

    (VOID)unused;

    if (csr != LE_CSR0)
    {
        if (csr < 4)
            mock_csr[csr] = value;
        return;
    }

    if ((value & LE_C0_STOP) != 0)
    {
        mock_csr[0] = LE_C0_STOP;
        return;
    }

    if ((value & LE_C0_INIT) != 0)
    {
        /* The mock completes the 24-byte init-block DMA immediately. */
        mock_csr[0] = LE_C0_IDON;
        return;
    }

    mock_csr[0] &= (UWORD)~(value & ack);

    if ((mock_csr[0] & (LE_C0_BABL | LE_C0_CERR | LE_C0_MISS |
                        LE_C0_MERR | LE_C0_RINT | LE_C0_TINT |
                        LE_C0_IDON)) == 0)
        mock_csr[0] &= (UWORD)~LE_C0_INTR;

    if ((value & LE_C0_STRT) != 0)
        mock_csr[0] |= (UWORD)(LE_C0_RXON | LE_C0_TXON);
    if ((value & LE_C0_INEA) != 0)
        mock_csr[0] |= LE_C0_INEA;
}

static VOID fixture_init(VOID)
{
    memset(&mock_board, 0, sizeof(mock_board));
    memset(&card, 0, sizeof(card));
    memset(&nic, 0, sizeof(nic));
    memset(mock_csr, 0, sizeof(mock_csr));

    card.mem_off  = 0;
    card.mem_size = sizeof(mock_board.bytes);
    nic.board     = mock_board.bytes;
    nic.card      = &card;

    expect_u32("initialise", (ULONG)lance_init(&nic), 0);
    expect_u32("initial RXON", mock_csr[0] & LE_C0_RXON, LE_C0_RXON);
    expect_u32("initial TXON", mock_csr[0] & LE_C0_TXON, LE_C0_TXON);
}

static VOID test_normal_completion(VOID)
{
    fixture_init();

    nic.txb_inuse = 1;
    nic.tx_done   = 0;
    le_put16(&nic, LE_TXD_OFF + 2, LE_T1_ONE);
    le_put16(&nic, LE_TXD_OFF + 6, 0);
    mock_csr[0] = (UWORD)(LE_C0_INTR | LE_C0_TINT |
                          LE_C0_RXON | LE_C0_TXON | LE_C0_INEA);

    expect_u32("normal interrupt claimed", lance_intr(&nic), TRUE);
    expect_u32("normal completion count", nic.tx_packets, 1);
    expect_u32("normal collision count", nic.collisions, 1);
    expect_u32("normal completion no reset", nic.resets, 0);
    expect_u32("normal completion retired", nic.txb_inuse, 0);
}

static VOID test_underflow_resets(VOID)
{
    fixture_init();

    nic.txb_inuse = 1;
    nic.tx_done   = 0;
    le_put16(&nic, LE_TXD_OFF + 2, LE_T1_ERR);
    le_put16(&nic, LE_TXD_OFF + 6, LE_T3_UFLO);
    /* The real part clears TXON when it reports this descriptor. */
    mock_csr[0] = (UWORD)(LE_C0_INTR | LE_C0_TINT |
                          LE_C0_RXON | LE_C0_INEA);

    expect_u32("underflow interrupt claimed", lance_intr(&nic), TRUE);
    expect_u32("underflow error count", nic.tx_errors, 1);
    expect_u32("underflow reset", nic.resets, 1);
    expect_u32("underflow ring cleared", nic.txb_inuse, 0);
    expect_u32("underflow recovered RX", mock_csr[0] & LE_C0_RXON,
               LE_C0_RXON);
    expect_u32("underflow recovered TX", mock_csr[0] & LE_C0_TXON,
               LE_C0_TXON);
}

static VOID test_stopped_receiver_resets(VOID)
{
    fixture_init();

    /* CERR supplies an interrupt source; RXON being clear is the fault the
       descriptor ring cannot report. */
    mock_csr[0] = (UWORD)(LE_C0_INTR | LE_C0_CERR |
                          LE_C0_TXON | LE_C0_INEA);

    expect_u32("stopped RX interrupt claimed", lance_intr(&nic), TRUE);
    expect_u32("stopped RX error count", nic.rx_errors, 1);
    expect_u32("stopped RX reset", nic.resets, 1);
    expect_u32("stopped RX recovered", mock_csr[0] & LE_C0_RXON,
               LE_C0_RXON);
}

int main(void)
{
    test_normal_completion();
    test_underflow_resets();
    test_stopped_receiver_resets();

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }

    puts("all LANCE tests passed");
    return 0;
}
