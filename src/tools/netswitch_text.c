/* SPDX-License-Identifier: MIT */
#include "netswitch_text.h"

const char *const ns_hint_text         = "Return / dbl-click toggles";
const char *const ns_addresses_head    = "Addresses:";
const char *const ns_no_error          = "none";
const char *const ns_pending_word      = "no address yet";
const char *const ns_none_word         = "-";
const char *const ns_panel_pending     = "online but has no address yet";
const char *const ns_panel_none        = "(none)";
const char *const ns_take_all_warning  = "This may drop your own connection.";
const char *const ns_stack_down_hint   = "Network is not running.";
const char *const ns_offline_button    = "Offline";

static size_t ns_len(const char *s)
{
    size_t n = 0;

    while (s != NULL && s[n] != '\0') n++;
    return n;
}

/* Appends text, refusing rather than truncating.  *used counts what is in out;
   one byte is always kept for the terminator. */
static int append(char *out, size_t cap, size_t *used, const char *text)
{
    size_t n = ns_len(text);
    size_t i;

    if (*used + n + 1 > cap) return 0;
    for (i = 0; i < n; i++) out[*used + i] = text[i];
    *used += n;
    out[*used] = '\0';
    return 1;
}

/* Appends an unsigned number, the one place this module needs digits. */
static int append_num(char *out, size_t cap, size_t *used, unsigned value)
{
    char digits[11];
    int  n = 0;

    do
    {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (value != 0);

    if (*used + (size_t)n + 1 > cap) return 0;
    while (n > 0) out[(*used)++] = digits[--n];
    out[*used] = '\0';
    return 1;
}

const char *ns_state_word(NsState state)
{
    switch (state)
    {
    case NS_STATE_UP:       return "Online";
    case NS_STATE_DOWN:     return "Offline";
    case NS_STATE_DETACHED: return "Not attached";
    }
    return "Not attached";
}

const char *ns_legend_word(NsState state)
{
    switch (state)
    {
    case NS_STATE_UP:       return "online";
    case NS_STATE_DOWN:     return "offline";
    case NS_STATE_DETACHED: return "not attached";
    }
    return "not attached";
}

const char *ns_action_word(NsState state)
{
    return state == NS_STATE_UP ? "Go Offline" : "Go Online";
}

int ns_status_line(char *out, size_t cap, unsigned total, unsigned online,
                   unsigned down, unsigned detached)
{
    size_t used = 0;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (!append_num(out, cap, &used, total) ||
        !append(out, cap, &used, total == 1 ? " interface | " : " interfaces | ") ||
        !append_num(out, cap, &used, online) ||
        !append(out, cap, &used, " online ") ||
        !append_num(out, cap, &used, down) ||
        !append(out, cap, &used, " offline ") ||
        !append_num(out, cap, &used, detached) ||
        !append(out, cap, &used, " not attached"))
        return 0;
    return 1;
}

int ns_list_address(char *out, size_t cap, const char *ipv4, const char *ipv6,
                    NsState state)
{
    size_t used = 0;
    const char *text;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (ipv4 != NULL && ipv4[0] != '\0')
        text = ipv4;
    else if (ipv6 != NULL && ipv6[0] != '\0')
        text = ipv6;
    else if (state == NS_STATE_UP)
        text = ns_pending_word;
    else
        text = ns_none_word;

    return append(out, cap, &used, text);
}

int ns_selected_line(char *out, size_t cap, const char *name)
{
    size_t used = 0;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (!append(out, cap, &used, "Selected: ")) return 0;
    return append(out, cap, &used, name != NULL ? name : "");
}

int ns_last_error(char *out, size_t cap, const char *error)
{
    size_t used = 0;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (!append(out, cap, &used, "Last error: ")) return 0;
    return append(out, cap, &used,
                  (error != NULL && error[0] != '\0') ? error : ns_no_error);
}

int ns_take_all_label(char *out, size_t cap, unsigned count)
{
    size_t used = 0;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (!append(out, cap, &used, "Take all offline (")) return 0;
    if (!append_num(out, cap, &used, count)) return 0;
    return append(out, cap, &used, ")");
}

int ns_take_all_prompt(char *out, size_t cap, unsigned count)
{
    size_t used = 0;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (!append_num(out, cap, &used, count)) return 0;
    if (!append(out, cap, &used,
                count == 1 ? " interface will go offline."
                           : " interfaces will go offline."))
        return 0;
    return 1;
}

int ns_elide(char *out, size_t cap, const char *text, unsigned chars)
{
    size_t len = ns_len(text);
    size_t keep;
    size_t i;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    if (len <= chars)
    {
        size_t used = 0;
        return append(out, cap, &used, text);
    }

    /* "..." costs three of the column: the point of eliding is to say more
       with less, and a limit too small for that keeps the text instead. */
    keep = chars > 3 ? chars - 3 : chars;
    if (cap <= keep + 3) return 0;

    for (i = 0; i < keep; i++) out[i] = text[i];
    if (chars > 3)
    {
        out[keep] = '.';
        out[keep + 1] = '.';
        out[keep + 2] = '.';
        out[keep + 3] = '\0';
    }
    else
        out[keep] = '\0';
    return 1;
}
