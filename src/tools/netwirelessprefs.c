/*
 * NetWirelessPrefs, the Wi-Fi window: the networks in range, joining one,
 * and the known networks, all written to WirelessManager's
 * ENV:Sys/Wireless.prefs.  From a Shell, LIST and STATUS print the same
 * answers the window shows.
 *
 * WirelessManager owns the association: this program scans and reads the
 * link, writes the file and restarts WirelessManager, and never joins or
 * sets a key itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netwireless_dev.h"
#include "netwireless_sig.h"
#include "aminetxduo/version.h"

#include <exec/execbase.h>

const char *const tool_name = "NetWirelessPrefs";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetWirelessPrefs");

#define TEMPLATE "DEVICE/K,UNIT/K/N,INTERFACE/K,LIST/S,STATUS/S"

enum
{
    ARG_DEVICE,
    ARG_UNIT,
    ARG_INTERFACE,
    ARG_LIST,
    ARG_STATUS,
    ARG_COUNT
};

#define NW_LIST_MAX     32
#define NW_DEVICE_LEN   128

static NwDev  nw_dev;
static NwScan nw_list[NW_LIST_MAX];
static char   nw_device[NW_DEVICE_LEN];
static ULONG  nw_unit;

/* DEVICE as given; else the interface file's; else, when the interface was
   not named, the Pi Wi-Fi's own driver.  FALSE for a named interface that
   names no device. */
static BOOL nw_pick_device(LONG *args)
{
    nw_unit = 0;
    if (args[ARG_DEVICE] != 0)
        tool_copy_string(nw_device, sizeof(nw_device),
                         (const char *)args[ARG_DEVICE]);
    else if (args[ARG_INTERFACE] != 0)
    {
        if (!nw_dev_from_interface((const char *)args[ARG_INTERFACE],
                                   nw_device, sizeof(nw_device), &nw_unit))
            return FALSE;
    }
    else if (!nw_dev_from_interface(NW_DEFAULT_INTERFACE, nw_device,
                                    sizeof(nw_device), &nw_unit))
        tool_copy_string(nw_device, sizeof(nw_device), NW_DEFAULT_DEVICE);
    if (args[ARG_UNIT] != 0)
        nw_unit = (ULONG)*(LONG *)args[ARG_UNIT];
    return TRUE;
}

static VOID nw_print_ssid(const UBYTE *ssid, LONG len, ULONG width)
{
    char  text[NW_SSID_MAX + 1];
    ULONG n;

    nw_ssid_text(ssid, (int)len, text);
    tool_printf("%s", (LONG)text);
    for (n = (ULONG)len; n < width; n++) tool_printf(" ");
}

static LONG nw_status(VOID)
{
    LONG  signal = 0, noise = 0, ssid_len = -1, state;
    UBYTE ssid[NW_SSID_MAX], bssid[6];
    char  mac[20];

    state = nw_dev_signal(&nw_dev, &signal, &noise);
    tool_printf("Device        %s, unit %lu\n", (LONG)nw_device, (LONG)nw_unit);
    switch (state)
    {
        case NW_LINK_OFF:
            tool_printf("Link          off\n");
            return RETURN_WARN;
        case NW_LINK_DOWN:
            tool_printf("Link          not connected\n");
            return RETURN_WARN;
        case NW_LINK_ERROR:
            tool_printf("Link          no answer (%s, %s)\n",
                        (LONG)tool_code_sana2(nw_dev.req->ios2_Req.io_Error),
                        (LONG)tool_code_wire((LONG)nw_dev.req->ios2_WireError));
            return RETURN_ERROR;
        default:
            break;
    }
    tool_printf("Link          connected\n");
    if (nw_dev_network(&nw_dev, ssid, &ssid_len, bssid) && ssid_len >= 0)
    {
        tool_printf("Network       ");
        nw_print_ssid(ssid, ssid_len, 0);
        tool_format_mac(bssid, mac, sizeof(mac));
        tool_printf("\nAccess point  %s\n", (LONG)mac);
    }
    tool_printf("Signal        %ld dBm, %ld of 4 bars\n", signal,
                (LONG)nw_bars(signal));
    if (noise != 0) tool_printf("Noise         %ld dBm\n", noise);
    return RETURN_OK;
}

static LONG nw_scan_list(VOID)
{
    ULONG sigs, width = 7;
    LONG  n = -1, i;
    BOOL  broke = FALSE;

    if (SysBase->LibNode.lib_Version < 39)
    {
        tool_error("scanning needs Kickstart 3.0 (exec V39 memory pools)");
        return RETURN_FAIL;
    }
    if (!nw_dev_bring_up(&nw_dev))
    {
        tool_error("the unit could not be configured (%s, %s)",
                   (LONG)tool_code_sana2(nw_dev.req->ios2_Req.io_Error),
                   (LONG)tool_code_wire((LONG)nw_dev.req->ios2_WireError));
        return RETURN_ERROR;
    }
    if (!nw_dev_scan_start(&nw_dev, NULL, 0))
    {
        tool_error("not enough memory to scan");
        return RETURN_FAIL;
    }

    /* A running scan is never aborted: Ctrl-C is noted and waited out. */
    while (nw_dev.scan_busy)
    {
        sigs = Wait((1UL << nw_dev.async->mp_SigBit) | SIGBREAKF_CTRL_C);
        if ((sigs & SIGBREAKF_CTRL_C) && !broke)
        {
            broke = TRUE;
            tool_say("Waiting for the scan to finish.\n");
        }
        if (nw_dev_reply(&nw_dev) == NW_REPLY_SCAN)
            n = nw_dev_scan_collect(&nw_dev, nw_list, NW_LIST_MAX);
    }
    if (broke) return RETURN_WARN;
    if (n < 0)
    {
        tool_error("the scan failed (%s, %s)",
                   (LONG)tool_code_sana2(nw_dev.scan->ios2_Req.io_Error),
                   (LONG)tool_code_wire((LONG)nw_dev.scan->ios2_WireError));
        return RETURN_ERROR;
    }

    for (i = 0; i < n; i++)
        if ((ULONG)nw_list[i].ssid_len > width) width = (ULONG)nw_list[i].ssid_len;
    tool_printf("Network");
    for (i = 7; i < (LONG)width; i++) tool_printf(" ");
    tool_printf("  Signal     Ch  Band  Security\n");
    for (i = 0; i < n; i++)
    {
        const NwScan *e = &nw_list[i];

        nw_print_ssid(e->ssid, e->ssid_len, width);
        tool_printf("  %4ld dBm  %3ld  %s  %s\n", e->signal, (LONG)e->channel,
                    (LONG)(e->band5 ? "5   " : "2.4 "),
                    (LONG)nw_security_label(e->sec));
    }
    if (n == 0) tool_printf("No networks in range.\n");
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    LONG           rc, err;

    (VOID)argv;
    if (tool_from_workbench(argc)) return RETURN_FAIL;
    tool_break_arm();

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[DEVICE <name>] [UNIT <n>] [INTERFACE <name>] [LIST] [STATUS]",
                   "List the Wi-Fi networks in range, or the link's state.");
        return RETURN_ERROR;
    }
    if (!nw_pick_device(args))
    {
        tool_error("DEVS:NetInterfaces/%s names no DEVICE",
                   (LONG)args[ARG_INTERFACE]);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    err = nw_dev_open(&nw_dev, nw_device, nw_unit);
    if (err != 0)
    {
        tool_error("cannot open %s unit %lu (error %ld)", (LONG)nw_device,
                   (LONG)nw_unit, err);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    rc = args[ARG_LIST] != 0 ? nw_scan_list() : nw_status();
    nw_dev_close(&nw_dev);
    FreeArgs(rda);
    return (int)rc;
}
