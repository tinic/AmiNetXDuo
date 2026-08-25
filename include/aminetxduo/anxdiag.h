/*
 * anxnet.device: what the probe did, published where a command can read it.
 *
 * THE FAULT THIS EXISTS FOR.  A card that does not attach produces no unit, so
 * there is nothing to OpenDevice() and nothing to ask.  Everything the driver
 * knows about why -- which window it wrote, what the chip answered, which CIS
 * tuples were there -- happened inside the device's romtag init and was
 * reachable only by rebuilding with NETDEV_TRACE and attaching a serial cable.
 * That is not a thing a user can be asked to do, so the questions the audit
 * could not settle stayed open.
 *
 * So the probe records itself into this structure whatever the outcome, and
 * the structure is published under a public semaphore name.  CheckNetDevice
 * finds it and prints it.  Nothing about that path needs a unit, an open
 * device, a running stack, or a filesystem.
 *
 * WHY A SEMAPHORE AND NOT A MESSAGE PORT.  This is a snapshot a reader copies,
 * not a conversation: there is no request the tool could make that the device
 * could answer better than "here is what happened".  A public MsgPort would
 * buy request/response and cost a message loop inside a driver that has no
 * task of its own -- the probe runs in the romtag init, on whatever process
 * called OpenDevice(), and there is nobody left afterwards to serve a port.
 * The semaphore is also what this tree already does for the same shape of
 * problem: AmiHealthMark in aminetxduo/health.h is published exactly this way,
 * with the SignalSemaphore first so FindSemaphore() returns the record.
 *
 * NOBODY OBTAINS THE SEMAPHORE.  It is a name to be found, and the reader
 * copies the whole record under Forbid().  The device removes it under
 * Forbid() in its expunge, before the memory holding it can go away, so a
 * reader that holds Forbid() across find and copy cannot be reading a freed
 * one.  Blocking on the semaphore would mean waiting on a driver that may be
 * the thing that is broken.
 *
 * WHY THE RECORD IS CODES AND THE PROSE IS IN THE TOOL.  The device is
 * resident and the strings are long -- a whole sentence per step, because a
 * user has to be able to read the output without a table.  Putting them in the
 * driver would keep every one of them in memory forever to be printed at most
 * once.  The user never sees a code: CheckNetDevice turns each one into the
 * sentence.  ad_Version and ad_Size are what stop a tool and a device that
 * disagree about this file from misreading each other, since once both are in
 * C: and DEVS: they are upgraded separately.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ANXDIAG_H
#define AMINETXDUO_ANXDIAG_H

#include <exec/types.h>
#include <exec/semaphores.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough that no other publisher can hit it, and it names the driver. */
#define ANXDIAG_NAME        "anxnet.device.Probe"
#define ANXDIAG_MAGIC       0x414E5844UL      /* 'ANXD' */
#define ANXDIAG_VERSION     1

/*
 * How many steps are kept.  The probe walks every ConfigDev on the bus and
 * then every fixed/PCMCIA row, and the PCMCIA row alone contributes about
 * fifteen; 64 covers a machine with four boards and the slot with room to
 * spare, and the recorder counts what it had to drop rather than wrapping.
 */
#define ANXDIAG_STEPS       64

/* ad_Card for a step that is about the machine rather than one card. */
#define ANXDIAG_NOCARD      0xffffu

/*
 * The steps.  Numbers are a wire format between two binaries and are never
 * reused or renumbered; a value CheckNetDevice does not know is printed as
 * itself rather than dropped, so an older tool against a newer device still
 * says something true.
 *
 * The comment on each is what the tool prints, in short.
 */

/* --- the machine ------------------------------------------------------- */
#define ANXDIAG_START           1   /* probe began; value = card rows known  */
#define ANXDIAG_EXPANSION       2   /* expansion.library base, 0 = not open  */
#define ANXDIAG_BOARDS          3   /* ConfigDevs walked; value = how many   */
#define ANXDIAG_NOMATCH         4   /* a board matching no row; value = man
                                       << 16 | product                       */
#define ANXDIAG_UNITS_FULL      5   /* a supported board past NETDEV_MAX_UNITS */
#define ANXDIAG_DONE            6   /* probe ended; value = units created    */

/* --- one card ---------------------------------------------------------- */
#define ANXDIAG_ZORRO_FOUND    10   /* autoconfig match; value = board addr  */
#define ANXDIAG_FIXED_TRY      11   /* a fixed-address row; value = addr     */
#define ANXDIAG_NO_CORE        12   /* no chip core compiled for this row    */
#define ANXDIAG_ATTACH_OK      13   /* attached; value = transfer mode       */
#define ANXDIAG_ATTACH_FAIL    14   /* attach refused; value = ANXDIAG_WHY_* */
#define ANXDIAG_MAC_HI         15   /* station address, octets 0..1          */
#define ANXDIAG_MAC_LO         16   /* station address, octets 2..5          */
#define ANXDIAG_MAC_SOURCE     17   /* value = ANXDIAG_MAC_*                 */
#define ANXDIAG_GETODD         18   /* value 1 = the CNet16 word-read path is
                                       on for this card                      */
#define ANXDIAG_UNIT           19   /* became a unit; value = unit number    */
#define ANXDIAG_CR_READ        20   /* command register readback during
                                       detection; value = the byte            */
#define ANXDIAG_ODD_RETRY      21   /* odd registers failed a byte read and
                                       the CNet16 word path was tried         */
#define ANXDIAG_CHIP           22   /* value = NETDEV_CHIP_*.  Recorded before
                                       attach, so a card that never attaches
                                       still says what it was taken for -- and
                                       so the reader does not claim a transfer
                                       mode for a LANCE, which has no data
                                       port to have one                       */
#define ANXDIAG_EL3_MFG        23   /* EtherLink III: the window 0 manufacturer
                                       ID exactly as read, before any swap    */
#define ANXDIAG_EL3_ORDER      24   /* EtherLink III: 0 = the register window
                                       delivers words straight, 1 = its halves
                                       are exchanged and the core swaps them  */
#define ANXDIAG_EL3_MEDIA      25   /* EtherLink III: window 0 config control,
                                       whose high half says which media the
                                       card was built with                    */
#define ANXDIAG_ODD_PLAIN      26   /* odd-register probe read as bytes:
                                       isr << 16 | bnry(5a) << 8 | bnry(a5)   */
#define ANXDIAG_ODD_WORD       27   /* the same three through the word path   */
#define ANXDIAG_ODDWIN         28   /* the odd-register window, or 0 when the
                                       register file is contiguous            */
#define ANXDIAG_BUF_SEEN       29   /* a packet-buffer readback mismatch:
                                       offset << 16 | wrote << 8 | read       */

/* --- the PCMCIA slot --------------------------------------------------- */
#define ANXDIAG_PC_RESOURCE    30   /* card.resource base, 0 = no slot       */
#define ANXDIAG_PC_OWN         31   /* OwnCard() answer, 0 = we have it      */
#define ANXDIAG_PC_FUNCID      32   /* CISTPL_FUNCID, or ANXDIAG_ABSENT      */
#define ANXDIAG_PC_NOTLAN      33   /* FUNCID present and not 6              */
#define ANXDIAG_PC_MANFID      34   /* man << 16 | product, or ANXDIAG_ABSENT */
#define ANXDIAG_PC_FUNCE       35   /* first FUNCE subtuple, or ANXDIAG_ABSENT */
#define ANXDIAG_PC_NODEID      36   /* a LAN node ID was found in the CIS    */
#define ANXDIAG_PC_NOCONFIG    37   /* no CISTPL_CONFIG: cannot be configured */
#define ANXDIAG_PC_CFGBASE     38   /* TPCC_RADR from CISTPL_CONFIG          */
#define ANXDIAG_PC_NOCFTABLE   39   /* no CISTPL_CFTABLE_ENTRY               */
#define ANXDIAG_PC_INDEX       40   /* configuration index written to the COR */
#define ANXDIAG_PC_IOMODE      41   /* CardMiscControl bits for I/O mode     */
#define ANXDIAG_PC_COR         42   /* COR address written; value = address  */
#define ANXDIAG_PC_CR          43   /* CR readback after it; value = byte    */
#define ANXDIAG_PC_COR2        44   /* the doubled COR address, as a retry   */
#define ANXDIAG_PC_CR2         45   /* CR readback after the retry           */
#define ANXDIAG_PC_SILENT      46   /* no DP8390 answered either way         */
#define ANXDIAG_PC_IRQMODE     47   /* status-change IRQ enabled; value = the
                                       card.resource version                 */
#define ANXDIAG_PC_IRQSKIP     48   /* pre-V39 resource, bits not meaningful */
#define ANXDIAG_PC_CLAIMED     49   /* slot claimed; value = register base   */
#define ANXDIAG_PC_CARD        50   /* which row the CIS picked; value = the
                                       netdev_cards[] index.  The steps above
                                       this one are recorded against the
                                       MACHINE and not a card, because until
                                       this step there is no card yet         */
#define ANXDIAG_PC_CFTABLE     51   /* the first configuration table entry's
                                       leading body bytes, 2 << 24 | 3 << 16 |
                                       4 << 8 | 5 -- the I/O descriptor this
                                       driver does not parse, so that a report
                                       can say what it would have said        */
#define ANXDIAG_PC_MISC        52   /* CardMiscControl()'s answer to the
                                       I/O-mode call; a bit cleared in it is a
                                       bit this machine does not support      */
#define ANXDIAG_PC_CORVAL      53   /* the byte written to the COR            */
#define ANXDIAG_PC_SETTLE      54   /* 2 ms rounds waited after the COR write
                                       before a chip answered; 0 = at once    */
#define ANXDIAG_PC_NOROW       55   /* the CIS named a card no row drives, and
                                       there is no fallback row either        */
#define ANXDIAG_PC_RESET       68   /* CardResetCard() at claim, 1 = pulsed   */
#define ANXDIAG_CLOCK          69   /* iterations of a bare spin per raster
                                       line, measured against the beam once at
                                       claim; 0 = no readable beam, so every
                                       wait in the driver is a counted loop
                                       again.  Tens on a 14 MHz 68020, tens of
                                       thousands behind an accelerator, and
                                       that ratio is why a counted delay is
                                       not a delay -- see netdev_clock.h      */

/* --- the ISA Plug and Play bridge, netdev_isapnp.c --------------------- */
#define ANXDIAG_PNP_VENDOR     60   /* isolation serial identifier bytes 0..3,
                                       the vendor and device ID              */
#define ANXDIAG_PNP_SERIAL     61   /* bytes 4..7, the card's serial number  */
#define ANXDIAG_PNP_CSUM       62   /* byte 8 << 8 | the checksum computed
                                       over the eight before it              */
#define ANXDIAG_PNP_IO         63   /* the ISA port the logical device was
                                       told to decode at                     */
#define ANXDIAG_PNP_SETTLE     64   /* 2 ms rounds waited after Activate     */
#define ANXDIAG_PNP_CR         65   /* CR readback after it; value = byte    */
#define ANXDIAG_PNP_SILENT     66   /* nothing answered; value = the register
                                       base it was configured for            */
#define ANXDIAG_PNP_OK         67   /* configured; value = the register base */

/* A tuple or a value that was not there at all, rather than one that was
   there and zero.  Chosen so it cannot be a real MANFID or subtuple. */
#define ANXDIAG_ABSENT      0xfffffffeUL

/* ANXDIAG_ATTACH_FAIL values. */
#define ANXDIAG_WHY_UNKNOWN     0   /* the core refused and said no more     */
#define ANXDIAG_WHY_CR          1   /* command register readback wrong       */
#define ANXDIAG_WHY_ODD         2   /* odd registers unreadable either way   */
#define ANXDIAG_WHY_BUFFER      3   /* the 16 KB buffer did not read back    */
#define ANXDIAG_WHY_ADDRESS     4   /* no usable station address anywhere    */
#define ANXDIAG_WHY_REGS        5   /* ED core: register file did not answer */
#define ANXDIAG_WHY_CSR         6   /* LANCE: CSR0 did not answer            */
#define ANXDIAG_WHY_MFGID       7   /* EL3: the manufacturer ID read back as
                                       neither byte order                     */
#define ANXDIAG_WHY_EEPROM      8   /* EL3: the EEPROM never went ready       */
#define ANXDIAG_WHY_ODD_BNRY    9   /* the odd registers answered the ISR read
                                       and would not round-trip a write, in
                                       either mode                             */
#define ANXDIAG_WHY_MEM        10   /* the 32-byte probe of the buffer passed
                                       and a full 16 KB pass did not, which is
                                       buffer RAM rather than the data port   */

/* ANXDIAG_MAC_SOURCE values. */
#define ANXDIAG_MAC_PROM        0   /* the card's address PROM               */
#define ANXDIAG_MAC_PROM_FIXED  1   /* the PROM, with its group bit cleared  */
#define ANXDIAG_MAC_CIS         2   /* the card's CIS LAN node ID            */
#define ANXDIAG_MAC_DERIVED     3   /* derived: the PROM was blank           */
#define ANXDIAG_MAC_SERIAL      4   /* the autoconfig serial number (LANCE)  */

typedef struct AnxDiagStep
{
    UWORD   ds_Code;        /* ANXDIAG_*                                     */
    UWORD   ds_Card;        /* netdev_cards[] index, or ANXDIAG_NOCARD       */
    ULONG   ds_Value;
} AnxDiagStep;

typedef struct AnxDiagMark
{
    /* First, so FindSemaphore() returns the record itself. */
    struct SignalSemaphore ad_Semaphore;

    ULONG   ad_Magic;                   /* ANXDIAG_MAGIC                     */
    UWORD   ad_Version;                 /* ANXDIAG_VERSION                   */
    UWORD   ad_Size;                    /* sizeof(AnxDiagMark)               */

    UWORD   ad_Used;                    /* steps recorded, <= ANXDIAG_STEPS  */
    UWORD   ad_Lost;                    /* steps the recorder had to drop    */
    UWORD   ad_Units;                   /* units that came up                */
    UWORD   ad_Dropped;                 /* supported boards with no unit     */

    /*
     * The card names, so the tool never has to agree with netdev_cards.c
     * about row order: index by ds_Card.
     *
     * COPIED IN, not pointed at.  A pointer here would be into the device's
     * own segment, and the reader prints after it has let Forbid() go -- the
     * one moment the segment may be expunged out from under it.  The whole
     * record is copied by value for that reason, and a record with pointers
     * in it cannot be.
     */
    UWORD   ad_Cards;                   /* how many ad_Name entries are set  */
    UWORD   ad_Pad;
    char    ad_Name[16][12];

    AnxDiagStep ad_Step[ANXDIAG_STEPS];
} AnxDiagMark;

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_ANXDIAG_H */
