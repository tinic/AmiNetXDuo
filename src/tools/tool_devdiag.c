/*
 * src/tools, what a command needs in order to explain a network card.
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
    "a2065.device", "ariadne.device", "ariadne_ii.device", "hydra.device",
    "eb920.device", "cnet.device", "x-surf.device", "x-surf-100.device",
    "e3b_ax88796.device", "prism2.device", "eth3com.device", "rtl8029.device",
    "emac.device", "uaenet.device", "slip.device", "ppp.device",

    /*
     * USB Ethernet through Poseidon, on a Deneb, Subway or Algor. Both names
     * are documented in the Roadshow 1.15 manual.
     */
    "moschipeth.device", "usbmoschipeth.device",

    "ZZ9000Net.device",         /* MNT ZZ9000                               */
    "pi-net.device",            /* PiStorm                                  */
    "a314eth.device",           /* A314, and PiStorm's emulation of it      */
    "scsidayna.device",         /* DaynaPORT SCSI/Link                      */

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


    probe = tool_device_probe(device, unit, card);

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
        const char *a;
        const char *b = name;

        /* The caller uses this as an NX interface index, so a slot with no
           device in it is not an answer: an interface that yielded its slot,
           or one whose card never opened, still has its name sitting in the
           list behind a cleared `configured`. */
        if (!cfg->interfaces[i].configured)
            continue;

        a = cfg->interfaces[i].name;

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
