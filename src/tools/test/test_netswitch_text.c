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

    refuses("selected", wrap_selected, "eth0");
    refuses("error", wrap_error, "boom");

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
