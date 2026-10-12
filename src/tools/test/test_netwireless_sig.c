/*
 * Security labels from real element layouts, the bar thresholds, and the bar
 * picture pixel by pixel and as Intuition Image planes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_sig.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int yes, const char *what)
{
    if (!yes) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

/* RSN: version 1, group CCMP, one pairwise CCMP, then the AKM list. */
#define RSN_HEAD 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, \
                 0x00, 0x0f, 0xac, 0x04

static void test_security(void)
{
    static const unsigned char wpa2[] = {
        0x00, 0x04, 'H', 'o', 'm', 'e',                 /* SSID first */
        48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
    static const unsigned char mixed3[] = {
        48, 24, RSN_HEAD, 0x02, 0x00, 0x00, 0x0f, 0xac, 0x02,
        0x00, 0x0f, 0xac, 0x08, 0x00, 0x00 };
    static const unsigned char sae[] = {
        48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0x80, 0x00 };
    static const unsigned char eap[] = {
        48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x01, 0x00, 0x00 };
    static const unsigned char wpa1[] = {
        221, 22, 0x00, 0x50, 0xf2, 0x01, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02,
        0x01, 0x00, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02 };
    static const unsigned char both[] = {
        221, 22, 0x00, 0x50, 0xf2, 0x01, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02,
        0x01, 0x00, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02,
        48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
    static const unsigned char wmm[] = {          /* vendor 00-50-F2 type 2 */
        221, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00 };
    static const unsigned char short_rsn[] = { 48, 2, 0x01, 0x00 };
    static const unsigned char truncated[] = {
        48, 40, RSN_HEAD, 0x01, 0x00 };           /* claims more than it has */
    static const unsigned char count_lies[] = {
        48, 14, RSN_HEAD, 0x09, 0x00 };          /* 9 AKMs, none present */
    static const unsigned char owe[] = {
        48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x12, 0x00, 0x00 };
    static const unsigned char sha256[] = {
        48, 20, RSN_HEAD, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x06, 0x00, 0x00 };
    static const unsigned char ft_and_psk[] = {
        48, 24, RSN_HEAD, 0x02, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
    unsigned s;

    s = nw_security(wpa2, sizeof(wpa2), 0x0011);
    check(s == NW_SEC_WPA2_PSK && strcmp(nw_security_label(s), "WPA2") == 0, "WPA2");
    s = nw_security(mixed3, sizeof(mixed3), 0x0011);
    check(strcmp(nw_security_label(s), "WPA2/WPA3") == 0 &&
          nw_security_joinable(s), "WPA2/WPA3 transition joins by PSK");
    s = nw_security(sae, sizeof(sae), 0x0011);
    check(strcmp(nw_security_label(s), "WPA3") == 0 && !nw_security_joinable(s),
          "WPA3 only is not joinable");
    s = nw_security(eap, sizeof(eap), 0x0011);
    check(strcmp(nw_security_label(s), "Enterprise") == 0 &&
          !nw_security_joinable(s), "802.1X");
    s = nw_security(wpa1, sizeof(wpa1), 0x0011);
    check(strcmp(nw_security_label(s), "WPA") == 0 && nw_security_joinable(s), "WPA");
    s = nw_security(both, sizeof(both), 0x0011);
    check(strcmp(nw_security_label(s), "WPA/WPA2") == 0, "WPA/WPA2");
    s = nw_security(wmm, sizeof(wmm), 0x0011);
    check(strcmp(nw_security_label(s), "WEP") == 0 && !nw_security_joinable(s),
          "privacy with no WPA element is WEP");
    s = nw_security(wmm, sizeof(wmm), 0x0001);
    check(s == 0 && strcmp(nw_security_label(s), "Open") == 0 &&
          nw_security_joinable(s), "open");
    s = nw_security(NULL, 0, 0x0001);
    check(s == 0, "no elements, no privacy");
    s = nw_security(short_rsn, sizeof(short_rsn), 0x0011);
    check(s == NW_SEC_EAP, "a version-only RSN takes the 802.1X default");
    s = nw_security(truncated, sizeof(truncated), 0x0011);
    check(s == NW_SEC_WEP, "an element longer than the buffer is not read");
    s = nw_security(count_lies, sizeof(count_lies), 0x0011);
    check(s == NW_SEC_OTHER && !nw_security_joinable(s) &&
          strcmp(nw_security_label(s), "Other") == 0,
          "an AKM count past the element reads nothing past it, and no AKM "
          "is not open");
    s = nw_security(owe, sizeof(owe), 0x0011);
    check(strcmp(nw_security_label(s), "Other") == 0 && !nw_security_joinable(s),
          "OWE is not open");
    s = nw_security(sha256, sizeof(sha256), 0x0011);
    check(!nw_security_joinable(s), "PSK-SHA256 alone needs a key_mgmt WM lacks");
    s = nw_security(ft_and_psk, sizeof(ft_and_psk), 0x0011);
    check(strcmp(nw_security_label(s), "WPA2") == 0 && nw_security_joinable(s),
          "FT-PSK beside PSK joins by PSK");
}

static NwScan entry(const char *ssid, long signal)
{
    NwScan e;

    memset(&e, 0, sizeof(e));
    e.ssid_len = (int)strlen(ssid);
    memcpy(e.ssid, ssid, (size_t)e.ssid_len);
    e.signal = signal;
    return e;
}

static void test_scan(void)
{
    NwScan list[3], e;
    int    n = 0;
    char   text[NW_SSID_MAX + 1];
    static const unsigned char zeros[4] = { 0, 0, 0, 0 };
    static const unsigned char odd[5] = { 'a', 0x07, 0x7f, 0x9f, 0xe9 };

    e = entry("Mid", -60);   n = nw_scan_add(list, n, 3, &e);
    e = entry("Weak", -80);  n = nw_scan_add(list, n, 3, &e);
    e = entry("Mid", -70);   n = nw_scan_add(list, n, 3, &e);
    check(n == 2 && list[0].signal == -60 && list[0].aps == 2,
          "a weaker access point of the same network is counted, not listed");
    e = entry("Weak", -50);  n = nw_scan_add(list, n, 3, &e);
    check(n == 2 && strcmp((char *)list[0].ssid, "Weak") == 0 &&
          list[0].signal == -50 && list[0].aps == 2 && list[1].signal == -60,
          "a stronger one moves the network up");
    e = entry("", -40);      n = nw_scan_add(list, n, 3, &e);
    memset(&e, 0, sizeof(e)); memcpy(e.ssid, zeros, 4); e.ssid_len = 4;
    e.signal = -30;          n = nw_scan_add(list, n, 3, &e);
    check(n == 2, "hidden networks are not listed");
    e = entry("Third", -90); n = nw_scan_add(list, n, 3, &e);
    e = entry("Fourth", -95); n = nw_scan_add(list, n, 3, &e);
    check(n == 3 && list[2].signal == -90, "a full list keeps the strongest");
    e = entry("Strong", -20); n = nw_scan_add(list, n, 3, &e);
    check(n == 3 && list[0].signal == -20 && list[2].signal == -60,
          "a stronger one pushes the weakest out");
    check(nw_scan_add(list, 0, 0, &e) == 0, "no room");

    nw_ssid_text(odd, 5, text);
    check(strcmp(text, "a???\xe9") == 0, "control characters as '?'");
}

static void test_bars(void)
{
    check(nw_bars(-30) == 4 && nw_bars(-55) == 4, "4 bars");
    check(nw_bars(-56) == 3 && nw_bars(-66) == 3, "3 bars");
    check(nw_bars(-67) == 2 && nw_bars(-77) == 2, "2 bars");
    check(nw_bars(-78) == 1 && nw_bars(-88) == 1 && nw_bars(-100) == 1, "1 bar");
    check(nw_bars_held(-60, 0) == 3, "nothing shown: the plain bars");
    check(nw_bars_held(-54, 3) == 3 && nw_bars_held(-52, 3) == 4,
          "up only 3 dB past the threshold");
    check(nw_bars_held(-58, 4) == 4 && nw_bars_held(-59, 4) == 3,
          "down only 3 dB below it");
    check(nw_bars_held(-90, 4) == 1 && nw_bars_held(-40, 1) == 4,
          "a large change moves at once");
}

#define W 26
#define H 14

static void test_picture(void)
{
    unsigned char planes[2 * H * 4];
    int i, x, y, x0, y0, x1, y1, px0, px1 = -2;

    for (i = 0; i < 4; i++)
    {
        nw_bar_rect(i, W, H, &x0, &y0, &x1, &y1);
        check(x0 > px1 + 1 && x1 < W && y1 == H - 1 && y0 >= 0 && x1 > x0 + 1,
              "bars apart, inside, standing on the bottom row");
        px0 = x0;
        px1 = x1;
        (void)px0;
    }
    nw_bar_rect(0, W, H, &x0, &y0, &x1, &y1);
    check(y0 > 0, "the first bar is the shortest");
    nw_bar_rect(3, W, H, &x0, &y0, &x1, &y1);
    check(y0 == 0, "the fourth bar is full height");

    /* Two bars: the first two filled, the others hollow. */
    for (i = 0; i < 4; i++)
    {
        nw_bar_rect(i, W, H, &x0, &y0, &x1, &y1);
        check(nw_icon_pen(NW_ICON_CONNECTED, 2, W, H, x0, y1) == 1, "outline");
        check(nw_icon_pen(NW_ICON_CONNECTED, 2, W, H, x0 + 1, y1 - 1) ==
              (i < 2 ? 3 : 0), "filled up to the bar count");
        check(nw_icon_pen(NW_ICON_JOINING, 0, W, H, x0 + 1, y1 - 1) == 0,
              "joining is hollow");
    }
    check(nw_icon_pen(NW_ICON_CONNECTED, 4, W, H, 0, 0) == 0, "space above bars");

    /* Off: dotted outline. */
    nw_bar_rect(0, W, H, &x0, &y0, &x1, &y1);
    check(nw_icon_pen(NW_ICON_OFF, 0, W, H, x0, y1) !=
          nw_icon_pen(NW_ICON_OFF, 0, W, H, x0 + 1, y1), "off outline dotted");

    /* No device: both diagonals reach the corners. */
    check(nw_icon_pen(NW_ICON_NODEV, 0, W, H, 0, 0) == 1 &&
          nw_icon_pen(NW_ICON_NODEV, 0, W, H, W - 1, H - 1) == 1 &&
          nw_icon_pen(NW_ICON_NODEV, 0, W, H, 0, H - 1) == 1 &&
          nw_icon_pen(NW_ICON_NODEV, 0, W, H, W - 1, 0) == 1, "crossed out");

    /* Error: the tallest bar is a '!', its gap two rows from the bottom. */
    nw_bar_rect(3, W, H, &x0, &y0, &x1, &y1);
    x = (x0 + x1) / 2;
    check(nw_icon_pen(NW_ICON_ERROR, 0, W, H, x, y0) == 1 &&
          nw_icon_pen(NW_ICON_ERROR, 0, W, H, x, y1 - 2) == 0 &&
          nw_icon_pen(NW_ICON_ERROR, 0, W, H, x, y1) == 1 &&
          nw_icon_pen(NW_ICON_ERROR, 0, W, H, x0, y1) == 0, "exclamation mark");

    /* Planes: pen 1 in plane 0, pen 3 in both, MSB first, 2-byte rows. */
    nw_icon_render(NW_ICON_CONNECTED, 4, W, H, planes);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            int pen = nw_icon_pen(NW_ICON_CONNECTED, 4, W, H, x, y);
            int b0 = (planes[y * 4 + x / 8] >> (7 - x % 8)) & 1;
            int b1 = (planes[H * 4 + y * 4 + x / 8] >> (7 - x % 8)) & 1;
            if (b0 != (pen & 1) || b1 != ((pen >> 1) & 1))
            {
                check(0, "plane bits match the pens");
                return;
            }
        }
    for (y = 0; y < H; y++)
        check((planes[y * 4 + 3] & 0x3f) == 0 && (planes[H * 4 + y * 4 + 3] & 0x3f) == 0,
              "padding bits past the width are clear");

    /* Below 13 x 4 the bars cannot be apart: nothing is drawn. */
    memset(planes, 0xff, sizeof(planes));
    nw_icon_render(NW_ICON_CONNECTED, 4, 12, 4, planes);
    for (y = 0, x = 0; y < 2 * 4 * 2; y++) x |= planes[y];
    check(x == 0, "too small: cleared, nothing drawn");
}

int main(void)
{
    test_security();
    test_bars();
    test_scan();
    test_picture();
    if (failures == 0) printf("netwireless sig: all passed\n");
    return failures == 0 ? 0 : 1;
}
