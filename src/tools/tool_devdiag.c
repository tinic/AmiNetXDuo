/*
 * src/tools, what a command needs in order to explain a network card.
 *
 * Kept out of tool_diag.c, which is in TOOLS_COMMON_SOURCES and so linked by
 * every command: this material is 3.8 KB of prose plus a table of sixteen
 * driver names.  `whois` carried the whole device-name table and the cable
 * advice that goes with it, with no way to reach the code that prints them.
 *
 * --gc-sections does collect the dead functions. It cannot collect their
 * strings.  On m68k-amigaos there is no .rodata: string literals go into the
 * plain `.text`, pooled, while -ffunction-sections gives each function its own
 * `.text.<name>`.  One surviving string anchors the whole pool.  Measured, and
 * every flag that claims to fix it was tried:
 *
 *   -fdata-sections        acts on named data objects, and there are none here.
 *                          Also gives the `$VER:` tag a section of its own
 *                          that nothing references, so every command silently
 *                          loses its version string (cmake/check-version-tag
 *                          .cmake), and the binaries come out 0.9% larger.
 *   -fno-merge-constants   unpools literals from mergeable .rodata.str1.1
 *                          sections, which this target never creates.  No
 *                          effect: all 18 device names still present.
 *   -flto                  would drop them before sections are assigned. This
 *                          binutils has no LTO plugin ("plugin needed to
 *                          handle lto object").
 *
 * The remaining mechanism is not to compile it into commands that cannot reach
 * it, which is what this file is for.  The three functions that touch the
 * device table, tool_explain_device, tool_explain_no_interfaces and
 * tool_scan_devices, live here with it.
 *
 * tool_find_interface() came from tool_util.c, which is also common: its
 * "there is no interface called X" path calls tool_explain_no_interfaces(),
 * which dragged the table into every command on its own.  Its only callers are
 * AddNetInterface and Online/Offline, both of which link this.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>

#include "sana2_device.h"

#include "aminetxduo/compat.h"

/* Same directory as tool_diag.c uses, duplicated rather than exported,
   because one string is cheaper than a header for one constant. */
#define DIAG_DIR_NETWORKS     "DEVS:Networks"

/* Shared with tool_diag.c, which keeps them for tool_device_where(). */
extern const char *const diag_device_dirs[];
extern BOOL diag_is_resident(const char *device);

/*
 * SANA-II drivers known by name, used only to look harder. A driver not on this
 * list is still found by the DEVS:Networks scan. This catches a machine whose
 * driver is already in memory (loaded from a Zorro ROM, or by an earlier stack)
 * with nothing on disk.
 */
static const char *const diag_known_devices[] =
{
    /*
     * The file names the drivers really ship under, which is not what the
     * boards are called: Village Tronic's Ariadne II is ariadne_ii.device,
     * Hydra Systems' AmigaNet is hydra.device, ASDG's LAN Rover is
     * eb920.device. This list held ariadne2.device, amiganet.device and
     * xsurf100.device, none of which is a file that exists, so the three
     * cards they stood for were never found by name.
     */
    "a2065.device", "ariadne.device", "ariadne_ii.device", "hydra.device",
    "eb920.device", "cnet.device", "x-surf.device", "x-surf-100.device",
    "e3b_ax88796.device", "prism2.device", "eth3com.device", "rtl8029.device",
    "emac.device", "uaenet.device", "slip.device", "ppp.device",

    /*
     * USB Ethernet through Poseidon, on a Deneb, Subway or Algor. Both names
     * are documented in the Roadshow 1.15 manual.
     */
    "moschipeth.device", "usbmoschipeth.device",

    /*
     * Modern hardware: none of these keeps its driver in DEVS:Networks where
     * the scan above would find it. ZZ9000 and PiStorm load theirs from the
     * card, so a machine running one has a working card with nothing on disk.
     *
     * Every name here was read out of the vendor's own build:
     *
     *   ZZ9000Net.device   BlitterStudio/zz9000-drivers, net/Makefile:
     *                      "Makefile for ZZ9000Net.device (SANA-II)"
     *   pi-net.device      captain-amygdala/pistorm, the built driver at
     *                      platforms/amiga/net/net_driver_amiga/
     *   a314eth.device     the same repo, a314/software-amiga/ethernet_pistorm
     *                      build.sh, "-o ../a314eth.device"
     *   scsidayna.device   RobSmithDev/daynaport-amiga, Makefile DEVICEID
     *                      (a DaynaPORT SCSI/Link, still being worked on)
     *
     * The mixed case in ZZ9000Net is the vendor's, and is what a user sees.
     * Matching does not depend on it: diag_is_resident() compares with
     * tool_stricmp() and AmigaDOS paths are case-insensitive.
     */
    "ZZ9000Net.device",         /* MNT ZZ9000                               */
    "pi-net.device",            /* PiStorm                                  */
    "a314eth.device",           /* A314, and PiStorm's emulation of it      */
    "scsidayna.device",         /* DaynaPORT SCSI/Link                      */

    /*
     * plipbox (cnvogelg/plipbox, amiga/src/makefile DEVICE_NAME) reaches the
     * network over the parallel port but does not belong with SLIP and PPP:
     * HW_ADDRFIELDSIZE is 6, the header is 14 bytes of dst/src/type, the MTU is
     * 1500 and it reports S2WireType_Ethernet. It is an ordinary Ethernet
     * SANA-II device whose cable is a parallel one, so nothing above the driver
     * can tell and supporting it needed no code, only the name.
     */
    "plipbox.device",           /* plipbox, and the ESP32 variants of it    */
    NULL
};

static ToolDevice diag_found[TOOL_MAX_DEVICES];
static UWORD      diag_found_count;
static BOOL       diag_scanned;

static BOOL diag_already_found(const char *name)
{
    UWORD i;

    for (i = 0; i < diag_found_count; i++)
    {
        if (tool_stricmp(diag_found[i].name, name) == 0)
            return TRUE;
    }

    return FALSE;
}

static VOID diag_add(const char *name, const char *where)
{
    if (diag_found_count >= (UWORD)TOOL_MAX_DEVICES || diag_already_found(name))
        return;

    tool_copy_string(diag_found[diag_found_count].name, TOOL_NAME_LEN, name);
    tool_copy_string(diag_found[diag_found_count].where, TOOL_NAME_LEN, where);
    diag_found_count++;
}

ULONG tool_scan_devices(VOID)
{
    char  names[TOOL_MAX_DEVICES][TOOL_NAME_LEN];
    ULONG n;
    ULONG i;
    int   d;

    if (diag_scanned)
        return diag_found_count;

    diag_scanned     = TRUE;
    diag_found_count = 0;

    /* Everything in DEVS:Networks is, by convention, a SANA-II driver. */
    n = tool_list_dir(DIAG_DIR_NETWORKS, names, (ULONG)TOOL_MAX_DEVICES,
                      ".device");
    for (i = 0; i < n; i++)
        diag_add(names[i], DIAG_DIR_NETWORKS);

    /* Then the names we know, wherever they happen to be. */
    for (d = 0; diag_known_devices[d] != NULL; d++)
    {
        const char *name = diag_known_devices[d];
        int         dir;

        if (diag_already_found(name))
            continue;

        if (diag_is_resident(name))
        {
            diag_add(name, "already in memory");
            continue;
        }

        for (dir = 0; diag_device_dirs[dir] != NULL; dir++)
        {
            char path[TOOL_NAME_LEN * 2];

            tool_join_path(path, sizeof(path), diag_device_dirs[dir], name);
            if (tool_exists(path))
            {
                diag_add(name, diag_device_dirs[dir]);
                break;
            }
        }
    }

    return diag_found_count;
}

const ToolDevice *tool_scan_device(ULONG index)
{
    if (index >= (ULONG)diag_found_count)
        return NULL;

    return &diag_found[index];
}

VOID tool_explain_device_refused(const char *device, ULONG unit)
{
    LONG error = 0;
    LONG wire  = 0;

    tool_probe_sana2_codes(&error, &wire);

    /*
     * THE OPERATION AND ITS CODES, on the first line.  This used to read "%s
     * unit %lu opened, then refused a SANA-II command" and stop, which named
     * neither the command nor the answer: there was nothing in it to look up,
     * quote, or search the source for.  S2_DEVICEQUERY is the command
     * tool_device_probe() sends, and the pair is what the driver put in
     * ios2_Error and ios2_WireError.
     */
    tool_printf("  S2_DEVICEQUERY: %s unit %lu opened, then refused it "
                "(%s/%s, %ld/%ld)\n",
                (LONG)device, unit,
                (LONG)tool_code_sana2(error), (LONG)tool_code_wire(wire),
                error, wire);
}

VOID tool_explain_device(const char *device, ULONG unit, const char *card)
{
    const char *where = tool_device_where(device);
    LONG        probe;

    /*
     * CARD= comes first, because every answer below it depends on it. A driver
     * that covers a family of boards opens on UNIT alone, so an unpinned probe
     * succeeds on the wrong card and reports that the unit opens, at someone
     * whose interface just refused to come up. The pinned probe is the one the
     * stack made, and this is the one line that names what was asked for.
     */
    if (card != NULL && *card != '\0' &&
        tool_device_probe(device, unit, card) != 0 &&
        tool_device_probe(device, unit, NULL) == 0)
    {
        tool_printf("  %s unit %lu opens, but there is no %s in this "
                    "machine, and CARD= pinned it to one.\n",
                    (LONG)device, unit, (LONG)card);
        tool_printf("  Correct the CARD line in DEVS:NetInterfaces/, or "
                    "remove it and let UNIT choose.\n");
        return;
    }


    /*
     * Ask the machine before saying the driver is not on it.
     *
     * tool_device_where() is a file scan over four directories plus a check of
     * the device list, and a driver can be openable without being in any of
     * them: loaded from a card's own ROM, reached through an assign, or sitting
     * somewhere this list does not know. Deciding on the scan alone printed
     * "There is no cnet.device on this machine" at someone whose cnet.device
     * had just opened and then failed to initialise, which sent them looking
     * for a driver they already had.
     */
    probe = tool_device_probe(device, unit, card);

    /*
     * Opened and then refused a SANA-II command. The driver is installed, the
     * unit is right and the card answered, so none of the advice below about
     * missing files or wrong unit numbers applies, wherever the file turned out
     * to be. This is the case AddNetInterface reaches when a PCMCIA card is in
     * the slot and will not initialise, and it used to print "There is no
     * <driver> on this machine" at someone whose driver had just run.
     */
    if (probe == TOOL_PROBE_REFUSED)
    {
        tool_explain_device_refused(device, unit);
        return;
    }

    if (where == NULL && probe == 0)
    {
        tool_printf("  %s unit %lu opens, and no driver file was found.\n",
                    (LONG)device, unit);
        return;
    }

    if (where == NULL)
    {
        tool_printf("  There is no %s on this machine.\n", (LONG)device);

        if (tool_scan_devices() > 0)
        {
            ULONG i;

            for (i = 0; i < tool_scan_devices(); i++)
            {
                const ToolDevice *dev = tool_scan_device(i);

                tool_printf("      %-24s (%s)\n", (LONG)dev->name,
                            (LONG)dev->where);
            }
        }

        return;
    }

    /* Probed above, before the absence branch: what it answered separates
       "did not open" from "the card is on unit 0, not unit 1", which is the
       usual mistake. */
    if (probe == 0)
    {
        /*
         * THE SENTENCE THAT COST SOMEBODY AN EVENING used to end here with
         * "Check the debug log for what failed after the device opened."
         *
         * There is no debug log.  AMI_ERROR, AMI_WARN and AMI_INFO compile to
         * nothing unless AMINETXDUO_LOG is defined, and it is off in every
         * shipped build (aminetxduo/compat.h, docs/BACKLOG.md), so the one
         * thing this told the user to go and read cannot exist on the machine
         * they were reading it on.
         *
         * ShowNetStatus EVENTS is what does exist.  The library records
         * bring-up failures in the event ring -- which call refused and what
         * it answered -- and that command reads the ring through the published
         * semaphore without opening the library, so it works with the stack
         * down, which is the state this branch is reached in.
         */
        tool_printf("  %s unit %lu opens on its own, so neither the card nor "
                    "the driver is what stopped the stack.\n",
                    (LONG)device, unit);
        tool_printf("  ShowNetStatus EVENTS names the call that refused; "
                    "CheckNetConfig reads the interface file.\n");
        return;
    }

    tool_printf("  %s is installed (%s) but unit %lu did not open.\n",
                (LONG)device, (LONG)where, unit);

    if (unit != 0 && tool_device_probe(device, 0, card) == 0)
    {
        tool_printf("  Unit 0 opens, and almost every card is unit 0.\n");
        tool_printf("  Change the UNIT line in DEVS:NetInterfaces/ to 0, or "
                    "run NetSetup again.\n");
        return;
    }

    /* DEVS: and DEVS:Networks are the two places a bare device name reaches.
       Anywhere else has to be named in full in DEVS:NetInterfaces. */
    if (where[0] == 'S' && where[1] == 'Y' && where[2] == 'S' && where[3] == ':')
    {
        tool_printf("  A driver in %s cannot be opened by name alone.\n",
                    (LONG)where);
        return;
    }

}

VOID tool_explain_no_interfaces(VOID)
{
    ULONG n;
    ULONG i;

    /* On a machine with no drivers this is the only line printed. Without it
       ShowNetStatus put its "What to look at" heading over nothing at all. */
    tool_printf("  No network interfaces are configured in "
                "DEVS:NetInterfaces.\n");

    n = tool_scan_devices();

    for (i = 0; i < n; i++)
    {
        const ToolDevice *dev = tool_scan_device(i);

        tool_printf("      %-24s (%s)\n", (LONG)dev->name, (LONG)dev->where);
    }
}

LONG tool_find_interface(const char *name)
{
    const AmiConfig *cfg = netstack_config();
    UWORD            i;

    if (cfg == NULL)
    {
        /* tool_explain_no_stack() already splits those two cases itself. */
        tool_explain_no_stack();
        return -1;
    }

    for (i = 0; i < cfg->interface_count; i++)
    {
        const char *a = cfg->interfaces[i].name;
        const char *b = name;

        while (*a != '\0' && *b != '\0')
        {
            char ca = *a++;
            char cb = *b++;

            if (ca >= 'A' && ca <= 'Z')
                ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z')
                cb = (char)(cb + 32);
            if (ca != cb)
                break;
        }

        if (*a == '\0' && *b == '\0')
            return (LONG)i;
    }

    tool_error("there is no interface called \"%s\"", (LONG)name);

    if (cfg->interface_count == 0)
    {
        tool_explain_no_interfaces();
    }
    else
    {
        for (i = 0; i < cfg->interface_count; i++)
            tool_printf("      %s\n", (LONG)cfg->interfaces[i].name);
    }

    return -1;
}
