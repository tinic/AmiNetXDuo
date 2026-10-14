/*
 * NetMeter in a Shell window: the frame drawn with console.device control
 * sequences (console.doc, CMD_WRITE) and the console's input read back -- key
 * presses and the window bounds report.  No AmigaOS calls, so
 * src/tools/test/test_netmeter_fmt.c runs it on the build machine.
 *
 * The colours are console pens 0-3 only, which every Workbench screen has:
 * 3 (blue on the default palette) is receive, 1 (black) is send.  Each is
 * also labelled RX or TX, and the graph puts receive above the middle line
 * and send below it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETMETER_TUI_H
#define NETMETER_TUI_H

#include <stdint.h>

/* Seconds of history kept per interface: the graph's widest. */
#define NM_TUI_HIST     128u

/* The frame, as text and control sequences.  `flush` takes a full buffer
   and the rest at the end; one Write() a frame when `size` is big enough. */
typedef struct NmTuiOut
{
    char      *buf;
    uint32_t   size;
    uint32_t   used;
    void     (*flush)(void *ctx, const char *s, uint32_t n);
    void      *ctx;
    uint32_t   col;        /* visible columns written on this row          */
    uint32_t   limit;      /* text past this column is dropped             */
    uint8_t    attr;       /* the rendition last sent, NM_TUI_A_*          */
} NmTuiOut;

typedef struct NmTuiIf
{
    const char *name;
    const char *ip4;           /* "" when none                             */
    const char *ip6;
    const char *device;        /* "" when none                             */
    uint32_t    unit;
    int         up;
    uint32_t    bps;           /* link speed, 0 when unknown               */
    int         have_bytes;
    uint32_t    rx_rate, tx_rate, rx_peak, tx_peak, rx_avg, tx_avg;
    uint32_t    rx_hi, rx_lo, tx_hi, tx_lo;     /* the interface's totals  */
    uint32_t    rx_pkts, tx_pkts, errors, dropped;
    uint32_t    scale;         /* the bars' full scale                     */
    const uint32_t *rx_hist;   /* NM_TUI_HIST rates, a ring                */
    const uint32_t *tx_hist;
    uint32_t    hist_head;     /* the next slot to write: newest is one back */
    uint32_t    hist_count;
} NmTuiIf;

typedef struct NmTuiView
{
    const char    *version;     /* "47.3"                                  */
    const char    *host;        /* "" when unknown                         */
    int            running;     /* the stack answered                      */
    int            paused;
    uint32_t       session_s;
    uint32_t       n;
    const NmTuiIf *ifs;
    uint32_t       rows, cols;
} NmTuiView;

/* Rows of graph each interface gets: even, 0 when there is no room. */
uint32_t nm_tui_graph_rows(uint32_t n_if, uint32_t rows);

/* The whole frame, row 1 to `rows`, every row rewritten in place. */
void nm_tui_frame(NmTuiOut *o, const NmTuiView *v);

/* Taking the window over and handing it back: cursor, wrap, scroll, the
   resize report, the screen. */
void nm_tui_enter(NmTuiOut *o);
void nm_tui_leave(NmTuiOut *o);

/* Window Status Request: the console answers with its bounds on the input. */
void nm_tui_ask_bounds(NmTuiOut *o);

void nm_tui_flush(NmTuiOut *o);

/* "10 Mbit/s", "1 Gbit/s", "2.5 Gbit/s". */
void nm_format_bps(uint32_t bps, char *out, uint32_t outlen);

/* "0:05:09", "27:46:40". */
void nm_format_duration(uint32_t seconds, char *out, uint32_t outlen);

/* ----------------------------------------------------------- input, */

enum { NM_IN_NONE, NM_IN_KEY, NM_IN_BOUNDS, NM_IN_RESIZE };

typedef struct NmTuiEvent
{
    int       kind;
    int       key;          /* NM_IN_KEY: the character, 27 for Esc       */
    uint32_t  rows, cols;   /* NM_IN_BOUNDS                               */
} NmTuiEvent;

typedef struct NmTuiIn
{
    uint8_t   state;
    uint8_t   inter;
    uint8_t   np;
    uint32_t  p[4];
} NmTuiIn;

/* One byte of the console's input: 1 and `ev` filled when it ends one. */
int nm_tui_feed(NmTuiIn *in, unsigned char c, NmTuiEvent *ev);

/* Nothing more to read: an Esc on its own was the key. */
int nm_tui_idle(NmTuiIn *in, NmTuiEvent *ev);

/* What a run of the console's input asks for, added up; zero it first. */
typedef struct NmTuiKeys
{
    int       quit;         /* q, Esc, Ctrl-C                             */
    int       reset;        /* r                                          */
    uint32_t  pauses;       /* Space presses: odd toggles                 */
    int       bounds;       /* a bounds report came: rows and cols        */
    uint32_t  rows, cols;
    int       resized;      /* the window changed: ask for its bounds     */
} NmTuiKeys;

/* `n` bytes of input into `k`; other keys are ignored.  With `idle` the
   input is all read, and an Esc left alone is the key. */
void nm_tui_keys(NmTuiIn *in, const unsigned char *b, uint32_t n, int idle,
                 NmTuiKeys *k);

#endif /* NETMETER_TUI_H */
