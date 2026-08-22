/*
 * El3Diag: dump the live registers of a 3c589 in the A1200 PCMCIA slot.
 *
 * A diagnostic for exactly one situation: the card transmits -- frames and
 * link beats reach the switch -- and receives nothing, with every driver
 * counter at zero.  Whether that is a receive engine that was never switched
 * on, a transceiver selected onto the wrong port, or a receive pair that no
 * longer hears the switch, is written in three registers the driver does not
 * report:
 *
 *   window 0, address configuration: bits 15..14 are the active transceiver,
 *     0 twisted pair, 1 AUI, 3 coax.  Loaded from EEPROM word 6 at power-up,
 *     and this driver never writes it.
 *   window 4, media status: bit 11 is the card's OWN link-beat detect -- the
 *     inbound half of the conversation the switch's LED only shows half of.
 *   window 1, RX status: what the receive FIFO holds right now.
 *
 * Raw values are printed; the register window on this card exchanges the
 *  halves of every word, and decoding is done off-machine so the swap cannot
 * hide anything.  Each window excursion runs under Disable() and puts
 * window 1 back, so the driver's server never sees the wrong window.
 *
 * The card's registers are looked for at $A20300, where the driver put them
 * on the machine this exists for.
 */
#include "tools.h"

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("El3Diag");

#define REG(off) (*(volatile UWORD *)(0xA20300UL + (off)))
#define CMD      0x0E

static UWORD swp(UWORD v) { return (UWORD)((v >> 8) | (v << 8)); }

static UWORD peek(UWORD window, UWORD off)
{
    UWORD v;

    Disable();
    REG(CMD) = swp((UWORD)((1u << 11) | window));
    v = REG(off);
    REG(CMD) = swp((UWORD)((1u << 11) | 1));
    Enable();

    return v;
}

static UWORD cmd_word(UWORD op, UWORD arg)
{
    return (UWORD)((op << 11) | (arg & 0x07FFu));
}

static VOID poke_cmd(UWORD op, UWORD arg)
{
    Disable();
    REG(CMD) = swp(cmd_word(op, arg));
    Enable();
}

static VOID poke_w4(UWORD off, UWORD value)
{
    Disable();
    REG(CMD) = swp(cmd_word(1, 4));
    REG(off) = swp(value);
    REG(CMD) = swp(cmd_word(1, 1));
    Enable();
}

/* Watch the RX FIFO for about two seconds; the LAN's own broadcast chatter
   is the traffic source.  Returns how many samples held a complete frame. */
static ULONG rx_watch(void)
{
    ULONG hits = 0;
    UWORD i;

    for (i = 0; i < 100; i++)
    {
        UWORD st = swp(peek(1, 0x08));

        if ((st & 0x8000u) == 0)      /* a complete frame is waiting */
            hits++;
        Delay(1);
    }

    return hits;
}

int main(void)
{
    static const struct { const char *name; UWORD win; UWORD off; } probe[] = {
        { "w0 config ctrl", 0, 0x04 },
        { "w0 addr cfg   ", 0, 0x06 },
        { "w0 resource   ", 0, 0x08 },
        { "w1 rx status  ", 1, 0x08 },
        { "w1 status     ", 1, 0x0E },
        { "w2 addr 0     ", 2, 0x00 },
        { "w2 addr 2     ", 2, 0x02 },
        { "w2 addr 4     ", 2, 0x04 },
        { "w4 net diag   ", 4, 0x06 },
        { "w4 media      ", 4, 0x0A },
    };
    UWORD i;

    for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
    {
        UWORD raw = peek(probe[i].win, probe[i].off);

        Printf((STRPTR)"%s raw=$%04lx swapped=$%04lx\n",
               (STRPTR)probe[i].name, (ULONG)raw, (ULONG)swp(raw));
    }

    /* The whole register file, raw, one window per line.  Diffing two of
       these between a deaf boot and a working one finds every bit that
       matters without anyone deciding in advance which ones do. */
    {
        UWORD w, o;

        for (w = 0; w < 7; w++)
        {
            Printf((STRPTR)"w%ld:", (ULONG)w);
            for (o = 0; o < 16; o += 2)
                Printf((STRPTR)" %04lx", (ULONG)peek(w, o));
            Printf((STRPTR)"\n");
        }
    }

    /*
     * The staged experiment, for the card that hears link beat and captures
     * nothing.  Between each stage the FIFO is watched for two seconds; any
     * LAN has enough broadcast chatter to show up in that window.
     */
    Printf((STRPTR)"phase 0, as found:          %ld/100 samples saw a frame\n",
           rx_watch());

    {
        UWORD nd = swp(peek(4, 0x06));

        poke_w4(0x06, (UWORD)(nd & (UWORD)~0x000Fu));
        Printf((STRPTR)"phase 1, netdiag $%04lx->$%04lx: %ld/100 saw a frame\n",
               (ULONG)nd, (ULONG)swp(peek(4, 0x06)), rx_watch());
    }

    poke_cmd(0x10, 0x05);            /* filter individual|broadcast, again */
    poke_cmd(0x04, 0);               /* RX enable, again */
    Printf((STRPTR)"phase 2, filter+rxenable:   %ld/100 saw a frame\n",
           rx_watch());

    poke_cmd(0x10, 0x0F);            /* everything, promiscuous */
    Printf((STRPTR)"phase 3, promiscuous:       %ld/100 saw a frame\n",
           rx_watch());

    /*
     * The live-activate experiment.  The driver now sets the CONFIG_CTRL
     * activate bit in el3_init() and the register still reads without it,
     * so either something later in init clears it or the write never
     * lands.  Set it here, outside any init sequence, and read it straight
     * back; then let the statistics phase below say whether the MAC came
     * alive.  No reboot between the dump above and this write, so the
     * before and after describe the same deaf state.
     */
    {
        UWORD before = swp(peek(0, 0x04));

        Disable();
        REG(CMD) = swp(cmd_word(1, 0));
        REG(0x04) = swp((UWORD)(before | 0x0100u));
        REG(CMD) = swp(cmd_word(1, 1));
        Enable();

        Printf((STRPTR)"activate: cfg $%04lx -> wrote $%04lx -> reads $%04lx\n",
               (ULONG)before, (ULONG)(before | 0x0100u),
               (ULONG)swp(peek(0, 0x04)));
    }

    /*
     * Window 6 is the chip's own account of the MAC, kept regardless of what
     * the FIFO does with the result.  If "good frames received" moves while
     * the FIFO never holds one, the wire and the PHY are innocent and the
     * loss is inside the chip; if it stays zero under promiscuous capture on
     * a chattering LAN, nothing is being decoded off the pair at all.
     * Statistics registers clear on read, so the delta over the watch is the
     * count itself.
     */
    poke_cmd(0x10, 0x0F);            /* promiscuous for the count */
    poke_cmd(0x15, 0);               /* statistics enable */
    Disable();
    REG(CMD) = swp(cmd_word(1, 6));
    { volatile UWORD sink;
      UWORD o;
      for (o = 0; o < 10; o += 2) { sink = REG(o); (void)sink; } }
    REG(CMD) = swp(cmd_word(1, 1));
    Enable();

    Delay(100);                      /* two seconds of LAN chatter */

    {
        UWORD o;

        Disable();
        REG(CMD) = swp(cmd_word(1, 6));
        Enable();
        for (o = 0; o < 10; o += 2)
        {
            UWORD v;

            Disable();
            v = REG(o);
            Enable();
            Printf((STRPTR)"w6 stats +%ld raw=$%04lx swapped=$%04lx\n",
                   (ULONG)o, (ULONG)v, (ULONG)swp(v));
        }
        Disable();
        REG(CMD) = swp(cmd_word(1, 1));
        Enable();
    }

    poke_cmd(0x16, 0);               /* statistics off, as the driver runs */
    poke_cmd(0x10, 0x05);            /* back to normal before leaving */

    return 0;
}
