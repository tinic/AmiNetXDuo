/*
 * el3.c against a chip modelled in C.
 *
 * EL3_RAW_GET/EL3_RAW_PUT must be defined before el3.c is included whole, the
 * same way test_netdev_ed.c includes ed.c to reach its statics.  The frame
 * FIFOs come in through NetdevBusOps.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- the chip -- */

#define MOCK_WINDOWS    8
#define MOCK_REGS       7           /* offsets 0x00..0x0d, as words */
#define MOCK_FIFO       4096

static unsigned short mock_win[MOCK_WINDOWS][MOCK_REGS];
static unsigned char  mock_window;      /* which window is selected */
static unsigned short mock_status;
static int            mock_swapped;     /* the bus exchanges word halves */

/* The last command of each opcode, and how many of them were seen. */
static unsigned short mock_cmd_arg[32];
static int            mock_cmd_count[32];

/* The EEPROM, and the address a read was asked for. */
static unsigned short mock_eeprom[64];

/* The two frame FIFOs. */
static unsigned char  mock_txfifo[MOCK_FIFO];
static int            mock_txlen;
static unsigned char  mock_rxfifo[MOCK_FIFO];
static int            mock_rxlen;
static int            mock_rxpos;
static int            mock_rxread;

/*
 * The wire, which does not stop for the stack above.  MOCK_FIFO is this
 * model's receive memory and the head packet occupies it until RX_DISCARD
 * pops it, so what fits behind it is what the driver has not yet let go.
 */
#define MOCK_WIRE_FRAME 1514
static int            mock_wire_frames;
static int            mock_wire_overruns;

static void mock_wire_run(void)
{
    int used = (mock_rxlen + 3) & ~3;
    int cost = (MOCK_WIRE_FRAME + 3) & ~3;
    int i;

    for (i = 0; i < mock_wire_frames; i++)
    {
        if (MOCK_FIFO - used < cost)
            mock_wire_overruns++;
        else
            used += cost;
    }
}

/* What the driver delivered upward. */
static unsigned char  mock_rx_frame[2048];
static int            mock_rx_len;
static int            mock_rx_calls;

/* What the private direct-receive callbacks observed. */
static unsigned char  mock_direct_buf[2048];
static unsigned char  mock_claim_header[14];
static unsigned long  mock_claim_sum;
static unsigned short mock_claim_len;
static unsigned char  mock_claim_summed;
static int            mock_claim_accept;
static int            mock_claim_calls;
static int            mock_claimed_calls;

static int failures;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%lx\n", what, got);
        return;
    }

    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}

/*
 * The bus, which is where the byte order lives.
 *
 * mock_swapped is the hardware question the driver has to answer for itself:
 * with it set, a word the chip holds as 0x6d50 is delivered to the CPU as
 * 0x506d and everything the CPU writes is exchanged the same way.  Both
 * orders are run below, and the driver is told neither.
 */
static unsigned short bus_in(unsigned short v)
{
    return mock_swapped ? (unsigned short)((v >> 8) | (v << 8)) : v;
}

static unsigned short bus_out(unsigned short v)
{
    return bus_in(v);
}

static void mock_do_command(unsigned short cmd);

/* Which word register an offset is, or -1 for the command/status word. */
static int mock_index(unsigned long off)
{
    if (off == 0x0e)
        return -1;

    return (int)(off / 2);
}

static unsigned short mock_get(volatile unsigned short *p);
static void           mock_put(volatile unsigned short *p, unsigned short v);

#define EL3_RAW_GET(p)      mock_get((volatile unsigned short *)(p))
#define EL3_RAW_PUT(p, v)   mock_put((volatile unsigned short *)(p), \
                                     (unsigned short)(v))
#define EL3_SETTLE_READ(p)  ((void)(p), 0)

/* The probe record, which needs exec and is not what this test is about. */
#include <exec/types.h>
#include "netdev_cards.h"

/*
 * The shipping path drains through n68k_port_in_w_sum(), whose actual 68k
 * instructions are covered by IoSumDrill.  This test's FIFO is reactive, not
 * mapped, so route that operation through the model and test the integration
 * independently: header first, payload into the claimed destination, then one
 * completion and no staging callback.
 */
static ULONG mock_direct_drain_sum(UBYTE *dst, const volatile void *port,
                                   ULONG len)
{
    ULONG sum = 0;
    ULONG i;

    (void)port;
    for (i = 0; i < len; i++)
    {
        dst[i] = (UBYTE)(mock_rxpos < mock_rxlen ? mock_rxfifo[mock_rxpos++]
                                                  : 0);
        mock_rxread++;
    }

    for (i = 0; i < len; i += 4)
    {
        ULONG word = 0;
        ULONG n;

        for (n = 0; n < 4 && i + n < len; n++)
            word |= (ULONG)dst[i + n] << (24 - 8 * n);
        sum += word;
        if (sum < word)
            sum++;
    }

    return sum;
}

#define EL3_RX_DRAIN_SUM(dst, port, len) \
    mock_direct_drain_sum((dst), (port), (len))

VOID  netdev_diag_note(UWORD code, UWORD c, ULONG v)
{ (void)code; (void)c; (void)v; }
UWORD netdev_diag_card(const NetdevCard *c) { (void)c; return 0; }

/*
 * The machine fingerprint, for the same reason: the real one lives in
 * netdev_device.c and reads SysBase's memory list, Expansion's ConfigDevs and
 * the PCMCIA CIS, none of which a host has. el3_attach() calls it only on the
 * path where the card has no readable station address, and this returns a
 * fixed four bytes so that path is exercised deterministically rather than
 * skipped.
 *
 * It is declared in netdev_macgen.h and defined in netdev_device.c, which is
 * the reason this stub is needed at all: netdev_macgen.c, which the test does
 * link, does not carry it.
 */
UWORD netdev_mac_fingerprint(UBYTE *buf, UWORD max, ULONG salt)
{
    UWORD n = 0;

    while (n < 4 && n < max)
    {
        buf[n] = (UBYTE)((salt >> (n * 8)) & 0xFFUL);
        n++;
    }

    return n;
}

/*
 * The other three cores.  netdev_cards.c is linked whole, for the CIS-to-row
 * selection, and netdev_nic_ops_for() names every core in the table.  Linking
 * them all in would drag the DP8390 and the LANCE into a test about neither.
 * The tables are referenced by address only, so empty ones are enough.
 */
const struct NetdevNicOps netdev_nic_ne2000;
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;

#include "el3.c"

/* The board window the driver addresses.  Only its address is used: every
   access is intercepted, so nothing is ever stored here. */
static unsigned char board[0x400 + 16];
static NetdevCard    card;
static NetdevNic     nic;
static unsigned char regs[64];
static unsigned char odd[64];

static unsigned long mock_off(volatile unsigned short *p)
{
    return (unsigned long)((const volatile unsigned char *)p -
                           (const volatile unsigned char *)board) -
           card.reg_off;
}

static unsigned short mock_get(volatile unsigned short *p)
{
    unsigned long off = mock_off(p);
    int           i   = mock_index(off);

    if (i < 0)
        return bus_in((unsigned short)(mock_status |
                      ((unsigned short)mock_window << EL3_S_WINDOW_SHIFT)));

    return bus_in(mock_win[mock_window][i]);
}

static void mock_put(volatile unsigned short *p, unsigned short v)
{
    unsigned long  off = mock_off(p);
    int            i   = mock_index(off);
    unsigned short chip = bus_out(v);

    if (i < 0)
    {
        mock_do_command(chip);
        return;
    }

    /*
     * Window 1 offset 0 is the transmit FIFO, not a register: the preamble
     * words el3_tx() writes go into the same stream the body does.  The chip
     * takes a word low octet first, which is the octet the driver had to put
     * in the half that reaches the wire first.
     */
    if (mock_window == 1 && off == EL3_W1_FIFO)
    {
        if (mock_txlen + 2 <= MOCK_FIFO)
        {
            mock_txfifo[mock_txlen++] = (unsigned char)(chip & 0xff);
            mock_txfifo[mock_txlen++] = (unsigned char)(chip >> 8);
        }
        return;
    }

    mock_win[mock_window][i] = chip;

    /* The EEPROM answers a read command by clearing busy and posting data. */
    if (mock_window == 0 && off == EL3_W0_EEPROM_CMD)
    {
        if ((chip & 0x00c0) == EL3_EE_READ)
        {
            mock_win[0][EL3_W0_EEPROM_DATA / 2] =
                mock_eeprom[chip & EL3_EE_ADDR_MASK];
        }
        mock_win[0][EL3_W0_EEPROM_CMD / 2] =
            (unsigned short)(chip & (unsigned short)~EL3_EE_BUSY);
    }
}

static void mock_do_command(unsigned short cmd)
{
    unsigned short op  = (unsigned short)(cmd >> 11);
    unsigned short arg = (unsigned short)(cmd & 0x07ff);

    mock_cmd_arg[op] = arg;
    mock_cmd_count[op]++;

    switch (op)
    {
    case EL3_C_RESET:
        /* A reset selects window 0 and drops every pending cause. */
        mock_window = 0;
        mock_status = 0;
        break;

    case EL3_C_WINDOW:
        mock_window = (unsigned char)(arg & 7u);
        break;

    case EL3_C_ACK_INTR:
        /*
         * Only three causes are actually cleared by an acknowledgement.  The
         * chip is modelled saying so, because a driver that acknowledges
         * receive-complete and moves on spins forever on real hardware.
         */
        mock_status &= (unsigned short)~(arg & (EL3_S_INT_LATCH |
                                                EL3_S_TX_AVAIL |
                                                EL3_S_RX_EARLY |
                                                EL3_S_INT_REQ));
        break;

    case EL3_C_RX_DISCARD:
        mock_rxpos = 0;
        mock_rxlen = 0;
        mock_win[1][EL3_W1_RX_STATUS / 2] = EL3_RXS_INCOMPLETE;
        mock_status &= (unsigned short)~EL3_S_RX_COMPLETE;
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------- the two FIFOs -- */

static void mock_rdata(const NetdevBus *bus, UBYTE *dst, UWORD len)
{
    UWORD i;

    (void)bus;
    for (i = 0; i < len; i++)
    {
        dst[i] = (UBYTE)(mock_rxpos < mock_rxlen ? mock_rxfifo[mock_rxpos++]
                                                 : 0);
        mock_rxread++;
    }
}

static void mock_wdata(const NetdevBus *bus, const UBYTE *src, UWORD len)
{
    UWORD i;

    (void)bus;
    for (i = 0; i < len; i++)
    {
        if (mock_txlen < MOCK_FIFO)
            mock_txfifo[mock_txlen++] = src[i];
    }
}

static const struct NetdevBusOps mock_bus_ops = { mock_rdata, mock_wdata };

static void mock_reset_chip(int swapped)
{
    memset(mock_win, 0, sizeof(mock_win));
    memset(mock_cmd_arg, 0, sizeof(mock_cmd_arg));
    memset(mock_cmd_count, 0, sizeof(mock_cmd_count));
    memset(mock_txfifo, 0, sizeof(mock_txfifo));
    memset(mock_rxfifo, 0, sizeof(mock_rxfifo));
    memset(regs, 0, sizeof(regs));
    memset(odd, 0, sizeof(odd));

    mock_window = 0;
    mock_status = 0;
    mock_txlen  = 0;
    mock_rxlen  = 0;
    mock_rxpos  = 0;
    mock_rxread = 0;
    mock_wire_frames   = 0;
    mock_wire_overruns = 0;
    mock_rx_len = 0;
    mock_rx_calls = 0;
    memset(mock_direct_buf, 0, sizeof(mock_direct_buf));
    memset(mock_claim_header, 0, sizeof(mock_claim_header));
    mock_claim_sum = 0;
    mock_claim_len = 0;
    mock_claim_summed = 0;
    mock_claim_accept = 0;
    mock_claim_calls = 0;
    mock_claimed_calls = 0;
    mock_swapped = swapped;

    mock_win[0][EL3_W0_MFG_ID / 2]     = EL3_MFG_ID;
    mock_win[0][EL3_W0_CONFIG_CTRL / 2] = EL3_CC_UTP_PRESENT |
                                          EL3_CC_BNC_PRESENT;
    mock_win[0][EL3_W0_ADDR_CFG / 2]   = 0xffff;
    mock_win[1][EL3_W1_RX_STATUS / 2]  = EL3_RXS_INCOMPLETE;
    mock_win[1][EL3_W1_TX_FREE / 2]    = 2048;

    /* 3Com's own address, and a different OEM address over the top of it. */
    mock_eeprom[EL3_EE_NODE_ADDR_0 + 0] = 0x0020;
    mock_eeprom[EL3_EE_NODE_ADDR_0 + 1] = 0xaf12;
    mock_eeprom[EL3_EE_NODE_ADDR_0 + 2] = 0x3456;
    mock_eeprom[EL3_EE_OEM_ADDR_0 + 0]  = 0x00a0;
    mock_eeprom[EL3_EE_OEM_ADDR_0 + 1]  = 0x2411;
    mock_eeprom[EL3_EE_OEM_ADDR_0 + 2]  = 0x2233;
    mock_eeprom[EL3_EE_MFG_ID]           = EL3_MFG_ID;
}

static void mock_rx(APTR arg, const UBYTE *frame, UWORD len)
{
    (void)arg;
    mock_rx_calls++;
    mock_wire_run();
    mock_rx_len = (int)len;
    if (len <= sizeof(mock_rx_frame))
        memcpy(mock_rx_frame, frame, len);
}

static UBYTE *mock_rx_claim(APTR arg, const UBYTE *hdr, UWORD frame_len,
                            APTR *token)
{
    (void)arg;
    mock_claim_calls++;
    mock_claim_len = frame_len;
    memcpy(mock_claim_header, hdr, sizeof(mock_claim_header));
    if (!mock_claim_accept)
        return NULL;
    *token = (APTR)mock_direct_buf;
    return mock_direct_buf;
}

static void mock_rx_claimed(APTR arg, APTR token, ULONG sum, UBYTE summed)
{
    (void)arg;
    expect_u32("direct completion token",
               (unsigned long)(token == (APTR)mock_direct_buf), 1);
    mock_claimed_calls++;
    mock_wire_run();
    mock_claim_sum = sum;
    mock_claim_summed = summed;
}

static void nic_reset(int swapped)
{
    mock_reset_chip(swapped);
    memset(&nic, 0, sizeof(nic));

    card.name     = "3c589";
    card.manid    = 0x0101;
    card.prodid   = 0x0589;
    card.reg_off  = 0x0300;
    card.stride   = 1;
    card.chip     = NETDEV_CHIP_EL3;
    card.bus      = NETDEV_BUS_PCMCIA;
    card.base     = 0x00a20000UL;
    card.odd_off  = 0x00010000UL;
    card.regmap   = NULL;

    nic.card   = &card;
    nic.board  = (volatile UBYTE *)board;
    nic.rx     = mock_rx;
    nic.rx_arg = NULL;

    netdev_bus_setup(&nic.bus, regs, 1, NULL);
    netdev_bus_split(&nic.bus, odd);
    nic.bus.ops = &mock_bus_ops;
}

/* ----------------------------------------------------------- the tests --- */

/*
 * The byte order, which the driver is never told and has to measure.  Run
 * both ways round.  A card that reads 0x6d50 straight and a card whose window
 * exchanges the halves must both attach, and must set the flag differently.
 */
static void test_byte_order(void)
{
    int swapped;

    for (swapped = 0; swapped <= 1; swapped++)
    {
        char what[64];

        nic_reset(swapped);

        snprintf(what, sizeof(what), "attach with swapped=%d", swapped);
        expect_u32(what, (unsigned long)el3_attach(&nic), 0);

        snprintf(what, sizeof(what), "measured el3_swap, bus swapped=%d", swapped);
        expect_u32(what, (unsigned long)nic.el3_swap, (unsigned long)swapped);

        /* And the manufacturer ID really did come back as the constant. */
        snprintf(what, sizeof(what), "window 0 selected after attach, swapped=%d", swapped);
        expect_u32(what, (unsigned long)mock_window, 0);

        /*
         * The burst path's address.  netdev_bus_setup() puts it sixteen
         * strides past the register base, which is where a DP8390's data
         * port is and not where this part's FIFO is.  Attach must move it, or
         * every frame goes silently to an address nothing here decodes.
         */
        snprintf(what, sizeof(what), "fifo address, swapped=%d", swapped);
        expect_u32(what,
                   (unsigned long)((const volatile unsigned char *)nic.bus.asic -
                                   (const volatile unsigned char *)board),
                   (unsigned long)(card.reg_off + EL3_W1_FIFO));
    }

    /* A card that answers with neither order is refused, and says why. */
    nic_reset(0);
    mock_win[0][EL3_W0_MFG_ID / 2] = 0x1234;
    expect_u32("attach refuses a foreign id", (unsigned long)el3_attach(&nic),
               (unsigned long)-1);
    expect_u32("and records why", (unsigned long)nic.diag_why,
               (unsigned long)ANXDIAG_WHY_MFGID);

    /* An empty slot floats the bus, which is the case that must keep failing. */
    nic_reset(0);
    mock_win[0][EL3_W0_MFG_ID / 2] = 0xffff;
    expect_u32("attach refuses a floating bus",
               (unsigned long)el3_attach(&nic), (unsigned long)-1);
}

/*
 * The station address: three EEPROM words, the earlier octet in each high
 * half, taken from the OEM words and not 3Com's own.
 */
static void test_eeprom_address(void)
{
    static const UBYTE want[6] = { 0x00, 0xa0, 0x24, 0x11, 0x22, 0x33 };
    int i;
    int bad = 0;

    nic_reset(1);
    expect_u32("attach", (unsigned long)el3_attach(&nic), 0);

    for (i = 0; i < 6; i++)
    {
        if (nic.factory[i] != want[i])
            bad++;
    }
    expect_u32("station address octets wrong", (unsigned long)bad, 0);
    expect_u32("mac source", (unsigned long)nic.mac_source,
               (unsigned long)ANXDIAG_MAC_PROM);

    /* The media the card was built with, straight out of window 0. */
    expect_u32("media bits", (unsigned long)nic.el3_media,
               (unsigned long)(EL3_CC_UTP_PRESENT | EL3_CC_BNC_PRESENT));

    /*
     * A group bit in the EEPROM is repaired rather than programmed: a station
     * address with it set makes the part reject its own unicast frames.
     */
    nic_reset(1);
    mock_eeprom[EL3_EE_OEM_ADDR_0] = 0x01a0;
    expect_u32("attach with a group bit set",
               (unsigned long)el3_attach(&nic), 0);
    expect_u32("group bit cleared", (unsigned long)nic.factory[0], 0x00);
    expect_u32("and counted", (unsigned long)nic.mac_group_fix, 1);

    /* An OEM address that is unusable falls back to 3Com's own. */
    nic_reset(1);
    mock_eeprom[EL3_EE_OEM_ADDR_0 + 0] = 0;
    mock_eeprom[EL3_EE_OEM_ADDR_0 + 1] = 0;
    mock_eeprom[EL3_EE_OEM_ADDR_0 + 2] = 0;
    expect_u32("attach falls back", (unsigned long)el3_attach(&nic), 0);
    expect_u32("to 3Com's own word 0 high octet",
               (unsigned long)nic.factory[1], 0x20);
}

/*
 * Window bookkeeping.  Every excursion has to come back, because the receive
 * and transmit paths address window 1 without selecting it.
 */
static void test_windows(void)
{
    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);

    expect_u32("init", (unsigned long)el3_init(&nic), 0);
    expect_u32("init leaves window 1 selected", (unsigned long)mock_window, 1);
    expect_u32("and the core agrees", (unsigned long)nic.el3_win, 1);

    /* The station address really went into window 2, not wherever the
       driver happened to be. */
    expect_u32("station address word 0 in window 2",
               (unsigned long)mock_win[2][0],
               (unsigned long)(nic.mac[0] | (nic.mac[1] << 8)));
    expect_u32("station address word 2 in window 2",
               (unsigned long)mock_win[2][2],
               (unsigned long)(nic.mac[4] | (nic.mac[5] << 8)));

    /* Selecting a window already selected costs no command. */
    {
        int before = mock_cmd_count[EL3_C_WINDOW];

        el3_window(&nic, 1);
        expect_u32("re-selecting window 1 issues nothing",
                   (unsigned long)mock_cmd_count[EL3_C_WINDOW],
                   (unsigned long)before);
    }

    /* The PCMCIA resource configuration, which the part will not work
       without and which is not loaded from the EEPROM on this card. */
    expect_u32("resource config", (unsigned long)mock_win[0][EL3_W0_RESOURCE_CFG / 2],
               (unsigned long)EL3_RC_PCMCIA);
    expect_u32("rom and io base fields zeroed",
               (unsigned long)(mock_win[0][EL3_W0_ADDR_CFG / 2] &
                               EL3_AC_ROM_IO_MASK), 0);
}

/* The receive filter: four bits, and group only when something is joined. */
static void test_filter(void)
{
    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);

    expect_u32("filter with nothing joined",
               (unsigned long)mock_cmd_arg[EL3_C_SET_RX_FILTER],
               (unsigned long)(EL3_FIL_INDIVIDUAL | EL3_FIL_BROADCAST));

    netdev_mar_clear(nic.mar);
    nic.mar[3] = 0x10;
    el3_setfilter(&nic);
    expect_u32("group once something is joined",
               (unsigned long)mock_cmd_arg[EL3_C_SET_RX_FILTER],
               (unsigned long)(EL3_FIL_INDIVIDUAL | EL3_FIL_BROADCAST |
                               EL3_FIL_GROUP));

    nic.promisc = TRUE;
    el3_setfilter(&nic);
    expect_u32("promiscuous adds its bit",
               (unsigned long)mock_cmd_arg[EL3_C_SET_RX_FILTER],
               (unsigned long)(EL3_FIL_INDIVIDUAL | EL3_FIL_BROADCAST |
                               EL3_FIL_GROUP | EL3_FIL_PROMISC));
    nic.promisc = FALSE;
}

/*
 * The hash the hardware does not have.  A group address whose bit is set in
 * nic->mar[] is delivered and one whose bit is clear is dropped, through the
 * same netdev_mcaf.c the DP8390 core hands to its chip.  An opener therefore
 * sees the same frames through this card as through an Ariadne II.
 */
static void test_software_multicast(void)
{
    static const UBYTE joined[6]  = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    static const UBYTE stranger[6] = { 0x01, 0x00, 0x5e, 0x7f, 0x7f, 0x7f };
    static const UBYTE bcast[6]   = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    UBYTE frame[64];

    nic_reset(1);
    (void)el3_attach(&nic);
    netdev_mar_clear(nic.mar);
    netdev_mar_set(nic.mar, joined);

    memset(frame, 0, sizeof(frame));
    memcpy(frame, joined, 6);
    expect_u32("a joined group is wanted",
               (unsigned long)el3_rx_wanted(&nic, frame), 1);

    memcpy(frame, stranger, 6);
    expect_u32("an unjoined group is not",
               (unsigned long)el3_rx_wanted(&nic, frame), 0);

    memcpy(frame, bcast, 6);
    expect_u32("broadcast always is",
               (unsigned long)el3_rx_wanted(&nic, frame), 1);

    memcpy(frame, nic.factory, 6);
    expect_u32("unicast always is",
               (unsigned long)el3_rx_wanted(&nic, frame), 1);

    nic.promisc = TRUE;
    memcpy(frame, stranger, 6);
    expect_u32("promiscuous takes everything",
               (unsigned long)el3_rx_wanted(&nic, frame), 1);
    nic.promisc = FALSE;
}

/*
 * The transmit preamble and the padding.  The length word counts the frame
 * and not the padding, and a runt is left for the card to pad, because a pad
 * written here would fall inside the length.
 */
static void check_tx(const char *what, UWORD len, UWORD want_fifo)
{
    UBYTE frame[1600];
    UWORD i;
    char  label[96];

    for (i = 0; i < len; i++)
        frame[i] = (UBYTE)(i + 1);

    mock_txlen = 0;
    snprintf(label, sizeof(label), "%s: queued", what);
    expect_u32(label, (unsigned long)el3_tx(&nic, frame, len), 0);

    snprintf(label, sizeof(label), "%s: bytes in the fifo", what);
    expect_u32(label, (unsigned long)mock_txlen, (unsigned long)want_fifo);

    snprintf(label, sizeof(label), "%s: length word", what);
    expect_u32(label,
               (unsigned long)(mock_txfifo[0] |
                               ((unsigned long)mock_txfifo[1] << 8)),
               (unsigned long)len);

    snprintf(label, sizeof(label), "%s: second preamble word is zero", what);
    expect_u32(label,
               (unsigned long)(mock_txfifo[2] |
                               ((unsigned long)mock_txfifo[3] << 8)), 0);

    {
        int bad = 0;

        for (i = 0; i < len; i++)
        {
            if (mock_txfifo[4 + i] != (UBYTE)(i + 1))
                bad++;
        }
        snprintf(label, sizeof(label), "%s: body bytes wrong", what);
        expect_u32(label, (unsigned long)bad, 0);
    }
}

static void test_transmit(void)
{
    UBYTE frame[64];

    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);

    /* 60 is already a dword multiple: four preamble bytes and no pad. */
    check_tx("60-byte frame", 60, 64);
    /* 14 is not: two pad bytes to reach 16. */
    check_tx("14-byte frame", 14, 20);
    /* 61 needs three. */
    check_tx("61-byte frame", 61, 68);
    /* And a full-size one. */
    check_tx("1514-byte frame", 1514, 1520);

    /*
     * A FIFO with no room does not take the frame, does not corrupt it, and
     * arms the threshold that says when there will be room.  txb_inuse is
     * what tells the shell to stop pumping, and what the vertical-blank
     * watchdog then watches.
     */
    memset(frame, 0, sizeof(frame));
    mock_win[1][EL3_W1_TX_FREE / 2] = 8;
    mock_txlen = 0;
    expect_u32("a full fifo refuses",
               (unsigned long)el3_tx(&nic, frame, 60),
               (unsigned long)DP8390_TX_BUSY);
    expect_u32("and writes nothing", (unsigned long)mock_txlen, 0);
    expect_u32("and arms the threshold",
               (unsigned long)mock_cmd_arg[EL3_C_SET_TX_AVAIL], 64);
    expect_u32("and blocks the shell", (unsigned long)nic.txb_inuse, 1);

    /* Offline refuses without touching the chip at all. */
    nic.running = FALSE;
    mock_txlen  = 0;
    expect_u32("offline refuses", (unsigned long)el3_tx(&nic, frame, 60),
               (unsigned long)DP8390_TX_OFFLINE);
    expect_u32("and writes nothing", (unsigned long)mock_txlen, 0);
}

/*
 * The transmit status stack.  Read the byte, act on it, write it back, and
 * stop when it reads zero.  The write is what pops it, and a written zero is
 * what makes the next read terminate the loop in this model.
 */
static void test_tx_status(void)
{
    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);

    /* A clean completion: popped, nothing counted, no recovery. */
    odd[EL3_W1_TX_STATUS - 1] = EL3_TXS_COMPLETE;
    nic.tx_errors = 0;
    nic.tx_completed = 0;
    {
        int resets = mock_cmd_count[EL3_C_TX_RESET];

        el3_drain_tx_status(&nic);
        expect_u32("clean entry popped",
                   (unsigned long)odd[EL3_W1_TX_STATUS - 1], 0);
        expect_u32("nothing counted", (unsigned long)nic.tx_errors, 0);
        expect_u32("and no transmitter reset",
                   (unsigned long)mock_cmd_count[EL3_C_TX_RESET],
                   (unsigned long)resets);
    }
    expect_u32("a popped status is hardware progress",
               (unsigned long)nic.tx_completed, 1);

    /* An underrun counts, resets the transmitter and re-enables it. */
    odd[EL3_W1_TX_STATUS - 1] = (UBYTE)(EL3_TXS_COMPLETE | EL3_TXS_UNDERRUN);
    nic.tx_errors    = 0;
    nic.overruns     = 0;
    nic.tx_underruns = 0;
    {
        int resets  = mock_cmd_count[EL3_C_TX_RESET];
        int enables = mock_cmd_count[EL3_C_TX_ENABLE];

        el3_drain_tx_status(&nic);
        expect_u32("underrun counted", (unsigned long)nic.tx_errors, 1);
        expect_u32("as a transmit underrun",
                   (unsigned long)nic.tx_underruns, 1);
        expect_u32("and not as a receive overrun",
                   (unsigned long)nic.overruns, 0);
        expect_u32("transmitter reset",
                   (unsigned long)mock_cmd_count[EL3_C_TX_RESET],
                   (unsigned long)(resets + 1));
        expect_u32("and re-enabled",
                   (unsigned long)mock_cmd_count[EL3_C_TX_ENABLE],
                   (unsigned long)(enables + 1));
    }

    /* Maximum collisions re-enables without a reset. */
    odd[EL3_W1_TX_STATUS - 1] =
        (UBYTE)(EL3_TXS_COMPLETE | EL3_TXS_MAX_COLLISION);
    nic.collisions = 0;
    {
        int resets  = mock_cmd_count[EL3_C_TX_RESET];
        int enables = mock_cmd_count[EL3_C_TX_ENABLE];

        el3_drain_tx_status(&nic);
        expect_u32("collision counted", (unsigned long)nic.collisions, 1);
        expect_u32("no reset",
                   (unsigned long)mock_cmd_count[EL3_C_TX_RESET],
                   (unsigned long)resets);
        expect_u32("but re-enabled",
                   (unsigned long)mock_cmd_count[EL3_C_TX_ENABLE],
                   (unsigned long)(enables + 1));
    }
}

/* Receive status decode, and the discard that must follow every frame. */
static void test_receive(void)
{
    UWORD i;

    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);
    netdev_mar_clear(nic.mar);

    /* A good unicast frame, delivered whole. */
    mock_rxlen = 74;
    mock_rxpos = 0;
    memcpy(mock_rxfifo, nic.factory, 6);
    for (i = 6; i < 74; i++)
        mock_rxfifo[i] = (UBYTE)i;
    mock_win[1][EL3_W1_RX_STATUS / 2] = 74;

    {
        int discards = mock_cmd_count[EL3_C_RX_DISCARD];

        expect_u32("a complete frame is taken",
                   (unsigned long)el3_rint(&nic), 1);
        expect_u32("delivered once", (unsigned long)mock_rx_calls, 1);
        expect_u32("with the right length", (unsigned long)mock_rx_len, 74);
        expect_u32("and counted", (unsigned long)nic.rx_packets, 1);
        expect_u32("and discarded",
                   (unsigned long)mock_cmd_count[EL3_C_RX_DISCARD],
                   (unsigned long)(discards + 1));
    }

    /* An incomplete frame is not touched and not discarded. */
    mock_win[1][EL3_W1_RX_STATUS / 2] = (UWORD)(EL3_RXS_INCOMPLETE | 20);
    mock_rx_calls = 0;
    {
        int discards = mock_cmd_count[EL3_C_RX_DISCARD];

        expect_u32("an incomplete frame is left alone",
                   (unsigned long)el3_rint(&nic), 0);
        expect_u32("nothing delivered", (unsigned long)mock_rx_calls, 0);
        expect_u32("nothing discarded",
                   (unsigned long)mock_cmd_count[EL3_C_RX_DISCARD],
                   (unsigned long)discards);
    }

    /*
     * An errored frame is still discarded.  Discard is the only thing that
     * advances the FIFO past a packet and its padding, so a receive path that
     * returned early on an error would stop receiving entirely.
     */
    mock_win[1][EL3_W1_RX_STATUS / 2] =
        (UWORD)(EL3_RXS_ERROR | (EL3_RXE_CRC << EL3_RXS_ERR_SHIFT) | 74);
    mock_rx_calls = 0;
    nic.rx_errors = 0;
    {
        int discards = mock_cmd_count[EL3_C_RX_DISCARD];

        expect_u32("an errored frame is handled",
                   (unsigned long)el3_rint(&nic), 1);
        expect_u32("not delivered", (unsigned long)mock_rx_calls, 0);
        expect_u32("counted", (unsigned long)nic.rx_errors, 1);
        expect_u32("and still discarded",
                   (unsigned long)mock_cmd_count[EL3_C_RX_DISCARD],
                   (unsigned long)(discards + 1));
    }

    /* An overrun is counted as one as well as an error. */
    mock_win[1][EL3_W1_RX_STATUS / 2] =
        (UWORD)(EL3_RXS_ERROR | (EL3_RXE_OVERRUN << EL3_RXS_ERR_SHIFT) | 74);
    nic.overruns = 0;
    (void)el3_rint(&nic);
    expect_u32("overrun counted", (unsigned long)nic.overruns, 1);

    /* A group frame nothing joined is read out and dropped, not delivered. */
    mock_rxlen = 64;
    mock_rxpos = 0;
    memset(mock_rxfifo, 0, sizeof(mock_rxfifo));
    mock_rxfifo[0] = 0x01;
    mock_rxfifo[1] = 0x00;
    mock_rxfifo[2] = 0x5e;
    mock_rxfifo[3] = 0x33;
    mock_win[1][EL3_W1_RX_STATUS / 2] = 64;
    mock_rx_calls = 0;
    {
        int discards = mock_cmd_count[EL3_C_RX_DISCARD];

        expect_u32("an unjoined group frame is handled",
                   (unsigned long)el3_rint(&nic), 1);
        expect_u32("but not delivered", (unsigned long)mock_rx_calls, 0);
        expect_u32("and still discarded",
                   (unsigned long)mock_cmd_count[EL3_C_RX_DISCARD],
                   (unsigned long)(discards + 1));
    }
}

/*
 * The second-copy removal itself.  IoSumDrill proves the assembler; this proves
 * el3_rint() feeds it only the payload and completes the claimed request
 * instead of also handing the staging buffer up the old path.
 */
static void test_receive_direct(void)
{
    UWORD i;

    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);
    netdev_mar_clear(nic.mar);
    nic.rx_claim = mock_rx_claim;
    nic.rx_claimed = mock_rx_claimed;

    mock_rxlen = 77;                    /* payload 63: three-byte tail */
    mock_rxpos = 0;
    memcpy(mock_rxfifo, nic.factory, 6);
    for (i = 6; i < (UWORD)mock_rxlen; i++)
        mock_rxfifo[i] = (UBYTE)(i ^ 0x5a);
    mock_win[1][EL3_W1_RX_STATUS / 2] = (UWORD)mock_rxlen;
    mock_claim_accept = 1;

    expect_u32("a direct frame is taken", (unsigned long)el3_rint(&nic), 1);
    expect_u32("direct claim called once", (unsigned long)mock_claim_calls, 1);
    expect_u32("claim saw the whole frame length",
               (unsigned long)mock_claim_len, 77);
    expect_u32("claim saw the frame header",
               (unsigned long)(memcmp(mock_claim_header, mock_rxfifo, 14) == 0),
               1);
    expect_u32("direct completion called once",
               (unsigned long)mock_claimed_calls, 1);
    expect_u32("direct completion carries a sum",
               (unsigned long)(mock_claim_summed != 0 && mock_claim_sum != 0),
               1);
    expect_u32("old receive path was not called",
               (unsigned long)mock_rx_calls, 0);
    expect_u32("only one frame was counted",
               (unsigned long)nic.rx_packets, 1);
    expect_u32("the FIFO was drained exactly once",
               (unsigned long)mock_rxread, 77);
    expect_u32("the payload landed without its Ethernet header",
               (unsigned long)(memcmp(mock_direct_buf, mock_rxfifo + 14,
                                     63) == 0), 1);

    /* A declined claim must be byte-for-byte the old staging path. */
    mock_rxpos = 0;
    mock_rxread = 0;
    mock_rxlen = 77;
    mock_rx_calls = 0;
    mock_claim_calls = 0;
    mock_claimed_calls = 0;
    mock_claim_accept = 0;
    mock_win[1][EL3_W1_RX_STATUS / 2] = (UWORD)mock_rxlen;

    expect_u32("a declined direct frame is still taken",
               (unsigned long)el3_rint(&nic), 1);
    expect_u32("declined claim called once",
               (unsigned long)mock_claim_calls, 1);
    expect_u32("declined claim was not completed",
               (unsigned long)mock_claimed_calls, 0);
    expect_u32("declined frame used the old receive path",
               (unsigned long)mock_rx_calls, 1);
    expect_u32("declined frame kept its length",
               (unsigned long)mock_rx_len, 77);
    expect_u32("declined frame stayed intact",
               (unsigned long)(memcmp(mock_rx_frame, mock_rxfifo,
                                     77) == 0), 1);
}

/*
 * The interrupt drain.  What matters here is what an acknowledgement does
 * not clear: receive-complete and transmit-complete are cleared by servicing
 * the FIFO and the status stack, so a drain that acknowledged them and left
 * would hold INT2 down forever.
 */
static void test_interrupt(void)
{
    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);
    netdev_mar_clear(nic.mar);

    /*
     * The two masks run the opposite way to their names.  A clear bit in the
     * read-zero mask forces that status bit to read as zero, so all ones
     * reports everything.  A zero there blinds the handler to every cause it
     * services, and the card goes quiet with no error anywhere.
     */
    expect_u32("read-zero mask reports everything",
               (unsigned long)mock_cmd_arg[EL3_C_SET_ZERO_MASK], 0xff);
    expect_u32("interrupt mask asks for what is serviced",
               (unsigned long)mock_cmd_arg[EL3_C_SET_INTR_MASK],
               (unsigned long)(EL3_S_ADAPTER_FAIL | EL3_S_TX_COMPLETE |
                               EL3_S_TX_AVAIL | EL3_S_RX_COMPLETE));

    /* Nothing pending is not ours: INT2 is shared. */
    mock_status = 0;
    expect_u32("an idle chip disclaims the interrupt",
               (unsigned long)el3_intr(&nic), 0);

    /* A receive drains the FIFO and the cause goes with the discard. */
    mock_rxlen = 74;
    mock_rxpos = 0;
    memcpy(mock_rxfifo, nic.factory, 6);
    mock_win[1][EL3_W1_RX_STATUS / 2] = 74;
    mock_status   = EL3_S_INT_LATCH | EL3_S_RX_COMPLETE;
    mock_rx_calls = 0;

    expect_u32("a receive interrupt is ours",
               (unsigned long)el3_intr(&nic), 1);
    expect_u32("the frame came up", (unsigned long)mock_rx_calls, 1);
    expect_u32("and the latch was dropped",
               (unsigned long)(mock_status & EL3_S_INT_LATCH), 0);

    /* Transmit-available unblocks the shell and is cleared by its ack. */
    nic.txb_inuse = 1;
    mock_status   = EL3_S_INT_LATCH | EL3_S_TX_AVAIL;
    expect_u32("a transmit-available interrupt is ours",
               (unsigned long)el3_intr(&nic), 1);
    expect_u32("the shell is unblocked", (unsigned long)nic.txb_inuse, 0);
    expect_u32("and the cause is gone",
               (unsigned long)(mock_status & EL3_S_TX_AVAIL), 0);

    /* Transmit-complete pops the stack rather than acknowledging. */
    odd[EL3_W1_TX_STATUS - 1] = EL3_TXS_COMPLETE;
    mock_status = EL3_S_INT_LATCH | EL3_S_TX_COMPLETE;
    mock_win[1][EL3_W1_TX_FREE / 2] = 2048;
    expect_u32("a transmit-complete interrupt is ours",
               (unsigned long)el3_intr(&nic), 1);
    expect_u32("the stack was popped",
               (unsigned long)odd[EL3_W1_TX_STATUS - 1], 0);

    /* An adapter failure restarts the part and counts a reset. */
    nic.resets  = 0;
    mock_status = EL3_S_INT_LATCH | EL3_S_ADAPTER_FAIL;
    expect_u32("an adapter failure is ours",
               (unsigned long)el3_intr(&nic), 1);
    expect_u32("and is recovered by a restart",
               (unsigned long)nic.resets, 1);

    /* A stopped unit disclaims everything: the server can still be armed. */
    nic.running = FALSE;
    mock_status = EL3_S_INT_LATCH | EL3_S_RX_COMPLETE;
    expect_u32("a stopped unit disclaims",
               (unsigned long)el3_intr(&nic), 0);
}

/* The watchdog's entry point, which the shell reaches through ops->reset. */
static void test_reset_and_ops(void)
{
    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);

    nic.txb_inuse = 1;
    nic.resets    = 0;
    netdev_nic_el3.reset(&nic);
    expect_u32("the watchdog reset clears the block",
               (unsigned long)nic.txb_inuse, 0);
    expect_u32("counts itself", (unsigned long)nic.resets, 1);
    expect_u32("and leaves the chip running",
               (unsigned long)nic.running, 1);

    expect_u32("the ops table carries a reset",
               (unsigned long)(netdev_nic_el3.reset != NULL), 1);
    expect_u32("and is the one the card table hands out",
               (unsigned long)(netdev_nic_ops_for(NETDEV_CHIP_EL3) ==
                               &netdev_nic_el3), 1);

    /* halt() leaves nothing that could hold a level-driven INT2 down. */
    el3_halt(&nic);
    expect_u32("halt stops the unit", (unsigned long)nic.running, 0);
    expect_u32("and masks every cause",
               (unsigned long)mock_cmd_arg[EL3_C_SET_INTR_MASK], 0);
}

/* The CIS identity that picks this row out of the table. */
/*
 * How long the head packet is held.  Two 1514-byte frames arrive while the
 * upstack call runs: they fit only if the head has already been discarded.
 */
static void test_receive_fifo_hold(void)
{
    UWORD i;

    nic_reset(1);
    (void)el3_attach(&nic);
    memcpy(nic.mac, nic.factory, 6);
    (void)el3_init(&nic);
    netdev_mar_clear(nic.mar);

    mock_rxlen = MOCK_WIRE_FRAME;
    mock_rxpos = 0;
    memcpy(mock_rxfifo, nic.factory, 6);
    for (i = 6; i < (UWORD)mock_rxlen; i++)
        mock_rxfifo[i] = (UBYTE)i;
    mock_win[1][EL3_W1_RX_STATUS / 2] = (UWORD)mock_rxlen;
    mock_wire_frames   = 2;
    mock_wire_overruns = 0;

    expect_u32("the staged frame is taken",
               (unsigned long)el3_rint(&nic), 1);
    expect_u32("the head was popped before the stack ran",
               (unsigned long)mock_wire_overruns, 0);

    /* The claimed path drains into the stack's own packet and must match. */
    nic.rx_claim   = mock_rx_claim;
    nic.rx_claimed = mock_rx_claimed;
    mock_claim_accept = 1;
    mock_rxlen = MOCK_WIRE_FRAME;
    mock_rxpos = 0;
    mock_win[1][EL3_W1_RX_STATUS / 2] = (UWORD)mock_rxlen;
    mock_wire_frames   = 2;
    mock_wire_overruns = 0;

    expect_u32("the claimed frame is taken",
               (unsigned long)el3_rint(&nic), 1);
    expect_u32("and its head was popped first",
               (unsigned long)mock_wire_overruns, 0);
}

static void test_card_selection(void)
{
    const NetdevCard *c = netdev_card_by_cis(0x0101, 0x0589);

    expect_u32("a 3C589 CIS finds a row", (unsigned long)(c != NULL), 1);
    if (c != NULL)
    {
        expect_u32("and it is the EtherLink III row",
                   (unsigned long)c->chip, (unsigned long)NETDEV_CHIP_EL3);
        expect_u32("on the PCMCIA slot",
                   (unsigned long)c->bus, (unsigned long)NETDEV_BUS_PCMCIA);
    }

    c = netdev_card_by_cis(0x1234, 0x5678);
    expect_u32("an unknown CIS falls back", (unsigned long)(c != NULL), 1);
    if (c != NULL)
    {
        expect_u32("to the NE2000 catch-all",
                   (unsigned long)c->chip, (unsigned long)NETDEV_CHIP_NE2000);
    }
}

int main(void)
{
    test_byte_order();
    test_eeprom_address();
    test_windows();
    test_filter();
    test_software_multicast();
    test_transmit();
    test_tx_status();
    test_receive();
    test_receive_direct();
    test_interrupt();
    test_reset_and_ops();
    test_receive_fifo_hold();
    test_card_selection();

    if (failures != 0)
    {
        printf("netdev_el3: %d failure(s)\n", failures);
        return 1;
    }

    printf("netdev_el3: all ok\n");
    return 0;
}
