#include "netswitch_text.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int yes, const char *what, const char *got, const char *want)
{
    if (!yes)
    {
        fprintf(stderr, "FAIL: %s: got \"%s\", wanted \"%s\"\n", what,
                got != NULL ? got : "(null)", want != NULL ? want : "(null)");
        failures++;
    }
}

static void eq(const char *what, const char *got, const char *want)
{
    check(strcmp(got, want) == 0, what, got, want);
}

/* Every builder must refuse rather than truncate: a half-written address or
   a chopped error is worse than nothing, because it reads as a fact. */
static void refuses(const char *what, int (*fn)(char *, size_t, const char *),
                    const char *arg)
{
    char buf[8];

    memset(buf, 'x', sizeof(buf));
    if (fn(buf, 4, arg))
    {
        fprintf(stderr, "FAIL: %s: accepted a 4-byte buffer\n", what);
        failures++;
        return;
    }
    if (buf[0] != '\0')
    {
        fprintf(stderr, "FAIL: %s: left \"%s\" behind\n", what, buf);
        failures++;
    }
}

static int wrap_selected(char *out, size_t cap, const char *name)
{
    return ns_selected_line(out, cap, name);
}

/* A proportional metric: narrow "il.:1 " 4 px, wide "MWmw&" 12, the rest 8 --
   the same classes the layout test's fake font uses, so a width that overflows
   a column here is one that overflows in the window. */
static int px_measure(void *ctx, const char *text, int len)
{
    int w = 0, i;

    (void)ctx;
    for (i = 0; i < len; i++)
        w += strchr("il.:1 ", text[i]) != NULL ? 4
             : (strchr("MWmw&", text[i]) != NULL ? 12 : 8);
    return w;
}

static int px_of(const char *s)
{
    return px_measure(NULL, s, (int)strlen(s));
}

static int wrap_error(char *out, size_t cap, const char *error)
{
    return ns_last_error(out, cap, error);
}

int main(void)
{
    char buf[128];
    char small[4];
    char tiny[2];

    eq("up", ns_state_word(NS_STATE_UP), "Online");
    eq("down", ns_state_word(NS_STATE_DOWN), "Offline");
    eq("detached", ns_state_word(NS_STATE_DETACHED), "Not attached");

    /* The button says what the press does, and going online attaches: Online
       alone answers ENXIO for a configured interface the stack has not got. */
    eq("action up", ns_action_word(NS_STATE_UP), "Go Offline");
    eq("action down", ns_action_word(NS_STATE_DOWN), "Go Online");
    eq("action detached", ns_action_word(NS_STATE_DETACHED), "Go Online");

    check(ns_status_line(buf, sizeof(buf), 3, 1, 1, 1), "status fits", buf, "");
    eq("status 3", buf, "3 interfaces | 1 online 1 offline 1 not attached");

    check(ns_status_line(buf, sizeof(buf), 1, 1, 0, 0), "status 1", buf, "");
    eq("status 1", buf, "1 interface | 1 online 0 offline 0 not attached");

    check(ns_status_line(buf, sizeof(buf), 0, 0, 0, 0), "status 0", buf, "");
    eq("status 0", buf, "0 interfaces | 0 online 0 offline 0 not attached");

    check(ns_status_line(buf, sizeof(buf), 12, 10, 1, 1), "status 12", buf, "");
    eq("status 12", buf, "12 interfaces | 10 online 1 offline 1 not attached");

    check(!ns_status_line(small, sizeof(small), 3, 1, 1, 1),
          "status refuses 4 bytes", small, "");
    check(!ns_status_line(tiny, sizeof(tiny), 1, 1, 0, 0),
          "status refuses 2 bytes", tiny, "");

    /* IPv4 wins; IPv6 stands in only without one. */
    check(ns_list_address(buf, sizeof(buf), "192.168.1.23", "fe80::1",
                          NS_STATE_UP), "address v4", buf, "");
    eq("address v4", buf, "192.168.1.23");
    check(ns_list_address(buf, sizeof(buf), NULL, "fe80::211:22ff:fe33:4455",
                          NS_STATE_UP), "address v6", buf, "");
    eq("address v6", buf, "fe80::211:22ff:fe33:4455");
    check(ns_list_address(buf, sizeof(buf), "", "", NS_STATE_UP),
          "address pending", buf, "");
    eq("address pending", buf, "no address yet");
    check(ns_list_address(buf, sizeof(buf), "", "", NS_STATE_DOWN),
          "address down", buf, "");
    eq("address down", buf, "-");
    check(ns_list_address(buf, sizeof(buf), NULL, NULL, NS_STATE_DETACHED),
          "address detached", buf, "");
    eq("address detached", buf, "-");

    check(ns_selected_line(buf, sizeof(buf), "eth0"), "selected", buf, "");
    eq("selected", buf, "Selected: eth0");

    check(ns_last_error(buf, sizeof(buf), NULL), "error none", buf, "");
    eq("error none", buf, "Last error: none");
    check(ns_last_error(buf, sizeof(buf), ""), "error empty", buf, "");
    eq("error empty", buf, "Last error: none");
    check(ns_last_error(buf, sizeof(buf),
                        "no such interface"), "error text", buf, "");
    eq("error text", buf, "Last error: no such interface");

    check(ns_take_all_label(buf, sizeof(buf), 1), "take-all 1", buf, "");
    eq("take-all 1", buf, "Take all offline (1)");
    check(ns_take_all_label(buf, sizeof(buf), 0), "take-all 0", buf, "");
    eq("take-all 0", buf, "Take all offline (0)");
    check(ns_take_all_prompt(buf, sizeof(buf), 1), "prompt 1", buf, "");
    eq("prompt 1", buf, "1 interface will go offline.");
    check(ns_take_all_prompt(buf, sizeof(buf), 4), "prompt 4", buf, "");
    eq("prompt 4", buf, "4 interfaces will go offline.");

    /* Elision is by characters, not pixels: the app draws in a cell font, so
       a column is a character count and this is the rule the host can check.
       What comes back is never longer than the column it was asked for. */
    check(ns_elide(buf, sizeof(buf), "eth0", 8), "elide shorter", buf, "");
    eq("elide shorter", buf, "eth0");
    check(ns_elide(buf, sizeof(buf), "fe80::211:22ff:fe33:4455", 15),
          "elide v6", buf, "");
    eq("elide v6", buf, "fe80::211:22...");
    check(strlen(buf) == 15, "elide v6 keeps the column", buf, "15 chars");
    check(ns_elide(buf, sizeof(buf), "anxzz9000.device 0", 18), "elide exact",
          buf, "");
    eq("elide exact", buf, "anxzz9000.device 0");
    check(ns_elide(buf, sizeof(buf), "anxzz9000.device 12", 18), "elide device",
          buf, "");
    eq("elide device", buf, "anxzz9000.devic...");
    check(strlen(buf) == 18, "elide device keeps the column", buf, "18 chars");
    check(ns_elide(buf, sizeof(buf), "abcdef", 3), "elide tight", buf, "");
    eq("elide tight", buf, "abc");
    check(ns_elide(buf, sizeof(buf), "abcdef", 0), "elide empty", buf, "");
    eq("elide empty", buf, "");
    check(!ns_elide(small, sizeof(small), "abcdef", 6), "elide refuses", small,
          "");

    /* In place, which is how the list's address column gets its cell: the
       formatter writes the address into the buffer and the elider cuts it
       there.  An elider that cleared the buffer before reading it would hand
       back an empty column and still say it had succeeded. */
    check(ns_list_address(buf, sizeof(buf), NULL, NULL, NS_STATE_DETACHED),
          "address in place", buf, "");
    check(ns_elide(buf, sizeof(buf), buf, 15), "elide in place", buf, "");
    eq("elide in place short", buf, "-");
    check(ns_list_address(buf, sizeof(buf), "fe80::211:22ff:fe33:4455", NULL,
                          NS_STATE_UP), "address in place long", buf, "");
    check(ns_elide(buf, sizeof(buf), buf, 15), "elide in place long", buf, "");
    eq("elide in place long", buf, "fe80::211:22...");
    check(strlen(buf) == 15, "elide in place keeps the column", buf,
          "15 chars");
    check(ns_list_address(buf, sizeof(buf), "", "", NS_STATE_UP),
          "address in place pending", buf, "");
    check(ns_elide(buf, sizeof(buf), buf, 18), "elide in place pending", buf,
          "");
    eq("elide in place pending", buf, "no address yet");

    refuses("selected", wrap_selected, "eth0");
    refuses("error", wrap_error, "boom");

    /* A column is a pixel width, so the cut has to follow the glyphs: this is
       the pair the layout's columns are sized from.  Counted characters give
       "WWWWWWWW" 96 px into a 64 px column; measured, four of them and the
       ellipsis fit exactly. */
    check(ns_elide(buf, sizeof(buf), "WWWWWWWW", 8), "elide wide chars", buf,
          "");
    eq("elide wide chars", buf, "WWWWWWWW");
    check(px_of(buf) == 96, "the counted cut is wider than the column",
          buf, "96 px");
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), "WWWWWWWW", 64),
          "fit wide chars", buf, "");
    eq("fit wide chars", buf, "WWWW...");
    check(px_of(buf) == 60 && px_of(buf) <= 64, "fit wide chars holds the column",
          buf, "60 px");

    /* The same width holds more of a narrow string: the cut measures.  Twenty
       narrow glyphs are 80 px, so thirteen of them and the ellipsis fill the
       column exactly where four wide ones do. */
    check(ns_fit(px_measure, NULL, buf, sizeof(buf),
                 "iiiiiiiiiiiiiiiiiiii", 64), "fit narrow chars", buf, "");
    check(px_of(buf) == 64, "fit narrow chars fills the column", buf, "64 px");
    check(strlen(buf) == 16 && strcmp(buf + 13, "...") == 0 &&
          strncmp(buf, "iiiiiiiiiiiiiiiiiiii", 13) == 0,
          "the narrow fit keeps thirteen glyphs", buf, "13 i's and ...");
    check(strlen(buf) > strlen("WWWW..."),
          "the narrow fit keeps more characters", buf, "> 7");

    /* Sixteen of them are exactly the column and are kept whole. */
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), "iiiiiiiiiiiiiiii", 64),
          "fit exact narrow", buf, "");
    eq("fit exact narrow", buf, "iiiiiiiiiiiiiiii");

    /* Whole when it fits, and exactly at the edge. */
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), "eth0", 64), "fit whole",
          buf, "");
    eq("fit whole", buf, "eth0");
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), "eth0", px_of("eth0")),
          "fit exact", buf, "");
    eq("fit exact", buf, "eth0");

    /* Too narrow even for "...": the longest prefix without one, and nothing
       at all rather than a bare mark when not one glyph fits. */
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), "iiii", 8), "fit tiny",
          buf, "");
    eq("fit tiny", buf, "ii");
    check(!ns_fit(px_measure, NULL, buf, sizeof(buf), "iiii", 3),
          "fit refuses one glyph", buf, "");
    eq("fit refuses one glyph", buf, "");

    /* A refusal, not a truncation, when the buffer cannot hold what fits. */
    check(!ns_fit(px_measure, NULL, small, sizeof(small), "eth0", 64),
          "fit refuses a 4-byte buffer", small, "");
    eq("fit refuses a 4-byte buffer", small, "");

    /* In place, the shape the renderer uses: format the cell, then cut it to
       the column it has to live in. */
    check(ns_list_address(buf, sizeof(buf), "fe80::211:22ff:fe33:4455", NULL,
                          NS_STATE_UP), "fit in place format", buf, "");
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), buf, 120),
          "fit in place", buf, "");
    check(px_of(buf) <= 120, "fit in place holds the column", buf, "<= 120 px");
    check(strncmp(buf, "fe80::211:22ff:fe33:4455", strlen(buf) - 3) == 0,
          "fit in place is its own prefix", buf, "");
    check(strcmp(buf + strlen(buf) - 3, "...") == 0, "fit in place marks the cut",
          buf, "");

    /* Long to short in one buffer, which is what a repaint is: the renderer
       keeps one cell buffer per column and formats it again for every row and
       every pass, so a short value written after a long one must not leave the
       long one behind it.  This is the same shape as the in-place defect above
       -- a longer string has to disappear from a cell that no longer has room
       for it -- and it is checked on the formatter the panel and the list
       share, for the address that the click in the emulator arm changed from
       an address to "none". */
    check(ns_list_address(buf, sizeof(buf), "fe80::211:22ff:fe33:4455", NULL,
                          NS_STATE_UP), "long then short: format the long value",
          buf, "");
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), buf, 40),
          "long then short: fit the long value", buf, "");
    check(strcmp(buf + strlen(buf) - 3, "...") == 0,
          "long then short: the long value was cut to the cell", buf, "");
    check(ns_list_address(buf, sizeof(buf), NULL, NULL, NS_STATE_DOWN),
          "long then short: format the short value", buf, "");
    check(ns_fit(px_measure, NULL, buf, sizeof(buf), buf, 40),
          "long then short: fit the short value", buf, "");
    eq("long then short: none of the long value survives", buf, ns_none_word);
    check(strlen(buf) == strlen(ns_none_word),
          "long then short: the cell is exactly the short value", buf, "1 char");

    /* The legend names the three shapes in the same order the states are
       numbered, so the glyph a reader looks up is the one the row shows. */
    eq("legend up", ns_legend_word(NS_STATE_UP), "online");
    eq("legend down", ns_legend_word(NS_STATE_DOWN), "offline");
    eq("legend detached", ns_legend_word(NS_STATE_DETACHED), "not attached");
    check(ns_pending_word[0] != '\0' && ns_none_word[0] != '\0' &&
          ns_panel_pending[0] != '\0' && ns_panel_none[0] != '\0' &&
          ns_take_all_warning[0] != '\0' && ns_stack_down_hint[0] != '\0' &&
          ns_offline_button[0] != '\0',
          "the no-number phrases are not empty", ns_pending_word, "");

    if (failures == 0) printf("netswitch text: all checks passed\n");
    return failures != 0;
}
