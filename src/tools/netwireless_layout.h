/*
 * NetWirelessPrefs: its three windows laid out from the gadget font's
 * metrics, the way NetPrefs lays itself out (netprefs_layout.c), and the
 * hidden passphrase's edit arithmetic.  No AmigaOS calls, so
 * test/test_netwireless_layout.c runs it on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETWIRELESS_LAYOUT_H
#define AMINETXDUO_NETWIRELESS_LAYOUT_H

enum { NW_WIN_MAIN, NW_WIN_JOIN, NW_WIN_KNOWN, NW_WIN_COUNT };

/* Every gadget, by window. */
enum
{
    NW_M_NETWORK, NW_M_SIGNAL, NW_M_LIST, NW_M_SCAN, NW_M_JOIN, NW_M_KNOWN,
    NW_M_DISCONNECT,
    NW_J_SSID, NW_J_SECURITY, NW_J_PASS, NW_J_SHOW, NW_J_HIDDEN, NW_J_STATUS,
    NW_J_JOIN, NW_J_CANCEL,
    NW_K_LIST, NW_K_UP, NW_K_DOWN, NW_K_REMOVE, NW_K_SAVE, NW_K_USE,
    NW_K_CANCEL,
    NW_L_COUNT
};

enum { NW_PLACE_NONE, NW_PLACE_LEFT, NW_PLACE_RIGHT, NW_PLACE_ABOVE, NW_PLACE_IN };

typedef struct NwItem
{
    const char   *label;
    unsigned char place;
    unsigned char window;
} NwItem;

extern const NwItem nw_items[NW_L_COUNT];
extern const char *const nw_security_choices[];   /* the join's cycle */

#define NW_LIST_ROWS    6

typedef int (*NwMeasure)(void *ctx, const char *text, int len);

typedef struct NwFont
{
    int       height;
    NwMeasure measure;
    void     *ctx;
} NwFont;

typedef struct NwBox
{
    int x, y, w, h;
} NwBox;

typedef struct NwLayout
{
    NwBox box[NW_L_COUNT];
    NwBox label[NW_L_COUNT];        /* where the label lands; w 0 = none */
    int   inner_w[NW_WIN_COUNT];
    int   inner_h[NW_WIN_COUNT];
    int   bars_w;                   /* the bar picture in a list row      */
    int   level_x;                  /* "-61 dBm" from the row's left      */
    int   lock_x;                   /* the padlock, likewise              */
} NwLayout;

/* Lays the three windows out for an interior of at most max_w by max_h.
   Returns 1 when all three fit. */
int nw_layout(const NwFont *font, int max_w, int max_h, NwLayout *out);

/* A masked string gadget shows '*'; the text typed is kept here.  One edit
   of the gadget, after it happened: op, the cursor after it, the gadget's
   length after it, and the character typed.  Returns 1 when real follows
   the edit, 0 when the edit is to be refused. */
enum { NW_EDIT_INSERT, NW_EDIT_REPLACE, NW_EDIT_OTHER };
int nw_secret_edit(char *real, int *len, int cap, int op, int pos_after,
                   int len_after, char code);

#endif
