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

/* What a network offers, as bits. */
#define NW_SEC_WEP       0x01       /* privacy bit with no WPA element      */
#define NW_SEC_WPA_PSK   0x02
#define NW_SEC_WPA2_PSK  0x04       /* RSN PSK or PSK-SHA256                */
#define NW_SEC_SAE       0x08       /* WPA3 personal                        */
#define NW_SEC_EAP       0x10       /* 802.1X, WPA or RSN                   */

/* The security of a scan entry from its information elements (the bytes
   after S2INFO_InfoElements' length word) and S2INFO_Capabilities. */
unsigned nw_security(const unsigned char *ie, size_t len, unsigned capabilities);

/* "Open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3",
   "Enterprise". */
const char *nw_security_label(unsigned sec);

/* WirelessManager joins it with no key or with a PSK. */
int nw_security_joinable(unsigned sec);

/* 1 to 4 bars: 4 from -55 dBm, 3 from -66, 2 from -77, 1 below. */
int nw_bars(long dbm);

/* What the AppIcon shows. */
enum
{
    NW_ICON_NODEV,                  /* bar outlines crossed out          */
    NW_ICON_OFF,                    /* dotted outlines                   */
    NW_ICON_JOINING,                /* bar outlines                      */
    NW_ICON_CONNECTED,              /* 1-4 filled bars                   */
    NW_ICON_ERROR                   /* outlines and a '!' for the tallest */
};

/* Bar i (0..3) of a w x h picture, inclusive corners. */
void nw_bar_rect(int i, int w, int h, int *x0, int *y0, int *x1, int *y1);

/* The pen of pixel (x, y): 0 background, 1 outline, 3 fill. */
int nw_icon_pen(int state, int bars, int w, int h, int x, int y);

/* The picture as a two-plane Intuition Image: plane 0's rows, then plane 1's,
   each row (w + 15) / 16 big-endian words. */
void nw_icon_render(int state, int bars, int w, int h, unsigned char *planes);

#endif
