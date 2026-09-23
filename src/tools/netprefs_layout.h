/* NetPrefs window geometry from font metrics; pure so a host test can run it. */
#ifndef AMINETXDUO_NETPREFS_LAYOUT_H
#define AMINETXDUO_NETPREFS_LAYOUT_H

/* Every gadget and drawn text in the window, in no particular order. */
enum
{
    NP_L_INTERFACE,
    NP_L_NEW,
    NP_L_REMOVE,
    NP_L_PANEL,
    NP_L_STATUS,
    NP_L_SAVE,
    NP_L_APPLY,
    NP_L_LIVE_STATUS,
    NP_L_LIVE_ACTION,
    NP_L_CLOSE,
    NP_L_NAME,
    NP_L_ID,
    NP_L_PRIORITY,
    NP_L_STATE,
    NP_L_BOOT,
    NP_L_MDNS,
    NP_L_IPV4,
    NP_L_ADDRESS,
    NP_L_NETMASK,
    NP_L_GATEWAY,
    NP_L_IPV6,
    NP_L_ADDRESS6_1,
    NP_L_ADDRESS6_2,
    NP_L_GATEWAY6,
    NP_L_DEVICE,
    NP_L_UNIT,
    NP_L_CARD,
    NP_L_HWADDRESS,
    NP_L_DOWN_OFFLINE,
    NP_L_INIT_DELAY,
    NP_L_PROMISCUOUS,
    NP_L_MTU,
    NP_L_RXBUFFER,
    NP_L_IPREQUESTS,
    NP_L_ARPREQUESTS,
    NP_L_WRITEREQUESTS,
    NP_L_NOTE,                      /* "0 = automatic", drawn with Text() */
    NP_L_COUNT
};

/* Pages; NP_PAGE_COMMON items are always attached. */
enum
{
    NP_PAGE_GENERAL,
    NP_PAGE_IPV4,
    NP_PAGE_IPV6,
    NP_PAGE_DEVICE,
    NP_PAGE_TUNING,
    NP_PAGE_COMMON
};

/* Where the label goes, as GadTools PLACETEXT_* would put it. */
enum
{
    NP_PLACE_NONE,
    NP_PLACE_LEFT,
    NP_PLACE_RIGHT,
    NP_PLACE_ABOVE,
    NP_PLACE_IN
};

typedef struct NpItem
{
    const char   *label;            /* NULL: no label */
    unsigned char place;
    unsigned char page;
} NpItem;

extern const NpItem np_items[NP_L_COUNT];

/* Cycle and text-display contents, measured for their gadgets' widths. */
extern const char *const np_panel_labels[];
extern const char *const np_ipv4_labels[];
extern const char *const np_ipv6_labels[];
extern const char *const np_live_labels[];
extern const char *const np_action_labels[];

typedef int (*NpMeasure)(void *ctx, const char *text, int len);

typedef struct NpFont
{
    int       height;               /* tf_YSize */
    int       baseline;             /* tf_Baseline */
    NpMeasure measure;              /* TextLength() */
    void     *ctx;
} NpFont;

typedef struct NpBox
{
    int x, y, w, h;
} NpBox;

typedef struct NpLayout
{
    NpBox box[NP_L_COUNT];          /* NP_L_NOTE: text cell, y is its top */
    NpBox label[NP_L_COUNT];        /* where the label text lands; w 0 = none */
    NpBox bevel;                    /* the recessed page frame */
    int   sep_x, sep_top, sep_bottom;
    int   inner_w, inner_h;         /* GZZ window interior */
} NpLayout;

/* Lays out for an interior of at most max_w by max_h.  Returns 1 if it fits;
   0 leaves *out holding the too-large layout. */
int np_layout(const NpFont *font, int max_w, int max_h, NpLayout *out);

#endif
