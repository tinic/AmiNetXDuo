/*
 * NetWireless: security, bars and the bar picture.  See the header.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_sig.h"

#define NW_IE_RSN       48
#define NW_IE_VENDOR    221
#define NW_CAP_PRIVACY  0x0010

static unsigned nw_read16le(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* The AKM list of an RSN element body, or of a WPA vendor element body after
   its OUI and type; oui is 00-0F-AC or 00-50-F2. */
static unsigned nw_akms(const unsigned char *p, size_t len,
                        const unsigned char *oui, int rsn)
{
    unsigned sec = 0, n, i;
    size_t   at = 2 + 4;            /* version, group cipher */

    /* A short element takes the defaults, and the default AKM is 802.1X. */
    if (len < at + 2) return NW_SEC_EAP;
    n = nw_read16le(p + at);
    at += 2 + 4 * (size_t)n;
    if (len < at + 2) return NW_SEC_EAP;
    n = nw_read16le(p + at);
    at += 2;
    for (i = 0; i < n && at + 4 <= len; i++, at += 4)
    {
        const unsigned char *s = p + at;

        if (s[0] != oui[0] || s[1] != oui[1] || s[2] != oui[2]) continue;
        switch (s[3])
        {
            case 1: case 5: sec |= NW_SEC_EAP; break;
            case 2: case 6: sec |= rsn ? NW_SEC_WPA2_PSK : NW_SEC_WPA_PSK; break;
            case 8: if (rsn) sec |= NW_SEC_SAE; break;
            default: break;
        }
    }
    return sec;
}

unsigned nw_security(const unsigned char *ie, size_t len, unsigned capabilities)
{
    static const unsigned char rsn_oui[3] = { 0x00, 0x0f, 0xac };
    static const unsigned char wpa_oui[3] = { 0x00, 0x50, 0xf2 };
    unsigned sec = 0;
    int      seen = 0;
    size_t   at = 0;

    while (ie != NULL && at + 2 <= len)
    {
        unsigned id = ie[at], n = ie[at + 1];
        const unsigned char *body = ie + at + 2;

        if (at + 2 + n > len) break;
        if (id == NW_IE_RSN)
        {
            sec |= nw_akms(body, n, rsn_oui, 1);
            seen = 1;
        }
        else if (id == NW_IE_VENDOR && n >= 4 && body[0] == wpa_oui[0] &&
                 body[1] == wpa_oui[1] && body[2] == wpa_oui[2] && body[3] == 1)
        {
            sec |= nw_akms(body + 4, n - 4, wpa_oui, 0);
            seen = 1;
        }
        at += 2 + n;
    }
    if (!seen && (capabilities & NW_CAP_PRIVACY)) sec |= NW_SEC_WEP;
    return sec;
}

const char *nw_security_label(unsigned sec)
{
    if (sec & NW_SEC_SAE)
        return (sec & NW_SEC_WPA2_PSK) ? "WPA2/WPA3" : "WPA3";
    if ((sec & NW_SEC_WPA2_PSK) && (sec & NW_SEC_WPA_PSK)) return "WPA/WPA2";
    if (sec & NW_SEC_WPA2_PSK) return "WPA2";
    if (sec & NW_SEC_WPA_PSK) return "WPA";
    if (sec & NW_SEC_EAP) return "Enterprise";
    if (sec & NW_SEC_WEP) return "WEP";
    return "Open";
}

int nw_security_joinable(unsigned sec)
{
    return sec == 0 || (sec & (NW_SEC_WPA_PSK | NW_SEC_WPA2_PSK)) != 0;
}

int nw_bars(long dbm)
{
    if (dbm >= -55) return 4;
    if (dbm >= -66) return 3;
    if (dbm >= -77) return 2;
    return 1;
}

void nw_bar_rect(int i, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    int gap = w / 13 > 0 ? w / 13 : 1;
    int bw  = (w - 3 * gap) / 4;
    int bh  = h * (i + 1) / 4;

    if (bw < 1) bw = 1;
    if (bh < 2) bh = 2;
    *x0 = i * (bw + gap);
    *x1 = *x0 + bw - 1;
    *y0 = h - bh;
    *y1 = h - 1;
}

int nw_icon_pen(int state, int bars, int w, int h, int x, int y)
{
    int i;

    if (state == NW_ICON_NODEV && w > 1 && h > 1)
    {
        /* Both diagonals, one pixel each. */
        int d = (x * (h - 1) + (w - 1) / 2) / (w - 1);
        if (y == d || y == h - 1 - d) return 1;
    }

    for (i = 0; i < 4; i++)
    {
        int x0, y0, x1, y1;

        nw_bar_rect(i, w, h, &x0, &y0, &x1, &y1);
        if (x < x0 || x > x1 || y < y0 || y > y1) continue;

        if (state == NW_ICON_ERROR && i == 3)
        {
            int cx = (x0 + x1) / 2;
            if (x != cx && x != cx + 1) return 0;
            return (y == y1 - 2) ? 0 : 1;
        }
        if (x == x0 || x == x1 || y == y0 || y == y1)
        {
            if (state == NW_ICON_OFF) return ((x + y) & 1) == 0 ? 1 : 0;
            return 1;
        }
        return (state == NW_ICON_CONNECTED && i < bars) ? 3 : 0;
    }
    return 0;
}

void nw_icon_render(int state, int bars, int w, int h, unsigned char *planes)
{
    int    row_bytes = ((w + 15) / 16) * 2;
    size_t plane = (size_t)row_bytes * (size_t)h;
    int    x, y;
    size_t k;

    for (k = 0; k < 2 * plane; k++) planes[k] = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int           pen = nw_icon_pen(state, bars, w, h, x, y);
            size_t        at = (size_t)y * (size_t)row_bytes + (size_t)(x >> 3);
            unsigned char bit = (unsigned char)(0x80 >> (x & 7));

            if (pen & 1) planes[at] |= bit;
            if (pen & 2) planes[plane + at] |= bit;
        }
}
