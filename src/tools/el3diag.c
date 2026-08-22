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
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

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

    return 0;
}
