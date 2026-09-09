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

/*
 * What CSR0 answers when the driver has just written STOP to it.  Zero is a
 * chip behaving; anything else is returned instead, which is how a bus that
 * answers with garbage or a slot with no LANCE in it is modelled -- the write
 * below always stores LE_C0_STOP, so a staged value alone cannot survive it.
 */
static UWORD mock_csr0_stuck;

static UWORD mock_csr_get(NetdevNic *unused, UWORD csr)
{
    (VOID)unused;
    if (csr == LE_CSR0 && mock_csr0_stuck != 0)
        return mock_csr0_stuck;
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

static VOID test_filter_waits_for_transmit(VOID)
{
    fixture_init();

    nic.txb_inuse = 1;
    nic.tx_done   = 0;
    le_put16(&nic, LE_TXD_OFF + 2, LE_T1_OWN | LE_T1_STP | LE_T1_ENP);
    nic.mar[0] = 0xa5;

    lance_setfilter(&nic);

    expect_u32("filter update is deferred", nic.filter_pending, TRUE);
    expect_u32("owned transmit survives filter update", nic.txb_inuse, 1);
    expect_u32("old filter remains active while TX runs",
               le_get16(&nic, LE_INIT_OFF + 8), 0);

    le_put16(&nic, LE_TXD_OFF + 2, 0);
    mock_csr[0] = (UWORD)(LE_C0_INTR | LE_C0_TINT |
                          LE_C0_RXON | LE_C0_TXON | LE_C0_INEA);

    expect_u32("completion interrupt claimed", lance_intr(&nic), TRUE);
    expect_u32("transmit completed before restart", nic.tx_completed, 1);
    expect_u32("filter update was applied", nic.filter_pending, FALSE);
    expect_u32("new logical filter is in the init block",
               le_get16(&nic, LE_INIT_OFF + 8), 0x00a5);
    expect_u32("receiver restarted after filter update",
               mock_csr[0] & LE_C0_RXON, LE_C0_RXON);
    expect_u32("transmitter restarted after filter update",
               mock_csr[0] & LE_C0_TXON, LE_C0_TXON);
}

/* ------------------------------------------------------------- attach ---- */

/*
 * lance_attach() decides three things and NOTHING drove it: this file calls
 * lance_intr, lance_setfilter, lance_init and lance_halt, and never attach.
 *
 * The station address is the one worth pinning.  Neither the A2065 nor the
 * Ariadne has an address PROM -- a read below the SRAM returns zero -- so the
 * address is SYNTHESISED: the OUI comes from the card row and the low four
 * bytes from the autoconfig serial number.  Get it wrong and every card of a
 * kind can end up with the same address, or with one whose group bit is set,
 * and a card sweep still passes because both ends agree on whatever was
 * produced.  It is the kind of defect a passing sweep cannot see.
 */
static void attach_reset(const NetdevCard *c, ULONG serial, UWORD stuck)
{
    memset(&nic, 0, sizeof(nic));
    memset(mock_csr, 0, sizeof(mock_csr));
    nic.card    = c;
    nic.serial  = serial;
    nic.board   = mock_board.bytes;
    mock_csr0_stuck = stuck;    /* 0 = a chip that answers properly */
}

static void test_attach_address_comes_from_the_serial(void)
{
    static NetdevCard a2065;
    static NetdevCard ariadne;

    printf("\n-- attach: the address is synthesised, not read\n");

    a2065          = card;
    a2065.mem_size = LE_END + 16;
    a2065.serial_oui = 0x0080u;          /* 00:80:10, Commodore */

    ariadne          = card;
    ariadne.mem_size = LE_END + 16;
    ariadne.serial_oui = 0x0060u;        /* 00:60:30, Village Tronic */

    attach_reset(&a2065, 0x10ABCDEFUL, 0);
    expect_u32("a2065 attaches", (ULONG)(LONG)lance_attach(&nic), 0);
    expect_u32("factory[0] is the row's OUI, high", nic.factory[0], 0x00);
    expect_u32("factory[1] is the row's OUI, low",  nic.factory[1], 0x80);
    expect_u32("factory[2] is serial 31..24", nic.factory[2], 0x10);
    expect_u32("factory[3] is serial 23..16", nic.factory[3], 0xAB);
    expect_u32("factory[4] is serial 15..8",  nic.factory[4], 0xCD);
    expect_u32("factory[5] is serial 7..0",   nic.factory[5], 0xEF);
    expect_u32("and the group bit is clear -- a station address, not a group",
               (ULONG)(nic.factory[0] & 1u), 0);
    expect_u32("mac[] is the factory address", (ULONG)memcmp(nic.mac,
               nic.factory, NETDEV_ADDR_LEN), 0);

    /*
     * THE SAME SERIAL ON THE OTHER ROW MUST GIVE A DIFFERENT ADDRESS.  The
     * OUI is the only thing separating them, so an attach that ignored the
     * row would give an Ariadne a Commodore address -- and the sweep would
     * not notice.
     */
    attach_reset(&ariadne, 0x10ABCDEFUL, 0);
    expect_u32("ariadne attaches", (ULONG)(LONG)lance_attach(&nic), 0);
    expect_u32("and takes ITS row's OUI", nic.factory[1], 0x60);
    expect_u32("with the same serial underneath", nic.factory[5], 0xEF);
}

/* The chip-presence check, and the refusal that protects it. */
static void test_attach_refuses_what_is_not_a_lance(void)
{
    static NetdevCard c;

    printf("\n-- attach: what is refused\n");

    c          = card;
    c.mem_size = LE_END + 16;
    c.serial_oui = 0x0080u;

    /* CSR0 answering with anything in its high byte is not a LANCE. */
    attach_reset(&c, 1, (UWORD)(LE_C0_STOP | 0x0100u));  /* garbage in the high byte */
    expect_u32("a high byte in CSR0 is refused",
               (ULONG)(LONG)lance_attach(&nic), (ULONG)(LONG)-1);
    expect_u32("and the reason is recorded for CheckNetDevice",
               nic.diag_why, (ULONG)ANXDIAG_WHY_CSR);

    /* STOP not set after a stop was written: the chip is not answering. */
    attach_reset(&c, 1, 0x0001u);   /* answers, but STOP is not set */
    expect_u32("CSR0 without STOP is refused",
               (ULONG)(LONG)lance_attach(&nic), (ULONG)(LONG)-1);

    /* A board too small for the rings, buffers and init block. */
    {
        static NetdevCard small;

        small          = card;
        small.mem_size = LE_END - 1;
        small.serial_oui = 0x0080u;
        attach_reset(&small, 1, 0);
        expect_u32("a board smaller than the ring geometry is refused",
                   (ULONG)(LONG)lance_attach(&nic), (ULONG)(LONG)-1);
    }

    mock_csr0_stuck = 0;
}

int main(void)
{
    test_normal_completion();
    test_underflow_resets();
    test_stopped_receiver_resets();
    test_filter_waits_for_transmit();
    test_attach_address_comes_from_the_serial();
    test_attach_refuses_what_is_not_a_lance();

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }

    puts("all LANCE tests passed");
    return 0;
}
