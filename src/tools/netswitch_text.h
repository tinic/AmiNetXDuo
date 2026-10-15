/*
 * NetSwitch: the words its window shows, and the rules that pick them.  No
 * AmigaOS calls, so test/test_netswitch_text.c runs it on the build host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSWITCH_TEXT_H
#define AMINETXDUO_NETSWITCH_TEXT_H

#include <stddef.h>

/*
 * The three states, read from NETSTATUS_INTERFACES' nsi_Flags: a definition
 * with no live interface, a live one that is down, and one that is up.  A
 * misspelt interface file and an offline one are not the same thing, and
 * ShowNetStatus answers the first as "offline" (shownetstatus.c:1330) --
 * which is the answer this app exists not to give.
 */
typedef enum NsState
{
    NS_STATE_DETACHED = 0,      /* configured; the stack has no such interface */
    NS_STATE_DOWN,              /* attached, not online */
    NS_STATE_UP                 /* attached and online */
} NsState;

/* The word for a state: "Online", "Offline", "Not attached". */
const char *ns_state_word(NsState state);

/* The action button's caption for the selected row.  Going online attaches
   when nothing is attached: Online only up/downs an interface the stack
   already has (onoff.c:368), and answers ENXIO otherwise. */
const char *ns_action_word(NsState state);

/*
 * The line under the list: "3 interfaces | 1 online 1 offline 1 not
 * attached".  All three counts are always there, so the line's shape does not
 * change as interfaces come and go; only "interface" is singular in one.
 * Returns 1, or 0 when cap is too small.
 */
int ns_status_line(char *out, size_t cap, unsigned total, unsigned online,
                   unsigned down, unsigned detached);

/*
 * The address cell of one list row.  IPv4 wins when the interface has it;
 * the first IPv6 stands in otherwise (the caller elides it to the column).
 * Up with neither is not the same as down: DHCP can still be running, so it
 * says so rather than showing the dash a down interface shows.
 */
int ns_list_address(char *out, size_t cap, const char *ipv4, const char *ipv6,
                    NsState state);

/* The panel's first line: "Selected: eth0". */
int ns_selected_line(char *out, size_t cap, const char *name);

/* "Last error: none" or the error the last action got.  A GUI that hides
   why a button did nothing is worse than the CLI it replaces. */
int ns_last_error(char *out, size_t cap, const char *error);

/*
 * The take-all button and the confirmation behind it.  This can cut the
 * operator's own link -- the ssh or wifi door into the machine -- so the
 * button carries the count and the requester states what will happen before
 * asking.  Returns 1, or 0 when cap is too small.
 */
int ns_take_all_label(char *out, size_t cap, unsigned count);
int ns_take_all_prompt(char *out, size_t cap, unsigned count);

/* Cuts text to at most chars characters, ending it with "...".  A limit
   under four characters truncates without the ellipsis.  Safe in place --
   out may be text, which is how netswitch.c formats a list column.  Returns
   1, or 0 when cap cannot hold what the limit allows; a refusal leaves out
   empty. */
int ns_elide(char *out, size_t cap, const char *text, unsigned chars);

/*
 * A font's width for len characters of text.  The same shape as NsMeasure in
 * netswitch_layout.h, repeated here so this module needs nothing from the
 * layout: a caller passes TextLength() and its RastPort as ctx.
 */
typedef int (*NsTextMeasure)(void *ctx, const char *text, int len);

/*
 * Cuts text to the longest prefix that, with "...", measures within px pixels
 * -- the rule for a column whose width came from the font.  Counting
 * characters cannot do this: a proportional font's "WWWW" is half again its
 * "iiii", so a column sized for the average glyph overflows on the wide ones
 * and runs into its neighbour.  A prefix that fits by itself is kept whole and
 * without an ellipsis; when even "..." is too wide the prefix is cut without
 * one; and when not one glyph fits, out is empty and the return is 0, which
 * says less than any cut would.  Refuses (empty out) rather than truncating
 * when cap cannot hold what fits.  Safe in place -- out may be text.
 */
int ns_fit(NsTextMeasure measure, void *ctx, char *out, size_t cap,
           const char *text, int px);

/*
 * The legend's word for a state, lowercase so it sits beside its shape:
 * "online", "offline", "not attached".  The panel uses ns_state_word()'s
 * capitals; the two differ on purpose.
 */
const char *ns_legend_word(NsState state);

/* The phrases with no numbers in them. */
extern const char *const ns_hint_text;          /* "Return / dbl-click toggles" */
extern const char *const ns_addresses_head;     /* "Addresses:" */
extern const char *const ns_no_error;           /* "none" */
extern const char *const ns_pending_word;       /* "no address yet" */
extern const char *const ns_none_word;          /* "-" */
extern const char *const ns_panel_pending;      /* "online but has no address yet" */
extern const char *const ns_panel_none;         /* "(none)" */
extern const char *const ns_take_all_warning;   /* "This may drop your own connection." */
extern const char *const ns_stack_down_hint;    /* "Network is not running." */
extern const char *const ns_query_failed_hint;  /* "Cannot read interfaces." */
extern const char *const ns_offline_button;     /* the confirm's yes: "Offline" */

#endif
