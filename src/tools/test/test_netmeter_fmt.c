/*
 * NetMeter's arithmetic (netmeter_fmt.c): the rate and total text, the delta
 * of a count kept in two 32-bit halves across its carry and a reset, and the
 * bar scale; and the Shell window frame (netmeter_tui.c), read back through
 * a small model of console.device that keeps each cell's character and pens.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netmeter_fmt.h"
#include "netmeter_tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
static unsigned long failures;

static void expect_text(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0)
    {
        failures++;
        printf("  FAIL %s: \"%s\", want \"%s\"\n", what, got, want);
    }
}

static void expect(int cond, const char *what)
{
    checks++;
    if (!cond)
    {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static void t_rates(void)
{
    char s[24];

    nm_format_rate(0, s, sizeof(s));            expect_text(s, "0 B/s", "zero");
    nm_format_rate(512, s, sizeof(s));          expect_text(s, "512 B/s", "bytes");
    nm_format_rate(1023, s, sizeof(s));         expect_text(s, "1023 B/s", "under a KB");
    nm_format_rate(1536, s, sizeof(s));         expect_text(s, "1.50 KB/s", "two decimals");
    nm_format_rate(12595, s, sizeof(s));        expect_text(s, "12.2 KB/s", "one decimal");
    nm_format_rate(262144, s, sizeof(s));       expect_text(s, "256 KB/s", "no decimal");
    nm_format_rate(2841641, s, sizeof(s));      expect_text(s, "2.70 MB/s", "a Zorro III card");
    nm_format_rate(0xFFFFFFFFu, s, sizeof(s));  expect_text(s, "3.99 GB/s", "the largest");
    nm_format_rate(2841641, s, 5);              expect_text(s, "2.70", "cut to the buffer");
}

static void t_totals(void)
{
    char s[24];

    nm_format_total(0, 1, s, sizeof(s));        expect_text(s, "1 byte", "one byte");
    nm_format_total(0, 312, s, sizeof(s));      expect_text(s, "312 bytes", "bytes");
    nm_format_total(0, 1536, s, sizeof(s));     expect_text(s, "1.50 KB", "KB");
    nm_format_total(0, 12898509, s, sizeof(s)); expect_text(s, "12.3 MB", "MB");
    /* 0x1_0400_0000 = 4.0625 GB: the high half has to count. */
    nm_format_total(1, 0x04000000u, s, sizeof(s));
    expect_text(s, "4.06 GB", "across 4 GB");
    nm_format_total(0x400, 0, s, sizeof(s));    expect_text(s, "4.00 TB", "TB");
}

static void t_delta(void)
{
    expect(nm_delta(0, 5000, 0, 1000) == 4000, "a plain delta");
    expect(nm_delta(1, 100, 0, 0xFFFFFF00u) == 356, "across the carry");
    expect(nm_delta(0, 10, 0, 5000) == 0, "a reset reads zero");
    expect(nm_delta(0, 10, 3, 0) == 0, "a reset of the high half too");
    expect(nm_delta(5, 0, 0, 0) == 0xFFFFFFFFu, "too far saturates");
}

static void t_rate(void)
{
    expect(nm_rate(2700000, 1000) == 2700000, "a second");
    expect(nm_rate(1350000, 500) == 2700000, "half a second");
    expect(nm_rate(1000, 0) == 0, "no time");
    expect(nm_rate(0xFFFFFFF0u, 1000) == 4294967000u, "large without overflow");
}

static void t_scale(void)
{
    uint32_t s = NM_SCALE_FLOOR;

    expect(nm_scale_next(s, 0, 0) == NM_SCALE_FLOOR, "the floor holds");
    s = nm_scale_next(s, 1000000, 0);
    expect(s == 1250000, "a peak raises it with a quarter headroom");
    s = nm_scale_next(s, 0, 0);
    expect(s == 1250000 - 1250000 / 16, "and it falls by a sixteenth");
    expect(nm_scale_next(0xF0000000u, 0xF0000000u, 0) == 0xFFFFFFFFu,
           "the headroom saturates");
}

static void t_bar(void)
{
    expect(nm_bar_fill(0, 1000, 100) == 0, "nothing");
    expect(nm_bar_fill(500, 1000, 100) == 50, "half");
    expect(nm_bar_fill(2000, 1000, 100) == 100, "over the scale is full");
    /* Large rates lose low bits to stay in 32-bit arithmetic: a pixel. */
    expect(nm_bar_fill(2700000, 5400000, 120) >= 59 &&
           nm_bar_fill(2700000, 5400000, 120) <= 60, "large rates");
    expect(nm_bar_fill(5, 0, 100) == 0, "no scale");
}

static void t_average(void)
{
    expect(nm_average(0, 5000, 0) == 0, "average of no time");
    expect(nm_average(0, 2700000, 1000) == 2700000, "average below 4 GB");
    /* Samples 1000 ms and a late 2500 ms apart: 1000 + 3000 bytes over
       3500 ms, not the two rates over two seconds. */
    expect(nm_average(0, 4000, 3500) == 1142, "unequal intervals");
    expect(nm_average(0, 1500, 1500) == 1000, "a fractional second");
    expect(nm_average(0, 999, 1999) == 499, "rounded down");
    /* 8 GiB over 4000 s, exactly. */
    expect(nm_average(2, 0, 4000000) == 2147483, "average across the carry");
    /* 16 GiB over an hour, and 1 TiB over 49.7 days (ULONG milliseconds). */
    expect(nm_average(4, 0, 3600000) == 4772185, "16 GiB over an hour");
    expect(nm_average(256, 0, 0xFFFFFFFFu) == 256000, "1 TiB over 49.7 days");
    /* 4 GB + 1 GB over 1 ms is past 32 bits; so is anything that large. */
    expect(nm_average(1, 0x40000000u, 1) == 0xFFFFFFFFu, "past 32 bits saturates");
    expect(nm_average(0xFFFFFFFFu, 0, 0xFFFFFFFFu) == 0xFFFFFFFFu, "past 2^64/1000 saturates");
    /* The edge: the largest total whose average still fits, exact. */
    expect(nm_average(0x418937u, 0x4B439581u, 0xFFFFFFFFu) == 4294967294u,
           "the largest average, exact");
}

static void t_text(void)
{
    char s[24];

    nm_format_bps(10000000u, s, sizeof(s));     expect_text(s, "10 Mbit/s", "10 Mbit");
    nm_format_bps(1000000000u, s, sizeof(s));   expect_text(s, "1 Gbit/s", "gigabit");
    nm_format_bps(2500000000u, s, sizeof(s));   expect_text(s, "2.5 Gbit/s", "2.5 Gbit");
    nm_format_bps(5500000u, s, sizeof(s));      expect_text(s, "5.5 Mbit/s", "802.11b");
    nm_format_bps(9600u, s, sizeof(s));         expect_text(s, "9.6 kbit/s", "kbit");
    nm_format_duration(309, s, sizeof(s));      expect_text(s, "0:05:09", "minutes");
    nm_format_duration(100000, s, sizeof(s));   expect_text(s, "27:46:40", "a day");
}

/* ------------------------------------------- a console.device model, */

#define SCR_ROWS 64
#define SCR_COLS 140

typedef struct
{
    char          ch[SCR_ROWS][SCR_COLS];
    unsigned char fg[SCR_ROWS][SCR_COLS], bg[SCR_ROWS][SCR_COLS];
    unsigned      rows, cols, r, c;
    unsigned      cur_fg, cur_bg, rev, bold;
    unsigned      outside;      /* characters past the window              */
    unsigned      sgr_count;
    int           cursor_off, wrap_off, scroll_off, resize_on, cleared;
    unsigned      bytes;
} Screen;

static Screen scr;
static char   frame[65536];
static unsigned frame_len;

static void scr_reset(unsigned rows, unsigned cols)
{
    memset(&scr, 0, sizeof(scr));
    memset(scr.ch, ' ', sizeof(scr.ch));
    memset(scr.fg, 1, sizeof(scr.fg));
    scr.rows = rows; scr.cols = cols; scr.cur_fg = 1;
}

static void scr_sgr(const unsigned *p, unsigned np)
{
    unsigned i;

    scr.sgr_count++;
    if (np == 0) { scr.cur_fg = 1; scr.cur_bg = 0; scr.rev = scr.bold = 0; }
    for (i = 0; i < np; i++)
    {
        if (p[i] == 0)      { scr.cur_fg = 1; scr.cur_bg = 0; scr.rev = scr.bold = 0; }
        else if (p[i] == 1) scr.bold = 1;
        else if (p[i] == 7) scr.rev = 1;
        else if (p[i] >= 30 && p[i] <= 37) scr.cur_fg = p[i] - 30;
        else if (p[i] >= 40 && p[i] <= 47) scr.cur_bg = p[i] - 40;
    }
}

static void scr_write(const char *s, unsigned n)
{
    unsigned i = 0, k;

    while (i < n)
    {
        unsigned char c = (unsigned char)s[i++];

        if (c == 0x9b)
        {
            unsigned p[8] = { 0 }, np = 0, any = 0;
            char     pre = 0, inter = 0, fin = 0;

            while (i < n)
            {
                c = (unsigned char)s[i++];
                if (c >= '0' && c <= '9') { p[np] = p[np] * 10 + (c - '0'); any = 1; }
                else if (c == ';') { if (np < 7) np++; }
                else if (c == '?' || c == '>') pre = (char)c;
                else if (c == ' ') inter = ' ';
                else { fin = (char)c; break; }
            }
            if (any || np) np++;
            if (fin == 'H') { scr.r = (p[0] ? p[0] : 1) - 1; scr.c = (np > 1 && p[1] ? p[1] : 1) - 1; }
            else if (fin == 'K') { for (k = scr.c; k < scr.cols && scr.r < scr.rows; k++) { scr.ch[scr.r][k] = ' '; scr.bg[scr.r][k] = 0; } }
            else if (fin == 'm') scr_sgr(p, np);
            else if (fin == 'p' && inter) scr.cursor_off = (np && p[0] == 0);
            else if (fin == 'l' && pre == '?' && p[0] == 7) scr.wrap_off = 1;
            else if (fin == 'h' && pre == '?' && p[0] == 7) scr.wrap_off = 0;
            else if (fin == 'l' && pre == '>' && p[0] == 1) scr.scroll_off = 1;
            else if (fin == 'h' && pre == '>' && p[0] == 1) scr.scroll_off = 0;
            else if (fin == '{' && p[0] == 12) scr.resize_on = 1;
            else if (fin == '}' && p[0] == 12) scr.resize_on = 0;
            continue;
        }
        if (c == '\f')
        {
            memset(scr.ch, ' ', sizeof(scr.ch));
            memset(scr.bg, 0, sizeof(scr.bg));
            scr.r = scr.c = 0; scr.cleared++;
            continue;
        }
        if (scr.r >= scr.rows || scr.c >= scr.cols) { scr.outside++; continue; }
        scr.ch[scr.r][scr.c] = (char)c;
        scr.fg[scr.r][scr.c] = (unsigned char)(scr.rev ? scr.cur_bg : scr.cur_fg);
        scr.bg[scr.r][scr.c] = (unsigned char)(scr.rev ? scr.cur_fg : scr.cur_bg);
        scr.c++;
    }
}

static void to_screen(void *ctx, const char *s, uint32_t n)
{
    (void)ctx;
    scr_write(s, n);
    if (frame_len + n <= sizeof(frame)) { memcpy(frame + frame_len, s, n); frame_len += n; }
    scr.bytes += n;
}

/* Row `r` (1-based) as text, trailing blanks kept. */
static const char *scr_row(unsigned r)
{
    static char line[SCR_COLS + 1];

    memcpy(line, scr.ch[r - 1], scr.cols);
    line[scr.cols] = '\0';
    return line;
}

static int row_has(unsigned r, const char *what)
{
    return strstr(scr_row(r), what) != NULL;
}

static uint32_t rx_hist[NM_TUI_HIST], tx_hist[NM_TUI_HIST];

static void fill_if(NmTuiIf *f, const char *name)
{
    unsigned k;

    memset(f, 0, sizeof(*f));
    f->name = name; f->ip4 = "192.168.1.50"; f->ip6 = "fe80::21b:21ff:fe3c:4d5e";
    f->device = "anxnet.device"; f->unit = 0; f->up = 1; f->bps = 10000000u;
    f->have_bytes = 1;
    f->rx_rate = 2831155; f->tx_rate = 41000;
    f->rx_peak = 2841641; f->tx_peak = 52000;
    f->rx_avg = 1258291; f->tx_avg = 20480;
    f->rx_hi = 0; f->rx_lo = 1288490188u; f->tx_hi = 0; f->tx_lo = 54840934u;
    f->rx_pkts = 912345; f->tx_pkts = 456789;
    f->scale = 3500000;
    for (k = 0; k < NM_TUI_HIST; k++)
    {
        rx_hist[k] = (k * 37u % 29u) * 100000u;
        tx_hist[k] = (k * 11u % 7u) * 4000u;
    }
    f->rx_hist = rx_hist; f->tx_hist = tx_hist;
    f->hist_head = 5; f->hist_count = 90;
}

static void render(const NmTuiView *v, uint32_t bufsize)
{
    static char buf[8192];
    NmTuiOut    o;

    scr_reset(v->rows, v->cols);
    frame_len = 0;
    memset(&o, 0, sizeof(o));
    o.buf = buf; o.size = bufsize; o.flush = to_screen;
    nm_tui_frame(&o, v);
}

static void dump(const char *title)
{
    unsigned r;

    if (getenv("NETMETER_TUI_DUMP") == NULL)
        return;
    printf("--- %s (%ux%u, %u bytes)\n", title, scr.cols, scr.rows, scr.bytes);
    for (r = 1; r <= scr.rows; r++)
    {
        unsigned c;
        char     line[SCR_COLS + 1];

        for (c = 0; c < scr.cols; c++)
            line[c] = (scr.ch[r - 1][c] == ' ' && scr.bg[r - 1][c] != 0)
                      ? (char)('0' + scr.bg[r - 1][c]) : scr.ch[r - 1][c];
        line[c] = '\0';
        printf("|%s|\n", line);
    }
}

static void t_frame_80x25(void)
{
    NmTuiIf   f;
    NmTuiView v;
    unsigned  r, c, rx_cells = 0, tx_cells = 0;
    char      big[65536];
    unsigned  big_len;

    fill_if(&f, "eth0");
    memset(&v, 0, sizeof(v));
    v.version = "47.3"; v.host = "amiga.local"; v.running = 1;
    v.session_s = 309; v.n = 1; v.ifs = &f; v.rows = 25; v.cols = 80;
    render(&v, 8192);
    dump("80x25");

    expect(scr.outside == 0, "80x25: nothing past the window");
    expect(strncmp(scr_row(1), " NetMeter 47.3  amiga.local", 27) == 0, "80x25: the title");
    expect(strncmp(scr_row(1) + 64, "Session 0:05:09 ", 16) == 0, "80x25: the session at the right");
    expect(scr.bg[0][0] == 3 && scr.fg[0][0] == 2, "80x25: title bar white on blue");
    expect(strncmp(scr_row(25), " Q Quit  R Reset  Space Pause", 29) == 0, "80x25: the keys");
    expect(scr.bg[24][78] == 3 && scr.bg[24][79] == 0, "80x25: the bottom right corner left alone");
    expect(row_has(3, " eth0 ") && scr.bg[2][2] == 2, "80x25: the name tag");
    expect(row_has(3, "Up  10 Mbit/s  192.168.1.50  anxnet.device unit 0"), "80x25: the link");
    expect(row_has(4, "RX  2.69 MB/s") && row_has(4, "Peak 2.70 MB/s") &&
           row_has(4, "Avg 1.19 MB/s"), "80x25: receive");
    expect(row_has(5, "TX  40.0 KB/s") && row_has(5, "Peak 50.7 KB/s"), "80x25: send");
    expect(row_has(6, "In 1.19 GB  912345 pkts   Out 52.2 MB  456789 pkts   Errors 0"),
           "80x25: totals");
    expect(nm_tui_graph_rows(1, 25) == 18, "80x25: an 18-row graph");
    expect(row_has(7, "3.33 MB/s") && row_has(24, "29.2 KB/s"), "80x25: the graphs' scales");
    expect(strncmp(scr_row(15) + 6, " RX ", 4) == 0 && scr.bg[14][7] == 3 &&
           strncmp(scr_row(16) + 6, " TX ", 4) == 0 && scr.bg[15][7] == 1, "80x25: the graph's halves");
    for (r = 6; r < 24; r++)
        for (c = 12; c < 80; c++)
        {
            if (scr.bg[r][c] == 3) rx_cells += (r < 15);
            if (scr.bg[r][c] == 1) tx_cells += (r >= 15);
            expect(!(scr.bg[r][c] == 3 && r >= 15) && !(scr.bg[r][c] == 1 && r < 15),
                   "80x25: receive above, send below");
        }
    expect(rx_cells > 50 && tx_cells > 10, "80x25: the graph has both");
    /* The bar: 2.70 of 3.34 MB/s in 30 cells. */
    for (c = 16, r = 0; c < 80; c++) r += scr.bg[3][c] == 3;
    expect(r == nm_bar_fill(2831155, 3500000, 30), "80x25: the receive bar");
    expect(scr.bytes < 6000, "80x25: a frame under 6 KB");

    /* A small buffer flushed many times draws the same bytes. */
    memcpy(big, frame, frame_len); big_len = frame_len;
    render(&v, 64);
    expect(frame_len == big_len && memcmp(big, frame, big_len) == 0,
           "a small buffer writes the same frame");
}

static void t_frame_small(void)
{
    NmTuiIf   f[2];
    NmTuiView v;
    unsigned  r;

    fill_if(&f[0], "eth0");
    fill_if(&f[1], "wlan0");
    f[1].up = 0; f[1].errors = 3;
    memset(&v, 0, sizeof(v));
    v.version = "47.3"; v.host = "amiga.local"; v.running = 1; v.paused = 1;
    v.session_s = 309; v.n = 1; v.ifs = f; v.rows = 12; v.cols = 40;
    render(&v, 8192);
    dump("40x12");
    expect(scr.outside == 0, "40x12: nothing past the window");
    expect(!row_has(4, "Peak") && row_has(4, "RX  2.69 MB/s"), "40x12: rate and bar only");
    expect(row_has(3, "10 Mbit/s") && !row_has(3, "fe80"), "40x12: whole fields only");
    expect(strncmp(scr_row(12), " Q Quit  R Reset  Space Resume", 30) == 0, "40x12: the keys");

    v.n = 2; v.rows = 25; v.cols = 80;
    render(&v, 8192);
    dump("80x25, two interfaces");
    expect(nm_tui_graph_rows(2, 25) == 6, "two interfaces: 6 rows each");
    expect(row_has(14, " wlan0 ") && row_has(14, " Down "), "two interfaces: the second");
    expect(row_has(17, "Errors 3"), "two interfaces: its errors");
    expect(row_has(1, "PAUSED"), "paused in the title");

    v.rows = 4; v.cols = 20;
    render(&v, 8192);
    dump("20x4");
    expect(scr.outside == 0, "20x4: nothing past the window");
    expect(row_has(4, " Q Quit"), "20x4: the keys");

    v.running = 0; v.rows = 25; v.cols = 80;
    render(&v, 8192);
    expect(row_has(3, "The network is not running."), "no stack");
    for (r = 4; r < 25; r++)
        expect(scr_row(r)[0] == ' ' && scr_row(r)[40] == ' ', "no stack: the rest cleared");
}

static void t_enter_leave(void)
{
    static char buf[256];
    NmTuiOut    o;

    scr_reset(25, 80);
    memset(&o, 0, sizeof(o));
    o.buf = buf; o.size = sizeof(buf); o.flush = to_screen;
    nm_tui_enter(&o);
    expect(scr.cursor_off && scr.wrap_off && scr.scroll_off && scr.resize_on &&
           scr.cleared == 1, "enter: cursor, wrap, scroll, resize report, clear");
    nm_tui_leave(&o);
    expect(!scr.cursor_off && !scr.wrap_off && !scr.scroll_off && !scr.resize_on &&
           scr.cleared == 2, "leave: all of it back");
    frame_len = 0;
    nm_tui_ask_bounds(&o);
    expect(frame_len == 4 && memcmp(frame, "\2330 q", 4) == 0, "the window status request");
}

static int feed(NmTuiIn *in, const char *s, NmTuiEvent *ev, unsigned cap)
{
    unsigned n = 0;

    while (*s != '\0' && n < cap)
        if (nm_tui_feed(in, (unsigned char)*s++, &ev[n]))
            n++;
    if (n < cap && nm_tui_idle(in, &ev[n]))
        n++;
    return (int)n;
}

static void t_input(void)
{
    NmTuiIn    in;
    NmTuiEvent ev[4];

    memset(&in, 0, sizeof(in));
    expect(feed(&in, "\2331;1;25;80 r", ev, 4) == 1 && ev[0].kind == NM_IN_BOUNDS &&
           ev[0].rows == 25 && ev[0].cols == 80, "the bounds report");
    expect(feed(&in, "q", ev, 4) == 1 && ev[0].kind == NM_IN_KEY && ev[0].key == 'q', "a key");
    expect(feed(&in, "\233A \2331;1;48;132 r", ev, 4) == 2 && ev[0].kind == NM_IN_KEY &&
           ev[0].key == ' ' && ev[1].rows == 48 && ev[1].cols == 132,
           "a cursor key ignored, then space and a bounds report");
    expect(feed(&in, "\23312;0;0;0;0;0;0;0|", ev, 4) == 1 && ev[0].kind == NM_IN_RESIZE,
           "the resize report");
    expect(feed(&in, "\033", ev, 4) == 1 && ev[0].kind == NM_IN_KEY && ev[0].key == 27,
           "Esc on its own");
    expect(feed(&in, "\033[1;1;30;100 r", ev, 4) == 1 && ev[0].kind == NM_IN_BOUNDS &&
           ev[0].rows == 30, "Esc [ is a CSI");
    expect(feed(&in, "\003", ev, 4) == 1 && ev[0].key == 3, "Ctrl-C as a character");
    expect(feed(&in, "\2330 v", ev, 4) == 0, "Right Amiga V ignored");
}

int main(void)
{
    printf("NetMeter arithmetic\n");
    t_rates();
    t_totals();
    t_delta();
    t_rate();
    t_scale();
    t_bar();
    t_average();
    t_text();
    t_frame_80x25();
    t_frame_small();
    t_enter_leave();
    t_input();
    printf("netmeter_fmt checks=%lu failures=%lu\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
