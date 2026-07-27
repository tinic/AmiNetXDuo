/*
 * src/tools -- what a command needs in order to explain a NETWORK CARD.
 *
 * WHY THIS IS NOT IN tool_diag.c
 *
 * tool_diag.c is in TOOLS_COMMON_SOURCES, so every command links it, and this
 * material is 3.8 KB of prose plus a table of sixteen driver names.  `whois`
 * carried "a2065.device ... uaenet.device" and "the router or switch at the
 * other end is powered on" without any way to reach the code that prints them.
 *
 * --gc-sections DOES collect the dead functions; it cannot collect their
 * strings.  On m68k-amigaos there is no .rodata: string literals go into the
 * plain `.text`, pooled, while -ffunction-sections gives each function its own
 * `.text.<name>`.  One surviving string anchors the whole pool.  Measured
 * rather than assumed, and every flag that claims to fix it was tried:
 *
 *   -fdata-sections        acts on named data objects; there are none here.
 *                          Also gives the `$VER:` tag a section of its own
 *                          that nothing references, so every command silently
 *                          loses its version string (cmake/check-version-tag
 *                          .cmake), and the binaries come out 0.9% LARGER.
 *   -fno-merge-constants   unpools literals from mergeable .rodata.str1.1
 *                          sections, which this target never creates.  No
 *                          effect: all 18 device names still present.
 *   -flto                  would drop them before sections are assigned; this
 *                          binutils has no LTO plugin ("plugin needed to
 *                          handle lto object").
 *
 * So the only mechanism left is not to compile it into commands that cannot
 * reach it, which is what this file is for.  Exactly three functions touched
 * the device table -- tool_explain_device, tool_explain_no_interfaces and
 * tool_scan_devices -- and they live here with it.
 *
 * tool_find_interface() came from tool_util.c, which is also common: its
 * "there is no interface called X" path calls tool_explain_no_interfaces(),
 * which was enough to drag the table into every command on its own.  Its only
 * callers are AddNetInterface and Online/Offline, both of which link this.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>

#include "sana2_device.h"

#include "aminetxduo/compat.h"

/* Same directory as tool_diag.c uses; duplicated rather than exported,
   because one string is cheaper than a header for one constant. */
#define DIAG_DIR_NETWORKS     "DEVS:Networks"

/* Shared with tool_diag.c, which keeps them for tool_device_where(). */
extern const char *const diag_device_dirs[];
extern BOOL diag_is_resident(const char *device);

/*
 * SANA-II drivers we know by name. Only used to look harder -- a driver not on
 * this list is found perfectly well by the DEVS:Networks scan. It exists so
 * that a machine whose driver is already in memory (loaded from a Zorro ROM,
 * or by an earlier stack) is reported as having a card even though nothing is
 * on disk.
 */
static const char *const diag_known_devices[] =
{
    "a2065.device", "ariadne.device", "ariadne2.device", "amiganet.device",
    "cnet.device", "hydra.device", "x-surf.device", "xsurf100.device",
    "e3b_ax88796.device", "prism2.device", "eth3com.device", "rtl8029.device",
    "emac.device", "uaenet.device", "slip.device", "ppp.device",

    /*
     * USB Ethernet through Poseidon, on a Deneb, Subway or Algor. Both names
     * are documented in the Roadshow 1.15 manual, so they are as certain as
     * anything else on this list.
     */
    "moschipeth.device", "usbmoschipeth.device",

    /*
     * The modern hardware, and the reason this list exists at all: none of
     * these keeps its driver in DEVS:Networks where the scan above would find
     * it. ZZ9000 and PiStorm load theirs from the card, so a machine running
     * one has a working card with nothing on disk to prove it.
     *
     * Every name here was read out of the vendor's own build, not guessed:
     *
     *   ZZ9000Net.device   BlitterStudio/zz9000-drivers, net/Makefile:
     *                      "Makefile for ZZ9000Net.device (SANA-II)"
     *   pi-net.device      captain-amygdala/pistorm, the built driver at
     *                      platforms/amiga/net/net_driver_amiga/
     *   a314eth.device     the same repo, a314/software-amiga/ethernet_pistorm
     *                      build.sh -- "-o ../a314eth.device"
     *   scsidayna.device   RobSmithDev/daynaport-amiga, Makefile DEVICEID
     *                      (a DaynaPORT SCSI/Link, still being worked on)
     *
     * The mixed case in ZZ9000Net is the vendor's. It costs nothing either
     * way -- diag_is_resident() compares with tool_stricmp() and AmigaDOS
     * paths are case-insensitive -- but it is what a user would see.
     */
    "ZZ9000Net.device",         /* MNT ZZ9000                               */
    "pi-net.device",            /* PiStorm                                  */
    "a314eth.device",           /* A314, and PiStorm's emulation of it      */
    "scsidayna.device",         /* DaynaPORT SCSI/Link                      */

    /*
     * plipbox (cnvogelg/plipbox, amiga/src/makefile DEVICE_NAME). It reaches
     * the network over the parallel port, which sounds like it belongs with
     * SLIP and PPP above and does not: HW_ADDRFIELDSIZE is 6, the header is
     * 14 bytes of dst/src/type, the MTU is 1500 and it reports
     * S2WireType_Ethernet. It is an ordinary Ethernet SANA-II device whose
     * cable happens to be a parallel one, and the bridge on the far end is
     * what makes it so -- nothing above the driver can tell.
     *
     * Which is the point of driving SANA-II generically: this needed no code,
     * only the name.
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

VOID tool_explain_device(const char *device, ULONG unit)
{
    const char *where = tool_device_where(device);
    LONG        probe;

    tool_advise_blank();

    if (where == NULL)
    {
        tool_printf("  There is no %s on this machine.\n", (LONG)device);
        tool_advise_blank();
        tool_advise("That is the driver for your network card, and it has to be");
        tool_advise("installed before anything can use the card. Drivers belong");
        tool_advise("in DEVS:Networks/ -- they come with the card, or with the");
        tool_advise("operating system for cards Commodore made.");

        if (tool_scan_devices() > 0)
        {
            ULONG i;

            tool_advise_blank();
            tool_advise("These network drivers ARE installed:");
            for (i = 0; i < tool_scan_devices(); i++)
            {
                const ToolDevice *dev = tool_scan_device(i);

                tool_printf("      %-24s (%s)\n", (LONG)dev->name,
                            (LONG)dev->where);
            }
            tool_advise("If one of those is your card, run NetSetup and pick it.");
        }

        return;
    }

    /*
     * The driver is there, so ask it directly rather than guessing. This is
     * the difference between "would not open" and "your card is on unit 0,
     * not unit 1" -- which is the actual mistake most of the time.
     */
    probe = tool_device_probe(device, unit);

    if (probe == 0)
    {
        tool_printf("  %s unit %lu opens perfectly well on its own, so the\n",
                    (LONG)device, unit);
        tool_advise("card and its driver are fine.");
        tool_advise_blank();
        tool_advise("Something else has it open -- another network stack, or an");
        tool_advise("earlier copy of this one that is still running. A reboot");
        tool_advise("clears that. If this machine has no other stack installed,");
        tool_advise("the serial debug log records what actually failed.");
        return;
    }

    tool_printf("  %s is installed (%s) but unit %lu would not open.\n",
                (LONG)device, (LONG)where, unit);

    if (unit != 0 && tool_device_probe(device, 0) == 0)
    {
        tool_advise_blank();
        tool_printf("  Unit 0 opens. Almost every card is unit 0: change the UNIT\n");
        tool_printf("  line in DEVS:NetInterfaces to 0, or run NetSetup again.\n");
        return;
    }

    tool_advise_blank();
    tool_advise("The driver is installed but the card is not answering. Usually");
    tool_advise("that means the card is not in the machine, is not seated");
    tool_advise("properly, or needs a different unit number.");
}

VOID tool_explain_no_interfaces(VOID)
{
    ULONG n;

    tool_advise_blank();
    tool_advise("No network interfaces are configured.");
    tool_advise_blank();
    tool_advise("The stack reads one file per network card from");
    tool_advise("DEVS:NetInterfaces. There is nothing usable there yet.");

    n = tool_scan_devices();

    if (n > 0)
    {
        ULONG i;

        tool_advise_blank();
        tool_advise("The network card drivers on this machine are:");
        for (i = 0; i < n; i++)
        {
            const ToolDevice *dev = tool_scan_device(i);

            tool_printf("      %-24s (%s)\n", (LONG)dev->name, (LONG)dev->where);
        }
        tool_advise_blank();
        tool_advise("Run  NetSetup  and pick it from the list. Nothing has been");
        tool_advise("changed for you -- NetSetup asks first and writes after.");
    }
    else
    {
        tool_advise_blank();
        tool_advise("No network card driver could be found either. The driver for");
        tool_advise("your card belongs in DEVS:Networks/ -- for example");
        tool_advise("DEVS:Networks/ariadne.device for an Ariadne, or a2065.device");
        tool_advise("for an A2065. Copy it there first, then run  NetSetup.");
    }
}

LONG tool_find_interface(const char *name)
{
    const AmiConfig *cfg = netstack_config();
    UWORD            i;

    if (cfg == NULL)
    {
        if (tool_stack_library_running())
            tool_error("the network is up, but this command cannot read it");
        else
            tool_error("the network has not been started");

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
        tool_advise_blank();
        tool_advise("The interfaces this machine has are:");
        for (i = 0; i < cfg->interface_count; i++)
            tool_printf("      %s\n", (LONG)cfg->interfaces[i].name);
        tool_advise("The name is the name of the file in DEVS:NetInterfaces.");
    }

    return -1;
}
