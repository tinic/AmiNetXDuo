/*
 * NetSwitch's window laid out from the gadget font's metrics, the way NetPrefs
 * (netprefs_layout.c) and NetWirelessPrefs lay themselves out.  No AmigaOS
 * calls, so test/test_netswitch_layout.c runs it on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSWITCH_LAYOUT_H
#define AMINETXDUO_NETSWITCH_LAYOUT_H

/* Every box in the one window. */
enum
{
    NS_L_LEGEND,                    /* the three states, over the list */
    NS_L_LIST,                      /* the interface rows */
    NS_L_PANEL,                     /* the selected interface's detail */
    NS_L_ACTION,                    /* Go Online / Go Offline */
    NS_L_ALL,                       /* Take all offline */
    NS_L_HINT,                      /* "Return / dbl-click toggles" */
    NS_L_STATUS,                    /* the counts, under everything */
    NS_L_COUNT
};

typedef int (*NsMeasure)(void *ctx, const char *text, int len);

typedef struct NsFont
{
    int       height;               /* tf_YSize */
    int       baseline;             /* tf_Baseline */
    NsMeasure measure;              /* TextLength() */
    void     *ctx;
} NsFont;

typedef struct NsBox
{
    int x, y, w, h;
} NsBox;

typedef struct NsLayout
{
    NsBox box[NS_L_COUNT];
    int   inner_w, inner_h;         /* window interior */

    int   row_h;                    /* one list row */
    int   rows;                     /* rows shown at once; 0 = empty list */
    int   scroll;                   /* 1 when more interfaces are waiting */
    int   cell_pad;                 /* frame to the first row and column */
    int   strip_w;                  /* scroller strip, 0 when not scrolling */

    /* The row's columns, as offsets from the list box's left edge.  Row i is
       drawn at box[NS_L_LIST].y + cell_pad + row_h * i, the glyph first. */
    int   glyph_x, glyph_w;
    int   name_x, name_chars;       /* elide a longer name */
    int   device_x, device_chars;   /* 18: "anxzz9000.device 0" is the real one */
    int   address_x, address_chars; /* 15: a dotted quad, a v6 elides */
    int   panel_lead;               /* between the panel's lines */

    /* The columns as the font measures them, for ns_fit(): a character count
       bounds a monospace font and nothing else, so a cell fitted by count can
       be wider than its column and reach into the next one. */
    int   name_px, device_px, address_px;

    /* The panel's text column: the panel box is wider, this is what fits. */
    int   panel_w;
} NsLayout;

/*
 * Lays the window out for an interior of at most max_w by max_h, showing
 * count interfaces.  Returns 1 when what the window must show fits; 0 leaves
 * *out holding the too-large layout, so the caller can fall back to topaz 8
 * the way NetPrefs does.
 */
int ns_layout(const NsFont *font, int max_w, int max_h, unsigned count,
              NsLayout *out);

/*
 * The first row the list shows so that the selected one is on the screen.
 * The list scrolls only when it has to, and never past its last row.
 */
int ns_first_row(const NsLayout *l, unsigned count, unsigned selected);

#endif
