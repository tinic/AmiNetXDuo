/*
 * NetWireless: the Wi-Fi SANA-II unit, through the wireless commands of Neil
 * Cafferkey's sana2wireless.h, as WirelessManager uses them
 * (third_party/wifipi/tests/callers/wirelessmanager-1.5.tsv).
 *
 * Shared by the resident NetWireless and the NetWirelessPrefs window.  Neither
 * joins a network nor sets a key: WirelessManager owns the association.
 *
 * A running S2_GETNETWORKS is never aborted.  anxwifipi.device up to beta8
 * corrupts its scan queue on that AbortIO, so a scan is always waited out,
 * which takes one to two seconds.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETWIRELESS_DEV_H
#define AMINETXDUO_NETWIRELESS_DEV_H

#include <exec/types.h>
#include <exec/ports.h>

#include "sana2_device.h"
#include "netwireless_sig.h"

/* sana2wireless.h, Neil Cafferkey 2011-2013, the documented interface. */
#define NW_S2INFO_SSID          (TAG_USER + 0)
#define NW_S2INFO_BSSID         (TAG_USER + 1)
#define NW_S2INFO_Channel       (TAG_USER + 7)
#define NW_S2INFO_Signal        (TAG_USER + 8)
#define NW_S2INFO_Noise         (TAG_USER + 9)
#define NW_S2INFO_Capabilities  (TAG_USER + 10)
#define NW_S2INFO_InfoElements  (TAG_USER + 11)
#define NW_S2INFO_Band          (TAG_USER + 13)

#define NW_S2_GETSIGNALQUALITY  0xc010
#define NW_S2_GETNETWORKS       0xc011
#define NW_S2_GETNETWORKINFO    0xc014

#define NW_S2BAND_A             0           /* 5 GHz */
#define NW_S2BAND_B             1
#define NW_S2BAND_G             2

/* Cafferkey's sana2.h additions; anxwifipi.device reports both. */
#define NW_S2EVENT_CONNECT      (1L << 9)
#define NW_S2EVENT_DISCONNECT   (1L << 10)

/* What the unit says about the link. */
enum
{
    NW_LINK_NODEV,                  /* not open                            */
    NW_LINK_OFF,                    /* unit offline                        */
    NW_LINK_DOWN,                   /* online, not associated              */
    NW_LINK_UP,                     /* associated                          */
    NW_LINK_ERROR                   /* the unit did not answer             */
};

typedef struct NwDev
{
    struct MsgPort    *port;        /* DoIO() replies                      */
    struct MsgPort    *async;       /* the scan's and the event's replies  */
    struct IOSana2Req *req;
    struct IOSana2Req *scan;
    struct IOSana2Req *event;
    APTR               scan_pool;
    BOOL               scan_busy;
    BOOL               scan_replied;    /* taken off async by nw_dev_reply */
    BOOL               event_busy;
    struct TagItem     scan_tags[2];
    UBYTE              scan_ssid[NW_SSID_MAX + 1];
    LONG               net_channel;     /* from nw_dev_network, -1: absent */
    LONG               net_band5;       /* 1: 5 GHz, 0: 2.4 GHz, -1        */
} NwDev;

#define NW_IFACE_MAX            8192
#define NW_DEFAULT_DEVICE       "anxwifipi.device"
#define NW_DEFAULT_INTERFACE    "wifipi"

/* DEVICE and UNIT of DEVS:NetInterfaces/<name>.  FALSE when the file is
   not there or names no DEVICE; *unit is set only when it names one. */
BOOL nw_dev_from_interface(const char *name, char *device, ULONG cap,
                           ULONG *unit);

/* DEVICE as given, then DEVS:Networks/ for a bare name.  Returns the
   OpenDevice() error, 0 on success; d is closed either way on failure. */
LONG nw_dev_open(NwDev *d, const char *device, ULONG unit);
VOID nw_dev_close(NwDev *d);

/* What WirelessManager does before it scans: get the address and configure
   with it; a unit configured already is put online only if it is offline.
   TRUE when the unit is configured afterwards. */
BOOL nw_dev_bring_up(NwDev *d);

/* One S2_GETSIGNALQUALITY: NW_LINK_UP with *signal and *noise in dBm, or
   the state the refusal names. */
LONG nw_dev_signal(NwDev *d, LONG *signal, LONG *noise);

/* The SSID and BSSID of the associated network, and d->net_channel and
   d->net_band5 where the driver reports them (exec V39: the driver
   allocates the answer from a pool of the caller's).  *ssid_len -1 when
   there is none. */
BOOL nw_dev_network(NwDev *d, UBYTE ssid[NW_SSID_MAX], LONG *ssid_len,
                    UBYTE bssid[6]);

/* S2_GETNETWORKS, asynchronously; ssid NULL for every network, or one
   network by name (a hidden one).  Needs exec V39. */
BOOL nw_dev_scan_start(NwDev *d, const UBYTE *ssid, LONG ssid_len);

/* One reply off d->async, and which request it was.  Every message on that
   port goes through here, so a request is never taken off it twice. */
enum { NW_REPLY_NONE, NW_REPLY_SCAN, NW_REPLY_EVENT };
LONG nw_dev_reply(NwDev *d);

/* After nw_dev_reply() said NW_REPLY_SCAN: the networks, merged by
   SSID and strongest first, at most max.  -1 when the scan failed. */
LONG nw_dev_scan_collect(NwDev *d, NwScan *out, LONG max);

/* Wait for a scan that is running, and drop its answer. */
VOID nw_dev_scan_wait(NwDev *d);

/* S2_ONEVENT for the events in mask, asynchronously. */
BOOL nw_dev_event_arm(NwDev *d, ULONG mask);

#endif
