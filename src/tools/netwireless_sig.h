/*
 * NetWireless: what a scan entry's information elements say about its
 * security, signal bars from a level in dBm, and the bar picture drawn in
 * the AppIcon and the network list.  No AmigaOS calls, so
 * test/test_netwireless_sig.c runs it on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETWIRELESS_SIG_H
#define AMINETXDUO_NETWIRELESS_SIG_H

#include <stddef.h>

#ifndef NW_SSID_MAX
#define NW_SSID_MAX     32
#endif

/* What a network offers, as bits. */
#define NW_SEC_WEP       0x01       /* privacy bit with no WPA element      */
#define NW_SEC_WPA_PSK   0x02
#define NW_SEC_WPA2_PSK  0x04       /* RSN PSK or PSK-SHA256                */
#define NW_SEC_SAE       0x08       /* WPA3 personal                        */
#define NW_SEC_EAP       0x10       /* 802.1X, WPA or RSN                   */
#define NW_SEC_OTHER     0x20       /* an AKM WirelessManager cannot use,
                                       or a WPA element naming none       */

/* The security of a scan entry from its information elements (the bytes
   after S2INFO_InfoElements' length word) and S2INFO_Capabilities. */
unsigned nw_security(const unsigned char *ie, size_t len, unsigned capabilities);

/* "Open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3",
   "Enterprise", "Other". */
const char *nw_security_label(unsigned sec);

/* WirelessManager joins it with no key, or with a PSK by key_mgmt=WPA-PSK. */
int nw_security_joinable(unsigned sec);

/* 1 to 4 bars: 4 from -55 dBm, 3 from -66, 2 from -77, 1 below. */
int nw_bars(long dbm);

/* The same with 3 dB of hysteresis around the bars shown before (0: none
   shown), so a level at a threshold does not flip the picture. */
int nw_bars_held(long dbm, int shown);

/* One network of a scan.  aps counts the access points merged into it. */
typedef struct NwScan
{
    unsigned char ssid[NW_SSID_MAX];
    int           ssid_len;
    unsigned char bssid[6];
    long          signal;           /* dBm                                */
    int           channel;
    int           band5;            /* 5 GHz                              */
    unsigned      sec;              /* NW_SEC_*                           */
    int           aps;
    int           rank;             /* NW_RANK_CONNECTED, known 1.., other */
} NwScan;

/* A list row's place: the network in use, the known ones by priority, then
   every other one as scanned (strongest first). */
#define NW_RANK_CONNECTED 0
#define NW_RANK_OTHER     0x7fff

/* The n entries stably sorted by rank. */
void nw_scan_rank_sort(NwScan *list, int n);

/* Adds e to the n entries of list, at most max, and returns the new count.
   One entry per SSID, the strongest access point's, strongest first; an
   empty or all-zero SSID (a hidden network) is not added. */
int nw_scan_add(NwScan *list, int n, int max, const NwScan *e);

/* The SSID as text: control characters as '?', NUL-terminated.  out needs
   NW_SSID_MAX + 1 bytes. */
void nw_ssid_text(const unsigned char *ssid, int len, char *out);

/* What the AppIcon shows. */
enum
{
    NW_ICON_NODEV,                  /* radio with a small unavailable X  */
    NW_ICON_OFF,                    /* radio with a power badge          */
    NW_ICON_JOINING,                /* hollow radio arcs                 */
    NW_ICON_CONNECTED,              /* dot plus 0-3 filled radio arcs     */
    NW_ICON_ERROR                   /* radio with a small '!' badge      */
};

#define NW_ICON_SIZE    32
#define NW_ICON_COLORS   8

/* Optional desktop coordinates: 0 if neither is set, 1 for a valid pair,
   -1 for a partial/invalid pair. Decimal 0..32767; outputs change only
   for a valid pair. Shared by Shell arguments and Workbench tooltypes. */
int nw_icon_position(const char *x, const char *y, long *xpos, long *ypos);

/* Neutral greys, pen zero transparent. State is conveyed by shape as well
   as shading; no theme palette or particular accent colour is required. */
extern const unsigned char nw_icon_palette[NW_ICON_COLORS][3];

/* Normal and selected palette indices, each w*h bytes. Selection changes
   shading only: both states have exactly the same opaque silhouette. */
void nw_icon_render_color(int state, int bars, int w, int h,
                          unsigned char *normal, unsigned char *selected);

/* Bar i (0..3) of a w x h picture, inclusive corners.  The four bars are
   apart and inside from 13 x 4 up; nothing smaller is drawn. */
void nw_bar_rect(int i, int w, int h, int *x0, int *y0, int *x1, int *y1);

/* Classic Workbench pens: 0 background, 1 dark, 2 light. */
int nw_icon_pen(int state, int bars, int w, int h, int x, int y);

/* The picture as a two-plane Intuition Image: plane 0's rows, then plane 1's,
   each row (w + 15) / 16 big-endian words. */
void nw_icon_render(int state, int bars, int w, int h, unsigned char *planes);

#endif
