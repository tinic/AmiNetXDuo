/*
 * NetWireless: the text of ENV:Sys/Wireless.prefs, which is WirelessManager's
 * own wpa_supplicant configuration, and of a Roadshow interface file.  No
 * AmigaOS calls, so test/test_netwireless_prefs.c runs it on the build host.
 *
 * Edits are made in place.  A block or line nobody asked to change is copied
 * byte for byte, comments and keys unknown here included.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETWIRELESS_PREFS_H
#define AMINETXDUO_NETWIRELESS_PREFS_H

#include <stddef.h>

#ifndef NW_SSID_MAX
#define NW_SSID_MAX     32
#endif
#define NW_PSK_HEX      64

/* One network={ } block as wpa_supplicant 0.7.3 reads it. */
typedef struct NwNetwork
{
    size_t        start;            /* first byte of the "network={" line */
    size_t        end;              /* past the "}" line and its newline  */
    unsigned char ssid[NW_SSID_MAX];
    int           ssid_len;         /* -1: no usable ssid= line           */
    long          priority;         /* 0 when absent, as wpa_supplicant   */
    int           scan_ssid;
    int           open;             /* key_mgmt=NONE and no WEP key       */
    int           wep;              /* a wep_key0..3 line                 */
    int           has_psk;
    int           disabled;
} NwNetwork;

/* The blocks of text in file order, at most max of them stored.  Returns
   the number in the file, which is more than max when some were not
   stored, or -1 for a block with no closing "}" line. */
int nw_prefs_parse(const char *text, size_t len, NwNetwork *out, int max);

/* One key of a block: value NULL removes every line of that key. */
typedef struct NwField
{
    const char *key;
    const char *value;
    int         seen;               /* used by nw_prefs_write             */
} NwField;

/* What happens to one block.  index is a block of the parse, or -1 for a new
   block appended after the last byte of the text. */
typedef struct NwBlockEdit
{
    int      index;
    int      remove;
    NwField *fields;
    int      nfields;
} NwBlockEdit;

/* The text with the edits applied.  Within an edited block the first line of
   each key is replaced in place, later lines of that key are dropped, and a
   key the block lacks is added before its "}" in the block's indentation
   and line ending.  Returns 1 and sets *newlen, or 0 when out is too small
   or an edit names a block past nnets. */
int nw_prefs_write(const char *old, size_t oldlen,
                   const NwNetwork *nets, int nnets,
                   NwBlockEdit *edits, int nedits,
                   char *out, size_t cap, size_t *newlen);

/* The order a list shows the blocks in, highest priority first and file
   order among equals: order[0..count-1] gets block indices. */
void nw_prefs_order(const NwNetwork *nets, int count, unsigned char *order);

/* The edits that store a list: order[0..shown-1] the blocks kept, top
   first, given priorities shown..1; every other block removed.  A block
   whose priority is already right is not edited.  edits, fields and the
   12-byte prio strings need count entries.  Returns the number of edits. */
int nw_prefs_order_edits(const NwNetwork *nets, int count,
                         const unsigned char *order, int shown,
                         NwBlockEdit *edits, NwField *fields, char (*prio)[12]);

/* An ssid= value: "text" when every byte is printable ASCII, else hex, both
   as wpa_supplicant reads them.  out needs 2 * NW_SSID_MAX + 3 bytes. */
void nw_prefs_ssid_value(const unsigned char *ssid, int len, char *out);

/* A DEVICE, path aside and case aside, that is a Wi-Fi driver these tools
   know: anxwifipi.device or wifipi.device (the Pi), prism2.device (Neil
   Cafferkey's, PC Card and PCI). */
int nw_wireless_driver(const char *device);

/* The DEVICE and UNIT of a Roadshow interface file: keyword=value or
   keyword value, case aside, ';' and '#' comments, quotes around a value.
   Returns 1 when DEVICE is there; unit stays as given when UNIT is not. */
int nw_iface_device(const char *text, size_t len, char *device, size_t cap,
                    unsigned long *unit);

#endif
