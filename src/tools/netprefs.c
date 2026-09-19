/*
 * NetPrefs, a small Workbench editor for AmiNetXDuo interface definitions.
 *
 * This deliberately edits the Roadshow-compatible text files in place.  It
 * owns only the fields shown in the window; comments and every other keyword
 * survive a save byte for byte.  Live changes go through the public commands,
 * so the GUI cannot acquire a second, subtly different interface lifecycle.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "netprefs_text.h"
#include "aminetxduo/version.h"

#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>

const char *const tool_name = "NetPrefs";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetPrefs");

struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *GadToolsBase;

#define NP_MAX_INTERFACES  32
#define NP_PATH_LEN        192
#define NP_FILE_MAX        (256UL * 1024UL)
#define NP_VALUE_LEN       160
#define NP_SAVE_FIELDS     24
/* Roadshow and the live status ABI expose at most 15 name characters. */
#define NP_IFNAME_LEN      16

enum
{
    GID_INTERFACE = 1,
    GID_PANEL,
    GID_NEW,
    GID_NAME,
    GID_ID,
    GID_DEVICE,
    GID_UNIT,
    GID_CARD,
    GID_HWADDRESS,
    GID_IPV4,
    GID_ADDRESS,
    GID_NETMASK,
    GID_GATEWAY,
    GID_IPV6,
    GID_ADDRESS6_1,
    GID_ADDRESS6_2,
    GID_GATEWAY6,
    GID_MDNS,
    GID_STATE,
    GID_BOOT,
    GID_PRIORITY,
    GID_DOWN_OFFLINE,
    GID_INIT_DELAY,
    GID_PROMISCUOUS,
    GID_MTU,
    GID_RXBUFFER,
    GID_IPREQUESTS,
    GID_ARPREQUESTS,
    GID_WRITEREQUESTS,
    GID_STATUS,
    GID_SAVE,
    GID_APPLY,
    GID_LIVE_ACTION,
    GID_REMOVE,
    GID_CLOSE
};

enum
{
    NP_PANEL_GENERAL,
    NP_PANEL_IPV4,
    NP_PANEL_IPV6,
    NP_PANEL_DEVICE,
    NP_PANEL_TUNING,
    NP_PANEL_COUNT
};

enum
{
    NP_LIVE_UNKNOWN = -1,
    NP_LIVE_NOT_SAVED,
    NP_LIVE_STACK_STOPPED,
    NP_LIVE_NOT_ADDED,
    NP_LIVE_OFFLINE,
    NP_LIVE_ONLINE,
    NP_LIVE_UNAVAILABLE
};

typedef struct NetPrefs
{
    struct Screen *screen;
    struct Window *window;
    struct Gadget *gadgets;
    struct Gadget *panel_gadgets[NP_PANEL_COUNT];
    LONG           panel_gadget_count[NP_PANEL_COUNT];
    APTR           visual;
    struct Gadget *g_interface;
    struct Gadget *g_panel;
    struct Gadget *g_name;
    struct Gadget *g_id;
    struct Gadget *g_device;
    struct Gadget *g_unit;
    struct Gadget *g_card;
    struct Gadget *g_hwaddress;
    struct Gadget *g_ipv4;
    struct Gadget *g_address;
    struct Gadget *g_netmask;
    struct Gadget *g_gateway;
    struct Gadget *g_ipv6;
    struct Gadget *g_address6[AMI_CFG_MAX_ADDRESS6];
    struct Gadget *g_gateway6;
    struct Gadget *g_mdns;
    struct Gadget *g_state;
    struct Gadget *g_boot;
    struct Gadget *g_priority;
    struct Gadget *g_down_offline;
    struct Gadget *g_init_delay;
    struct Gadget *g_promiscuous;
    struct Gadget *g_mtu;
    struct Gadget *g_rxbuffer;
    struct Gadget *g_iprequests;
    struct Gadget *g_arprequests;
    struct Gadget *g_writerequests;
    struct Gadget *g_status;
    struct Gadget *g_live_status;
    struct Gadget *live_online_gadgets;
    struct Gadget *live_offline_gadgets;
    struct Gadget *g_live_online;
    struct Gadget *g_live_offline;
    struct Gadget *g_live_action;
    struct List    interface_list;
    struct Node    interface_nodes[NP_MAX_INTERFACES];
    char           names[NP_MAX_INTERFACES][TOOL_NAME_LEN];
    ULONG          count;
    LONG           selected;
    ULONG          active_panel;
    LONG           live_state;
} NetPrefs;

static NetPrefs np = { .selected = -1, .live_state = NP_LIVE_UNKNOWN };
static char np_address[16];
static char np_netmask[16];
static char np_gateway[16];
static char np_address6[AMI_CFG_MAX_ADDRESS6][AMI_CFG_IP6_STRLEN + 5];
static char np_gateway6[AMI_CFG_IP6_STRLEN];
static char np_hwaddress[18];
static char np_status[128];

static VOID show_panel(ULONG which);
static VOID select_panel(ULONG which);
static LONG live_state_of(const char *name);

static const STRPTR panel_labels[] =
{
    (STRPTR)"General", (STRPTR)"IPv4", (STRPTR)"IPv6",
    (STRPTR)"Device", (STRPTR)"Tuning", NULL
};

static const STRPTR ipv4_labels[] =
{
    (STRPTR)"Static", (STRPTR)"DHCP", (STRPTR)"Link-local",
    (STRPTR)"Off", NULL
};

static const STRPTR ipv6_labels[] =
{
    (STRPTR)"Off", (STRPTR)"Link-local", (STRPTR)"Automatic",
    (STRPTR)"Static", (STRPTR)"DHCPv6", NULL
};

static ULONG text_len(const char *s)
{
    ULONG n = 0;
    while (s != NULL && s[n] != '\0') n++;
    return n;
}

static BOOL sane_name(const char *s)
{
    return np_interface_name_safe(s, NP_IFNAME_LEN) ? TRUE : FALSE;
}

static VOID decimal(ULONG value, char *out, ULONG outlen)
{
    char  rev[12];
    ULONG n = 0;
    ULONG i;

    if (outlen == 0) return;
    do
    {
        rev[n++] = (char)('0' + value % 10UL);
        value /= 10UL;
    } while (value != 0 && n < sizeof(rev));

    i = 0;
    while (n != 0 && i + 1 < outlen) out[i++] = rev[--n];
    out[i] = '\0';
}

static VOID signed_decimal(LONG value, char *out, ULONG outlen)
{
    ULONG magnitude;

    if (value < 0)
    {
        if (outlen < 2) return;
        out[0] = '-';
        magnitude = (ULONG)(-(value + 1)) + 1UL;
        decimal(magnitude, out + 1, outlen - 1);
    }
    else
        decimal((ULONG)value, out, outlen);
}

static VOID requester(const char *body)
{
    struct EasyStruct easy;

    easy.es_StructSize   = sizeof(easy);
    easy.es_Flags        = 0;
    easy.es_Title        = (STRPTR)"AmiNetXDuo NetPrefs";
    easy.es_TextFormat   = (STRPTR)body;
    easy.es_GadgetFormat = (STRPTR)"OK";
    (VOID)EasyRequestArgs(np.window, &easy, NULL, NULL);
}

static BOOL confirm(const char *body, const char *yes)
{
    struct EasyStruct easy;
    char buttons[48];

    tool_copy_string(buttons, sizeof(buttons), yes);
    tool_copy_string(buttons + text_len(buttons),
                     sizeof(buttons) - text_len(buttons), "|Cancel");
    easy.es_StructSize   = sizeof(easy);
    easy.es_Flags        = 0;
    easy.es_Title        = (STRPTR)"AmiNetXDuo NetPrefs";
    easy.es_TextFormat   = (STRPTR)body;
    easy.es_GadgetFormat = (STRPTR)buttons;
    return (BOOL)(EasyRequestArgs(np.window, &easy, NULL, NULL) == 1);
}

static VOID set_status(const char *s)
{
    struct TagItem tags[2];

    tool_copy_string(np_status, sizeof(np_status), s);
    if (np.window == NULL || np.g_status == NULL) return;
    tags[0].ti_Tag  = GTTX_Text;
    tags[0].ti_Data = (ULONG)np_status;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;
    GT_SetGadgetAttrsA(np.g_status, np.window, NULL, tags);
}

static char *string_value(struct Gadget *g)
{
    struct StringInfo *si = (struct StringInfo *)g->SpecialInfo;
    return (si != NULL && si->Buffer != NULL) ? (char *)si->Buffer : (char *)"";
}

static LONG integer_value(struct Gadget *g)
{
    struct StringInfo *si = (struct StringInfo *)g->SpecialInfo;
    return (si != NULL) ? si->LongInt : 0;
}

static BOOL checked(struct Gadget *g)
{
    return (BOOL)((g->Flags & GFLG_SELECTED) != 0);
}

static VOID set_attr(struct Gadget *g, ULONG tag, ULONG value)
{
    struct TagItem tags[2];
    tags[0].ti_Tag = tag; tags[0].ti_Data = value;
    tags[1].ti_Tag = TAG_DONE; tags[1].ti_Data = 0;
    GT_SetGadgetAttrsA(g, np.window, NULL, tags);
}

/*
 * The Name gadget lives on the General page.  GA_Disabled through GadTools
 * refreshes the gadget into the window whether or not its page is attached:
 * a Save made from the Device page drew the disabled Name gadget over that
 * page's first line ("DeName"), and the page switches that followed hung the
 * whole machine (A1200, 2026-09-19, twice, reproducibly: New, Save from the
 * Device page, cycle to General, click a checkbox).  Off its page only the
 * flag changes; the page's AddGList + RefreshGList draws it when it returns.
 */
static VOID set_name_locked(BOOL locked)
{
    if (np.active_panel == NP_PANEL_GENERAL)
        set_attr(np.g_name, GA_Disabled, (ULONG)locked);
    else if (locked)
        np.g_name->Flags |= GFLG_DISABLED;
    else
        np.g_name->Flags &= (UWORD)~GFLG_DISABLED;
}

static VOID set_static_fields(BOOL enabled)
{
    set_attr(np.g_address, GA_Disabled, (ULONG)!enabled);
    set_attr(np.g_netmask, GA_Disabled, (ULONG)!enabled);
    set_attr(np.g_gateway, GA_Disabled, (ULONG)!enabled);
}

static VOID set_static6_fields(BOOL enabled)
{
    ULONG i;
    for (i = 0; i < AMI_CFG_MAX_ADDRESS6; i++)
        set_attr(np.g_address6[i], GA_Disabled, (ULONG)!enabled);
    set_attr(np.g_gateway6, GA_Disabled, (ULONG)!enabled);
}

static BOOL ensure_dir(const char *path)
{
    BPTR lock;

    if (tool_exists(path)) return TRUE;
    lock = CreateDir((CONST_STRPTR)path);
    if (lock == (BPTR)0) return FALSE;
    UnLock(lock);
    return TRUE;
}

static char *read_file(const char *path, ULONG *length, BOOL missing_ok)
{
    BPTR  fh;
    LONG  size;
    LONG  got;
    char *data;

    *length = 0;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
    {
        if (!missing_ok) requester("The file could not be opened.");
        return NULL;
    }
    if (Seek(fh, 0, OFFSET_END) < 0)
    {
        Close(fh);
        requester("The file size could not be read.");
        return NULL;
    }
    /* AmigaDOS Seek() returns the previous position.  After moving to the
       end, moving back to the beginning therefore returns the file size. */
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0 || (ULONG)size > NP_FILE_MAX)
    {
        Close(fh);
        requester("The file is too large to edit safely.");
        return NULL;
    }
    data = (char *)ami_alloc((ULONG)size + 1UL);
    if (data == NULL)
    {
        Close(fh);
        requester("There is not enough free memory to edit this file.");
        return NULL;
    }
    got = Read(fh, data, size);
    Close(fh);
    if (got != size)
    {
        ami_free(data);
        requester("Only part of the file could be read.");
        return NULL;
    }
    data[size] = '\0';
    *length = (ULONG)size;
    return data;
}

/* Temporary and rollback names end in .info, so the interface scanner can
 * never mistake a half-completed transaction for another interface. */
static BOOL replace_file(const char *path, const char *data, ULONG length)
{
    char tmp[NP_PATH_LEN];
    char old[NP_PATH_LEN];
    BPTR fh;
    LONG wrote;
    BOOL had_old;

    tool_copy_string(tmp, sizeof(tmp), path);
    tool_copy_string(tmp + text_len(tmp), sizeof(tmp) - text_len(tmp),
                     ".new.info");
    tool_copy_string(old, sizeof(old), path);
    tool_copy_string(old + text_len(old), sizeof(old) - text_len(old),
                     ".old.info");

    (VOID)DeleteFile((CONST_STRPTR)tmp);
    fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (fh == (BPTR)0) return FALSE;
    wrote = Write(fh, (APTR)data, (LONG)length);
    Close(fh);
    if (wrote != (LONG)length)
    {
        (VOID)DeleteFile((CONST_STRPTR)tmp);
        return FALSE;
    }

    had_old = tool_exists(path);
    if (had_old)
    {
        (VOID)DeleteFile((CONST_STRPTR)old);
        if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)old))
        {
            (VOID)DeleteFile((CONST_STRPTR)tmp);
            return FALSE;
        }
    }
    if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path))
    {
        if (had_old) (VOID)Rename((CONST_STRPTR)old, (CONST_STRPTR)path);
        (VOID)DeleteFile((CONST_STRPTR)tmp);
        return FALSE;
    }
    if (had_old) (VOID)DeleteFile((CONST_STRPTR)old);
    return TRUE;
}

static BOOL append_bytes(char *out, ULONG cap, ULONG *used,
                         const char *data, ULONG length)
{
    ULONG i;
    if (*used + length > cap) return FALSE;
    for (i = 0; i < length; i++) out[(*used)++] = data[i];
    return TRUE;
}

static char *patch_fields(const char *old, ULONG oldlen, NpTextField *fields,
                          ULONG count, ULONG *newlen)
{
    ULONG cap = oldlen + 1024UL;
    char *out = (char *)ami_alloc(cap);
    size_t used = 0;

    if (out == NULL) return NULL;
    if (!np_text_patch(old, oldlen, fields, count, out, cap, &used))
    {
        ami_free(out);
        return NULL;
    }
    *newlen = (ULONG)used;
    return out;
}

static BOOL line_command(const char *line, ULONG len, const char *name,
                         BOOL *commented, BOOL *wildcard)
{
    int was_commented, was_wildcard;
    int matched = np_startup_line(line, len, name, &was_commented, &was_wildcard);
    *commented = was_commented ? TRUE : FALSE;
    *wildcard = was_wildcard ? TRUE : FALSE;
    return matched ? TRUE : FALSE;
}

static BOOL query_boot_state(const char *name, BOOL *enabled,
                             BOOL *by_wildcard, BOOL *by_exact)
{
    ULONG len;
    BOOL exists = tool_exists("S:Network-Startup");
    char *data = read_file("S:Network-Startup", &len, (BOOL)!exists);
    ULONG at = 0;
    BOOL found = FALSE;

    *enabled = FALSE;
    if (by_wildcard != NULL) *by_wildcard = FALSE;
    if (by_exact != NULL) *by_exact = FALSE;
    if (data == NULL) return (BOOL)!exists;
    while (at < len)
    {
        ULONG start = at;
        BOOL commented, wildcard;
        while (at < len && data[at] != '\n') at++;
        if (at < len) at++;
        if (line_command(data + start, at - start, name, &commented, &wildcard) &&
            !commented)
        {
            found = TRUE;
            if (wildcard)
            {
                if (by_wildcard != NULL) *by_wildcard = TRUE;
            }
            else if (by_exact != NULL)
                *by_exact = TRUE;
        }
    }
    ami_free(data);
    *enabled = found;
    return TRUE;
}

static BOOL boot_state(const char *name, BOOL *by_wildcard, BOOL *by_exact)
{
    BOOL enabled;

    if (!query_boot_state(name, &enabled, by_wildcard, by_exact))
        return FALSE;
    return enabled;
}

static BOOL set_boot_state(const char *name, BOOL enabled, BOOL removing)
{
    ULONG len = 0, at = 0, used = 0;
    ULONG extra = text_len(name) + 128;
    BOOL startup_exists = tool_exists("S:Network-Startup");
    char *old = read_file("S:Network-Startup", &len,
                          (BOOL)!startup_exists);
    char *out;
    BOOL exact = FALSE, wildcard_active = FALSE;

    /* An unreadable startup file is not an empty one.  Treating it as empty
       here would replace policy we could not inspect. */
    if (old == NULL && startup_exists) return FALSE;
    if (old == NULL)
    {
        old = (char *)ami_alloc(1);
        if (old == NULL) return FALSE;
        old[0] = '\0';
    }
    out = (char *)ami_alloc(len + extra);
    if (out == NULL) { ami_free(old); return FALSE; }

    while (at < len)
    {
        ULONG start = at;
        BOOL commented, wildcard;
        while (at < len && old[at] != '\n') at++;
        if (at < len) at++;
        if (line_command(old + start, at - start, name, &commented, &wildcard))
        {
            if (wildcard && !commented) wildcard_active = TRUE;
            if (!wildcard && !commented)
            {
                exact = TRUE;
                if (!enabled)
                    if (!append_bytes(out, len + extra, &used, "; ", 2)) goto fail;
            }
            /* A line this tool (or a hand) commented out is the line to give
               back, not a reason to append another: enable/disable cycles
               used to leave one more "; C:AddNetInterface" each time. */
            if (!wildcard && commented && enabled && !exact)
            {
                ULONG p = start;

                exact = TRUE;
                while (p < at && (old[p] == ' ' || old[p] == '\t')) p++;
                if (p < at && old[p] == ';')
                {
                    p++;
                    while (p < at && (old[p] == ' ' || old[p] == '\t')) p++;
                }
                if (!append_bytes(out, len + extra, &used, old + p, at - p)) goto fail;
                continue;
            }
        }
        if (!append_bytes(out, len + extra, &used, old + start, at - start)) goto fail;
    }

    if (!enabled && wildcard_active && !removing)
    {
        requester("S:Network-Startup starts every interface with a wildcard.\n"
                  "NetPrefs will not rewrite that policy implicitly. Edit the\n"
                  "wildcard line first if this one interface must be excluded.");
        goto fail;
    }
    if (enabled && !exact && !wildcard_active)
    {
        if (used != 0 && out[used - 1] != '\n')
            if (!append_bytes(out, len + extra, &used, "\n", 1)) goto fail;
        if (!append_bytes(out, len + extra, &used,
                          "; Added by AmiNetXDuo NetPrefs.\n"
                          "C:AddNetInterface DEVS:NetInterfaces/",
                          text_len("; Added by AmiNetXDuo NetPrefs.\n"
                                   "C:AddNetInterface DEVS:NetInterfaces/")) ||
            !append_bytes(out, len + extra, &used, name, text_len(name)) ||
            !append_bytes(out, len + extra, &used, " QUIET\n", 7)) goto fail;
    }

    if (!replace_file("S:Network-Startup", out, used))
    {
        requester("S:Network-Startup could not be updated.\n"
                  "The interface definition itself is unchanged.");
        goto fail;
    }
    ami_free(out);
    ami_free(old);
    return TRUE;
fail:
    ami_free(out);
    ami_free(old);
    return FALSE;
}

static LONG run_command(const char *command, const char *name, BOOL quiet)
{
    char line[NP_PATH_LEN];
    const char *prefix;
    BPTR input;
    BPTR output;
    LONG result;

    /* In a self-contained install C: deliberately searches the system first.
       Use the sibling commands, not a stale command belonging to another
       stack.  The SYS:Prefs copy has no siblings and therefore uses C:. */
    if (tool_exists("PROGDIR:AddNetInterface"))
        prefix = "PROGDIR:";
    else if (tool_exists("C:AddNetInterface"))
        prefix = "C:";
    else
        prefix = "AmiNetXDuo:C/";

    tool_copy_string(line, sizeof(line), prefix);
    tool_copy_string(line + text_len(line), sizeof(line) - text_len(line), command);
    tool_copy_string(line + text_len(line), sizeof(line) - text_len(line), " ");
    tool_copy_string(line + text_len(line), sizeof(line) - text_len(line), name);
    if (quiet)
        tool_copy_string(line + text_len(line), sizeof(line) - text_len(line),
                         " QUIET");

    /* A Preferences program must not borrow its launching Shell's streams:
       doing so can pull that screen to the front, and Online/Offline have no
       QUIET option.  SystemTagList owns and closes streams once it starts. */
    input = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    output = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (input == (BPTR)0 || output == (BPTR)0)
    {
        if (input != (BPTR)0) Close(input);
        if (output != (BPTR)0) Close(output);
        return -1;
    }
    /* The command runs to completion here -- AddNetInterface with DHCP can
       take a minute -- so the window says so.  The busy pointer is V39. */
    if (np.window != NULL && IntuitionBase->LibNode.lib_Version >= 39)
        SetWindowPointer(np.window, WA_BusyPointer, TRUE, TAG_DONE);
    result = SystemTags((CONST_STRPTR)line,
                        SYS_Input, (ULONG)input,
                        SYS_Output, (ULONG)output,
                        TAG_DONE);
    if (np.window != NULL && IntuitionBase->LibNode.lib_Version >= 39)
        SetWindowPointer(np.window, WA_Pointer, 0, TAG_DONE);
    if (result == -1)
    {
        Close(input);
        Close(output);
    }
    return result;
}

static VOID sort_names(VOID)
{
    ULONG i;
    for (i = 1; i < np.count; i++)
    {
        char held[TOOL_NAME_LEN];
        LONG j = (LONG)i - 1;
        tool_copy_string(held, sizeof(held), np.names[i]);
        while (j >= 0 && tool_stricmp(np.names[j], held) > 0)
        {
            tool_copy_string(np.names[j + 1], TOOL_NAME_LEN, np.names[j]);
            j--;
        }
        tool_copy_string(np.names[j + 1], TOOL_NAME_LEN, held);
    }
}

static VOID scan_interfaces(VOID)
{
    char resolved[AMI_CFG_PATH_LEN];
    const char *dir = ami_cfg_resolve("DEVS:NetInterfaces", resolved,
                                      sizeof(resolved));
    ULONG i;

    /* GadTools may be walking the old list while it repaints; detach it before
       rebuilding the Exec list in place. */
    if (np.window != NULL && np.g_interface != NULL)
        set_attr(np.g_interface, GTLV_Labels, (ULONG)~0UL);
    np.count = tool_list_dir(dir, np.names, NP_MAX_INTERFACES, NULL);
    sort_names();
    /* NewList() is in amiga.lib, which -nostartfiles tools do not link. */
    np.interface_list.lh_Head = (struct Node *)&np.interface_list.lh_Tail;
    np.interface_list.lh_Tail = NULL;
    np.interface_list.lh_TailPred =
        (struct Node *)&np.interface_list.lh_Head;
    for (i = 0; i < np.count; i++)
    {
        np.interface_nodes[i].ln_Name = np.names[i];
        np.interface_nodes[i].ln_Type = 0;
        np.interface_nodes[i].ln_Pri = 0;
        AddTail(&np.interface_list, &np.interface_nodes[i]);
    }
    if (np.window != NULL && np.g_interface != NULL)
        set_attr(np.g_interface, GTLV_Labels, (ULONG)&np.interface_list);
}

static VOID clear_form(VOID)
{
    ULONG i;
    ULONG restore = np.active_panel;

    show_panel(NP_PANEL_GENERAL);
    set_name_locked(FALSE);
    set_attr(np.g_name, GTST_String, (ULONG)"");
    set_attr(np.g_id, GTST_String, (ULONG)"");
    set_attr(np.g_mdns, GTCB_Checked, FALSE);
    set_attr(np.g_state, GTCB_Checked, TRUE);
    set_attr(np.g_boot, GTCB_Checked, FALSE);
    set_attr(np.g_priority, GTIN_Number, 0);
    show_panel(NP_PANEL_DEVICE);
    set_attr(np.g_device, GTST_String, (ULONG)"");
    set_attr(np.g_unit, GTIN_Number, 0);
    set_attr(np.g_card, GTST_String, (ULONG)"");
    set_attr(np.g_hwaddress, GTST_String, (ULONG)"");
    set_attr(np.g_down_offline, GTCB_Checked, FALSE);
    set_attr(np.g_init_delay, GTCB_Checked, FALSE);
    set_attr(np.g_promiscuous, GTCB_Checked, FALSE);
    show_panel(NP_PANEL_IPV4);
    set_attr(np.g_ipv4, GTCY_Active, AMI_IPTYPE_DHCP);
    set_attr(np.g_address, GTST_String, (ULONG)"");
    set_attr(np.g_netmask, GTST_String, (ULONG)"");
    set_attr(np.g_gateway, GTST_String, (ULONG)"");
    set_static_fields(FALSE);
    show_panel(NP_PANEL_IPV6);
    set_attr(np.g_ipv6, GTCY_Active, AMI_IP6TYPE_AUTO);
#ifndef AMINETXDUO_IPV6
    set_attr(np.g_ipv6, GA_Disabled, TRUE);
#endif
    for (i = 0; i < AMI_CFG_MAX_ADDRESS6; i++)
        set_attr(np.g_address6[i], GTST_String, (ULONG)"");
    set_attr(np.g_gateway6, GTST_String, (ULONG)"");
    set_static6_fields(FALSE);
    show_panel(NP_PANEL_TUNING);
    set_attr(np.g_mtu, GTIN_Number, 0);
    set_attr(np.g_rxbuffer, GTIN_Number, 0);
    set_attr(np.g_iprequests, GTIN_Number, 0);
    set_attr(np.g_arprequests, GTIN_Number, 0);
    set_attr(np.g_writerequests, GTIN_Number, 0);
    np.selected = -1;
    set_status("New interface: enter a name and device, then Save.");
    show_panel(restore < NP_PANEL_COUNT ? restore : NP_PANEL_GENERAL);
}

static VOID format_ip6_prefix(const AmiIp6Address *address, char *out,
                              ULONG outlen)
{
    char prefix[12];
    ULONG used;

    ami_config_format_ip6(address->addr, out, outlen);
    used = text_len(out);
    if (used + 2 >= outlen) return;
    out[used++] = '/';
    out[used] = '\0';
    decimal(address->prefix, prefix, sizeof(prefix));
    tool_copy_string(out + used, outlen - used, prefix);
}

static VOID load_form(LONG index)
{
    AmiIfConfig cfg;
    BOOL wildcard;
    ULONG i;
    ULONG restore;

    if (index < 0 || (ULONG)index >= np.count) { clear_form(); return; }
    if (ami_config_load_interface(np.names[index], &cfg) != AMI_CFG_OK)
    {
        set_attr(np.g_interface, GTLV_Selected,
                 np.selected >= 0 ? (ULONG)np.selected : (ULONG)~0UL);
        requester("This interface definition could not be parsed.\n"
                  "Run CheckNetConfig for the exact line and reason.");
        return;
    }
    np.selected = index;
    ami_config_format_ip(cfg.address, np_address, sizeof(np_address));
    ami_config_format_ip(cfg.netmask, np_netmask, sizeof(np_netmask));
    ami_config_format_ip(cfg.gateway, np_gateway, sizeof(np_gateway));
    for (i = 0; i < AMI_CFG_MAX_ADDRESS6; i++)
    {
        if (i < cfg.address6_count)
            format_ip6_prefix(&cfg.address6[i], np_address6[i],
                              sizeof(np_address6[i]));
        else
            np_address6[i][0] = '\0';
    }
    if (cfg.have_gateway6)
        ami_config_format_ip6(cfg.gateway6, np_gateway6, sizeof(np_gateway6));
    else
        np_gateway6[0] = '\0';
    if (cfg.have_hw_address)
        tool_format_mac(cfg.hw_address, np_hwaddress, sizeof(np_hwaddress));
    else
        np_hwaddress[0] = '\0';
    restore = np.active_panel;

    show_panel(NP_PANEL_GENERAL);
    set_attr(np.g_name, GTST_String, (ULONG)cfg.name);
    set_name_locked(TRUE);
    set_attr(np.g_id, GTST_String, (ULONG)cfg.id);
    set_attr(np.g_mdns, GTCB_Checked, (ULONG)cfg.mdns);
    set_attr(np.g_state, GTCB_Checked, (ULONG)cfg.up);
    set_attr(np.g_boot, GTCB_Checked,
             (ULONG)boot_state(cfg.name, &wildcard, NULL));
    set_attr(np.g_priority, GTIN_Number, (ULONG)(LONG)cfg.priority);
    show_panel(NP_PANEL_DEVICE);
    set_attr(np.g_device, GTST_String, (ULONG)cfg.device);
    set_attr(np.g_unit, GTIN_Number, cfg.unit);
    set_attr(np.g_card, GTST_String, (ULONG)cfg.card);
    set_attr(np.g_hwaddress, GTST_String, (ULONG)np_hwaddress);
    set_attr(np.g_down_offline, GTCB_Checked, (ULONG)cfg.down_goes_offline);
    set_attr(np.g_init_delay, GTCB_Checked, (ULONG)cfg.requires_init_delay);
    set_attr(np.g_promiscuous, GTCB_Checked, (ULONG)cfg.promiscuous);
    show_panel(NP_PANEL_IPV4);
    set_attr(np.g_ipv4, GTCY_Active, (ULONG)cfg.iptype);
    set_attr(np.g_address, GTST_String,
             (ULONG)(cfg.address != 0 ? np_address : ""));
    set_attr(np.g_netmask, GTST_String,
             (ULONG)(cfg.netmask != 0 ? np_netmask : ""));
    set_attr(np.g_gateway, GTST_String,
             (ULONG)(cfg.gateway != 0 ? np_gateway : ""));
    set_static_fields((BOOL)(cfg.iptype == AMI_IPTYPE_STATIC));
    show_panel(NP_PANEL_IPV6);
    set_attr(np.g_ipv6, GTCY_Active, (ULONG)cfg.ip6type);
#ifndef AMINETXDUO_IPV6
    set_attr(np.g_ipv6, GA_Disabled, TRUE);
#endif
    for (i = 0; i < AMI_CFG_MAX_ADDRESS6; i++)
        set_attr(np.g_address6[i], GTST_String, (ULONG)np_address6[i]);
    set_attr(np.g_gateway6, GTST_String, (ULONG)np_gateway6);
#ifdef AMINETXDUO_IPV6
    set_static6_fields((BOOL)(cfg.ip6type == AMI_IP6TYPE_STATIC));
#else
    set_static6_fields(FALSE);
#endif
    show_panel(NP_PANEL_TUNING);
    set_attr(np.g_mtu, GTIN_Number, cfg.mtu);
    set_attr(np.g_rxbuffer, GTIN_Number, cfg.rx_buffer);
    set_attr(np.g_iprequests, GTIN_Number, cfg.ip_requests);
    set_attr(np.g_arprequests, GTIN_Number, cfg.arp_requests);
    set_attr(np.g_writerequests, GTIN_Number, cfg.write_requests);
    set_status(wildcard ? "Starts at boot through the all-interface wildcard."
                        : "Loaded. Unknown settings and comments are preserved.");
    show_panel(restore < NP_PANEL_COUNT ? restore : NP_PANEL_GENERAL);
}

static BOOL get_cycle(struct Gadget *g, ULONG *value)
{
    struct TagItem tags[2];
    tags[0].ti_Tag = GTCY_Active; tags[0].ti_Data = (ULONG)value;
    tags[1].ti_Tag = TAG_DONE; tags[1].ti_Data = 0;
    return (BOOL)(GT_GetGadgetAttrsA(g, np.window, NULL, tags) != 0);
}

static BOOL parse_mac(const char *s)
{
    ULONG i;

    if (s == NULL) return FALSE;
    for (i = 0; i < AMI_CFG_MAC_SIZE; i++)
    {
        ULONG digit;
        if (i != 0 && (*s == ':' || *s == '-')) s++;
        for (digit = 0; digit < 2; digit++, s++)
            if (!( (*s >= '0' && *s <= '9') ||
                   (*s >= 'a' && *s <= 'f') ||
                   (*s >= 'A' && *s <= 'F') ))
                return FALSE;
    }
    return (BOOL)(*s == '\0');
}

static BOOL save_form(BOOL apply)
{
    char name[AMI_CFG_NAME_LEN], path[NP_PATH_LEN], resolved[AMI_CFG_PATH_LEN];
    char unit[12], priority[12], mtu[12], rxbuffer[12];
    char iprequests[12], arprequests[12], writerequests[12];
    char *old, *patched;
    ULONG oldlen = 0, newlen = 0, ipv4 = 0, ipv6 = 0;
    LONG pri = integer_value(np.g_priority);
    LONG mtu_n = integer_value(np.g_mtu);
    LONG rxbuffer_n = integer_value(np.g_rxbuffer);
    LONG iprequests_n = integer_value(np.g_iprequests);
    LONG arprequests_n = integer_value(np.g_arprequests);
    LONG writerequests_n = integer_value(np.g_writerequests);
    ULONG address, mask, gateway = 0;
#ifdef AMINETXDUO_IPV6
    ULONG parsed6[AMI_CFG_IP6_WORDS], prefix6;
#endif
    NpTextField *fields;
    BOOL existed, old_boot, wildcard;
    ULONG i, field_count = 0;

    tool_copy_string(name, sizeof(name), string_value(np.g_name));
    if (!sane_name(name))
    {
        select_panel(NP_PANEL_GENERAL);
        requester("Start the interface name with a letter or digit; then use\n"
                  "only letters, digits, dot, underscore or hyphen. The\n"
                  "maximum length is 15 characters.");
        return FALSE;
    }
    if (string_value(np.g_device)[0] == '\0')
    {
        select_panel(NP_PANEL_DEVICE);
        requester("Choose a SANA-II device before saving.");
        return FALSE;
    }
    if (integer_value(np.g_unit) < 0)
    {
        select_panel(NP_PANEL_DEVICE);
        requester("The device unit must not be negative.");
        return FALSE;
    }
    if (pri < -128 || pri > 127)
    {
        select_panel(NP_PANEL_GENERAL);
        requester("Priority must be -128 through 127.");
        return FALSE;
    }
    if (mtu_n < 0 || (mtu_n != 0 && mtu_n < 68) || rxbuffer_n < 0 ||
        iprequests_n < 0 || iprequests_n > AMI_CFG_READREQUESTS_MAX ||
        arprequests_n < 0 || arprequests_n > AMI_CFG_READREQUESTS_MAX ||
        writerequests_n < 0 || writerequests_n > AMI_CFG_WRITEREQUESTS_MAX)
    {
        select_panel(NP_PANEL_TUNING);
        requester("Tuning values must be zero (automatic) or positive. MTU must\n"
                  "be at least 68; read queues may not exceed 128, and the\n"
                  "write queue may not exceed this build's limit.");
        return FALSE;
    }
    if (string_value(np.g_hwaddress)[0] != '\0' &&
        !parse_mac(string_value(np.g_hwaddress)))
    {
        select_panel(NP_PANEL_DEVICE);
        requester("Hardware address must contain exactly six hexadecimal bytes.");
        return FALSE;
    }
    (VOID)get_cycle(np.g_ipv4, &ipv4);
#ifdef AMINETXDUO_IPV6
    (VOID)get_cycle(np.g_ipv6, &ipv6);
    if (ipv6 == AMI_IP6TYPE_STATIC)
    {
        prefix6 = 64;
        if (string_value(np.g_address6[0])[0] == '\0' ||
            !ami_config_parse_ip6(string_value(np.g_address6[0]), parsed6,
                                  &prefix6))
        {
            select_panel(NP_PANEL_IPV6);
            requester("Static IPv6 needs a valid first address, optionally followed by /prefix.");
            return FALSE;
        }
        if (string_value(np.g_address6[1])[0] != '\0')
        {
            prefix6 = 64;
            if (!ami_config_parse_ip6(string_value(np.g_address6[1]), parsed6,
                                      &prefix6))
            {
                select_panel(NP_PANEL_IPV6);
                requester("The second IPv6 address is not valid.");
                return FALSE;
            }
        }
        if (string_value(np.g_gateway6)[0] != '\0' &&
            !ami_config_parse_ip6(string_value(np.g_gateway6), parsed6, NULL))
        {
            select_panel(NP_PANEL_IPV6);
            requester("The IPv6 gateway is not valid.");
            return FALSE;
        }
    }
#else
    (VOID)ipv6;
#endif
    if (ipv4 == AMI_IPTYPE_STATIC)
    {
        if (!ami_config_parse_ip(string_value(np.g_address), &address) ||
            address == 0 || !ami_config_parse_ip(string_value(np.g_netmask), &mask) ||
            mask == 0 || (((~mask) & ((~mask) + 1UL)) != 0))
        {
            select_panel(NP_PANEL_IPV4);
            requester("A static IPv4 interface needs a valid address and netmask.");
            return FALSE;
        }
        if (string_value(np.g_gateway)[0] != '\0' &&
            (!ami_config_parse_ip(string_value(np.g_gateway), &gateway) ||
             gateway == 0))
        {
            select_panel(NP_PANEL_IPV4);
            requester("The gateway is not a valid IPv4 address.");
            return FALSE;
        }
        if (gateway != 0 && (gateway & mask) != (address & mask) &&
            !confirm("The gateway is outside this interface's network.\n"
                     "That is usually a typing error. Save it anyway?", "Save"))
            return FALSE;
    }

    decimal((ULONG)integer_value(np.g_unit), unit, sizeof(unit));
    signed_decimal(pri, priority, sizeof(priority));
    decimal((ULONG)mtu_n, mtu, sizeof(mtu));
    decimal((ULONG)rxbuffer_n, rxbuffer, sizeof(rxbuffer));
    decimal((ULONG)iprequests_n, iprequests, sizeof(iprequests));
    decimal((ULONG)arprequests_n, arprequests, sizeof(arprequests));
    decimal((ULONG)writerequests_n, writerequests, sizeof(writerequests));
    fields = (NpTextField *)ami_alloc(NP_SAVE_FIELDS * sizeof(*fields));
    if (fields == NULL)
    {
        requester("There is not enough free memory to prepare the new definition.");
        return FALSE;
    }
#define FIELD(key_, value_) do { \
    fields[field_count].key = (key_); \
    fields[field_count].value = (value_); \
    fields[field_count].seen = FALSE; \
    field_count++; \
} while (0)
    FIELD("DEVICE", string_value(np.g_device));
    FIELD("UNIT", unit);
    FIELD("CARD", string_value(np.g_card)[0] != '\0'
                  ? string_value(np.g_card) : NULL);
    FIELD("ID", string_value(np.g_id)[0] != '\0'
                ? string_value(np.g_id) : NULL);
    FIELD("CONFIGURE", ipv4 == AMI_IPTYPE_DHCP ? "DHCP" :
                       ipv4 == AMI_IPTYPE_LINKLOCAL ? "LINKLOCAL" :
                       ipv4 == AMI_IPTYPE_NONE ? "NONE" : "STATIC");
    FIELD("ADDRESS", ipv4 == AMI_IPTYPE_STATIC
                     ? string_value(np.g_address) : NULL);
    FIELD("NETMASK", ipv4 == AMI_IPTYPE_STATIC
                     ? string_value(np.g_netmask) : NULL);
    FIELD("GATEWAY", ipv4 == AMI_IPTYPE_STATIC && gateway != 0
                     ? string_value(np.g_gateway) : NULL);
#ifdef AMINETXDUO_IPV6
    FIELD("CONFIGURE6", ipv6 == AMI_IP6TYPE_LINKLOCAL ? "LINKLOCAL" :
                        ipv6 == AMI_IP6TYPE_AUTO ? "AUTO" :
                        ipv6 == AMI_IP6TYPE_STATIC ? "STATIC" :
                        ipv6 == AMI_IP6TYPE_DHCP ? "DHCP" : "OFF");
    FIELD("ADDRESS6", ipv6 == AMI_IP6TYPE_STATIC
                      ? string_value(np.g_address6[0]) : NULL);
    FIELD("ADDRESS6", ipv6 == AMI_IP6TYPE_STATIC &&
                      string_value(np.g_address6[1])[0] != '\0'
                      ? string_value(np.g_address6[1]) : NULL);
    FIELD("GATEWAY6", ipv6 == AMI_IP6TYPE_STATIC &&
                      string_value(np.g_gateway6)[0] != '\0'
                      ? string_value(np.g_gateway6) : NULL);
#endif
    FIELD("MDNS", checked(np.g_mdns) ? "YES" : "NO");
    FIELD("PRIORITY", priority);
    FIELD("STATE", checked(np.g_state) ? "UP" : "DOWN");
    FIELD("HARDWAREADDRESS", string_value(np.g_hwaddress)[0] != '\0'
                              ? string_value(np.g_hwaddress) : NULL);
    FIELD("DOWNGOESOFFLINE", checked(np.g_down_offline) ? "YES" : NULL);
    FIELD("REQUIRESINITDELAY", checked(np.g_init_delay) ? "YES" : NULL);
    FIELD("FILTER", checked(np.g_promiscuous) ? "EVERYTHING" : NULL);
    FIELD("MTU", mtu_n != 0 ? mtu : NULL);
    FIELD("RXBUFFER", rxbuffer_n != 0 ? rxbuffer : NULL);
    FIELD("IPREQUESTS", iprequests_n != 0 ? iprequests : NULL);
    FIELD("ARPREQUESTS", arprequests_n != 0 ? arprequests : NULL);
    FIELD("WRITEREQUESTS", writerequests_n != 0 ? writerequests : NULL);
#undef FIELD

    tool_copy_string(path, sizeof(path),
        ami_cfg_resolve("DEVS:NetInterfaces", resolved, sizeof(resolved)));
    if (!ensure_dir(path))
    {
        ami_free(fields);
        requester("DEVS:NetInterfaces could not be created.");
        return FALSE;
    }
    tool_copy_string(path + text_len(path), sizeof(path) - text_len(path), "/");
    tool_copy_string(path + text_len(path), sizeof(path) - text_len(path), name);
    existed = tool_exists(path);
    old = read_file(path, &oldlen, (BOOL)!existed);
    /* Do not flatten an existing file merely because it became unreadable. */
    if (old == NULL && existed)
    {
        ami_free(fields);
        return FALSE;
    }
    if (old == NULL)
    {
        static const char heading[] =
            "# Network interface, written by AmiNetXDuo NetPrefs.\n"
            "# Unknown keywords and comments are preserved when edited.\n\n";
        oldlen = sizeof(heading) - 1;
        old = (char *)ami_alloc(oldlen + 1);
        if (old == NULL)
        {
            ami_free(fields);
            return FALSE;
        }
        for (i = 0; i <= oldlen; i++) old[i] = heading[i];
    }
    patched = patch_fields(old, oldlen, fields, field_count, &newlen);
    ami_free(fields);
    ami_free(old);
    if (patched == NULL)
    {
        requester("There is not enough free memory to prepare the new definition.");
        return FALSE;
    }
    if (!replace_file(path, patched, newlen))
    {
        ami_free(patched);
        requester("The interface file could not be replaced. The old file was kept.");
        return FALSE;
    }
    ami_free(patched);

    old_boot = boot_state(name, &wildcard, NULL);
    if (old_boot != checked(np.g_boot) &&
        !set_boot_state(name, checked(np.g_boot), FALSE))
    {
        requester("The definition was saved, but its boot setting was not changed.");
        return FALSE;
    }
    scan_interfaces();
    for (i = 0; i < np.count; i++)
    {
        if (tool_stricmp(np.names[i], name) == 0)
        {
            np.selected = (LONG)i;
            set_attr(np.g_interface, GTLV_Selected, i);
            set_name_locked(TRUE);
            break;
        }
    }

    if (apply)
    {
        if (existed) (VOID)run_command("RemoveNetInterface", name, TRUE);
        if (run_command("AddNetInterface", name, TRUE) != 0)
        {
            requester("The definition was saved, but the interface did not start.\n"
                      "Run AddNetInterface from Shell to see the detailed reason.");
            return FALSE;
        }
        set_status("Saved and applied. The interface was restarted.");
    }
    else
        set_status("Saved. Use Save & Start to restart with these settings.");
    return TRUE;
}

static BOOL parked_path_for(const char *path, char *parked, ULONG parked_len)
{
    ULONG number;

    tool_copy_string(parked, parked_len, path);
    tool_copy_string(parked + text_len(parked), parked_len - text_len(parked),
                     ".disabled.info");
    if (!tool_exists(parked)) return TRUE;

    for (number = 1; number < 1000; number++)
    {
        char digits[12];
        tool_copy_string(parked, parked_len, path);
        tool_copy_string(parked + text_len(parked),
                         parked_len - text_len(parked), ".disabled.");
        decimal(number, digits, sizeof(digits));
        tool_copy_string(parked + text_len(parked),
                         parked_len - text_len(parked), digits);
        tool_copy_string(parked + text_len(parked),
                         parked_len - text_len(parked), ".info");
        if (!tool_exists(parked)) return TRUE;
    }
    return FALSE;
}

static VOID restore_live_interface(const char *name, LONG prior_state)
{
    LONG current;
    const char *command = NULL;

    if (run_command("AddNetInterface", name, TRUE) != 0)
    {
        requester("The definition was restored, but its live interface could\n"
                  "not be restored. Run AddNetInterface from Shell.");
        return;
    }

    current = live_state_of(name);
    if (prior_state == NP_LIVE_ONLINE && current == NP_LIVE_OFFLINE)
        command = "Online";
    else if (prior_state == NP_LIVE_OFFLINE && current == NP_LIVE_ONLINE)
        command = "Offline";
    if (command != NULL && run_command(command, name, FALSE) != 0)
        requester("The definition and interface were restored, but its prior\n"
                  "online state could not be restored.");
}

static VOID remove_form(VOID)
{
    char name[AMI_CFG_NAME_LEN], path[NP_PATH_LEN], parked[NP_PATH_LEN];
    char resolved[AMI_CFG_PATH_LEN];
    BOOL boot_enabled, exact;
    BOOL detached = FALSE;
    LONG prior_state;

    tool_copy_string(name, sizeof(name), string_value(np.g_name));
    if (!sane_name(name)) return;
    tool_copy_string(path, sizeof(path),
                     ami_cfg_resolve("DEVS:NetInterfaces", resolved, sizeof(resolved)));
    tool_copy_string(path + text_len(path), sizeof(path) - text_len(path), "/");
    tool_copy_string(path + text_len(path), sizeof(path) - text_len(path), name);
    if (!tool_exists(path))
    {
        requester("Save this interface before removing it.");
        return;
    }
    if (!parked_path_for(path, parked, sizeof(parked)))
    {
        requester("There are too many parked backups for this interface.\n"
                  "Move or remove an old .disabled.*.info file first.");
        return;
    }
    if (!confirm("Remove this interface definition?\n\n"
                 "It will be taken offline and parked as an ignored .info\n"
                 "backup, not deleted.", "Remove")) return;
    if (!query_boot_state(name, &boot_enabled, NULL, &exact)) return;

    prior_state = live_state_of(name);
    if (prior_state == NP_LIVE_UNAVAILABLE ||
        prior_state == NP_LIVE_UNKNOWN || prior_state == NP_LIVE_NOT_SAVED)
    {
        requester("The live interface state could not be checked, so nothing\n"
                  "was changed. Try again after checking ShowNetStatus.");
        return;
    }
    if (prior_state == NP_LIVE_ONLINE || prior_state == NP_LIVE_OFFLINE)
    {
        if (run_command("RemoveNetInterface", name, TRUE) != 0)
        {
            requester("The live interface could not be removed, so nothing\n"
                      "was changed. Close its connections or run\n"
                      "RemoveNetInterface from Shell for the exact reason.");
            return;
        }
        detached = TRUE;
    }

    if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)parked))
    {
        if (detached) restore_live_interface(name, prior_state);
        requester("The interface could not be parked; its definition was kept.");
        return;
    }

    /* A wildcard is policy for the whole drawer and must remain untouched.
       Only an active, exact line for this definition needs changing. */
    if (boot_enabled && exact && !set_boot_state(name, FALSE, TRUE))
    {
        if (!Rename((CONST_STRPTR)parked, (CONST_STRPTR)path))
        {
            requester("The boot setting was not changed, but the definition\n"
                      "could not be restored. Its parked .info backup is intact.");
            return;
        }
        if (detached) restore_live_interface(name, prior_state);
        return;
    }
    scan_interfaces();
    set_attr(np.g_interface, GTLV_Selected, (ULONG)~0UL);
    clear_form();
    set_status("Removed. The definition remains beside the drawer as a .disabled*.info backup.");
}

static LONG live_state_of(const char *name)
{
    NetStatusHeader    *answer;
    NetStatusInterface *entries;
    struct Library     *base;
    ULONG               size;
    LONG                count;
    LONG                i;
    LONG                state = NP_LIVE_NOT_ADDED;

    if (!sane_name(name)) return NP_LIVE_NOT_SAVED;
    if (!tool_stack_library_running()) return NP_LIVE_STACK_STOPPED;

    base = tool_netstatus_open(TRUE);
    if (base == NULL) return NP_LIVE_UNAVAILABLE;

    size = sizeof(NetStatusHeader) +
           (ULONG)NX_MAX_PHYSICAL_INTERFACES * sizeof(NetStatusInterface);
    answer = (NetStatusHeader *)ami_alloc(size);
    if (answer == NULL)
    {
        tool_netstatus_close(base);
        return NP_LIVE_UNAVAILABLE;
    }

    count = tool_netstatus_query(base, NETSTATUS_INTERFACES, answer, size,
                                 sizeof(NetStatusInterface));
    if (count < 0)
        state = NP_LIVE_UNAVAILABLE;
    else
    {
        entries = (NetStatusInterface *)NETSTATUS_ENTRIES(answer);
        for (i = 0;
             i < count && i < (LONG)NX_MAX_PHYSICAL_INTERFACES;
             i++)
        {
            if (!(entries[i].nsi_Flags & NETSTATUS_IF_NAMED) ||
                tool_stricmp(entries[i].nsi_Name, name) != 0)
                continue;
            state = (entries[i].nsi_Flags & NETSTATUS_IF_LINKUP)
                  ? NP_LIVE_ONLINE : NP_LIVE_OFFLINE;
            break;
        }
    }

    ami_free(answer);
    tool_netstatus_close(base);
    return state;
}

static VOID update_live_state(VOID)
{
    static const char *const status_text[] =
    {
        "New", "Stack off", "Not added", "Offline", "Online", "Unknown"
    };
    struct Gadget *action;
    LONG state;

    if (np.window == NULL) return;
    state = live_state_of(string_value(np.g_name));
    if (state == np.live_state) return;

    np.live_state = state;
    set_attr(np.g_live_status, GTTX_Text, (ULONG)status_text[state]);
    action = state == NP_LIVE_ONLINE ? np.g_live_offline : np.g_live_online;
    if (action != np.g_live_action)
    {
        if (np.g_live_action != NULL)
        {
            (VOID)RemoveGList(np.window, np.g_live_action, 1);
            np.g_live_action->NextGadget = NULL;
        }
        np.g_live_action = action;
        (VOID)AddGList(np.window, action, (UWORD)-1, 1, NULL);
    }
    set_attr(action, GA_Disabled,
             (ULONG)(state != NP_LIVE_OFFLINE && state != NP_LIVE_ONLINE));
    RefreshGList(action, np.window, NULL, 1);
}

static struct Gadget *add_gadget(ULONG kind, struct Gadget *previous,
                                 UWORD id, WORD x, WORD y, WORD w, WORD h,
                                 const char *label, ULONG flags,
                                 struct TagItem *tags)
{
    struct NewGadget ng;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y;
    ng.ng_Width = w; ng.ng_Height = h;
    ng.ng_GadgetText = (STRPTR)label;
    ng.ng_TextAttr = np.screen->Font;
    ng.ng_GadgetID = id; ng.ng_Flags = flags;
    ng.ng_VisualInfo = np.visual; ng.ng_UserData = NULL;
    return CreateGadgetA(kind, previous, &ng, tags);
}

static LONG gadget_count(struct Gadget *g)
{
    LONG count = 0;

    while (g != NULL)
    {
        count++;
        g = g->NextGadget;
    }
    return count;
}

static VOID detach_panel(ULONG which)
{
    struct Gadget *tail;
    LONG i;

    if (np.window == NULL || which >= NP_PANEL_COUNT) return;
    (VOID)RemoveGList(np.window, np.panel_gadgets[which],
                     np.panel_gadget_count[which]);

    /* The list is allocated and freed independently.  Restore that ownership
       boundary even on Intuition versions which retain the old successor in
       the removed tail's NextGadget link. */
    tail = np.panel_gadgets[which];
    for (i = 1; tail != NULL && i < np.panel_gadget_count[which]; i++)
        tail = tail->NextGadget;
    if (tail != NULL) tail->NextGadget = NULL;
}

/* GadTools has no page gadget.  The common controls and each page therefore
 * have separate gadget lists; show_panel() removes one list and attaches the
 * next.  The recessed panel and separator are window decoration and must be
 * restored after Intuition refreshes the window. */
static VOID draw_layout(VOID)
{
    struct DrawInfo *dri;
    struct RastPort *rp;
    struct TagItem tags[3];
    BYTE old_fg, old_bg, old_mode;

    if (np.window == NULL) return;
    rp = np.window->RPort;
    old_fg = rp->FgPen;
    old_bg = rp->BgPen;
    old_mode = rp->DrawMode;
    tags[0].ti_Tag = GT_VisualInfo;
    tags[0].ti_Data = (ULONG)np.visual;
    tags[1].ti_Tag = GTBB_Recessed;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_DONE;
    tags[2].ti_Data = 0;

    DrawBevelBoxA(rp, 166, 25, 456, 108, tags);

    /* A two-pixel shadow/shine pair is a proper recessed separator.  A
       two-pixel bevel box collapses to one visible edge on classic GadTools. */
    dri = GetScreenDrawInfo(np.screen);
    if (dri != NULL)
    {
        SetAPen(rp, dri->dri_Pens[SHADOWPEN]);
        Move(rp, 156, 5);
        Draw(rp, 156, 152);
        SetAPen(rp, dri->dri_Pens[SHINEPEN]);
        Move(rp, 157, 5);
        Draw(rp, 157, 152);
        if (np.active_panel == NP_PANEL_TUNING)
        {
            SetAPen(rp, dri->dri_Pens[TEXTPEN]);
            SetDrMd(rp, JAM1);
            Move(rp, 430, 106);
            Text(rp, (CONST_STRPTR)"0 = automatic", 13);
        }
        FreeScreenDrawInfo(np.screen, dri);
    }
    SetAPen(rp, (ULONG)(UBYTE)old_fg);
    SetBPen(rp, (ULONG)(UBYTE)old_bg);
    SetDrMd(rp, (ULONG)(UBYTE)old_mode);
}

static VOID show_panel(ULONG which)
{
    struct RastPort *rp;

    if (np.window == NULL || which >= NP_PANEL_COUNT ||
        which == np.active_panel) return;
    if (np.active_panel < NP_PANEL_COUNT)
        detach_panel(np.active_panel);

    rp = np.window->RPort;
    SetAPen(rp, rp->BgPen);
    RectFill(rp, 164, 23, 623, 134);
    np.active_panel = which;
    draw_layout();
    (VOID)AddGList(np.window, np.panel_gadgets[which], (UWORD)-1,
                   np.panel_gadget_count[which], NULL);
    RefreshGList(np.panel_gadgets[which], np.window, NULL,
                 np.panel_gadget_count[which]);
}

static VOID select_panel(ULONG which)
{
    if (which >= NP_PANEL_COUNT) return;
    set_attr(np.g_panel, GTCY_Active, which);
    show_panel(which);
}

static BOOL make_window(VOID)
{
    struct Gadget *context, *g;
    struct TagItem tags[5];
    struct TagItem win[12];
    ULONG i;
    WORD width, height;

    np.screen = LockPubScreen(NULL);
    if (np.screen == NULL) return FALSE;
    /* Fit the stock 640x200 NTSC Workbench as well as PAL and taller modes. */
    if (np.screen->Width < 640 || np.screen->Height < 200) return FALSE;
    np.visual = GetVisualInfoA(np.screen, NULL);
    if (np.visual == NULL) return FALSE;
    context = CreateContext(&np.gadgets);
    if (context == NULL) return FALSE;
    g = context;

#define TAG1(a,b) do { tags[0].ti_Tag=(a); tags[0].ti_Data=(ULONG)(b); \
    tags[1].ti_Tag=TAG_DONE; tags[1].ti_Data=0; } while (0)
#define ADD(var,kind,id,x,y,w,h,label,flags) do { \
    (var)=add_gadget((kind),g,(id),(x),(y),(w),(h),(label),(flags),tags); \
    if ((var)==NULL) { return FALSE; } \
    g=(var); \
} while (0)

    tags[0].ti_Tag=GTLV_Labels; tags[0].ti_Data=(ULONG)&np.interface_list;
    tags[1].ti_Tag=GTLV_Selected;
    tags[1].ti_Data=np.count != 0 ? 0 : (ULONG)~0UL;
    tags[2].ti_Tag=GTLV_ScrollWidth; tags[2].ti_Data=16;
    tags[3].ti_Tag=TAG_DONE; tags[3].ti_Data=0;
    ADD(np.g_interface,LISTVIEW_KIND,GID_INTERFACE,8,20,142,112,
        "Interfaces",PLACETEXT_ABOVE);
    TAG1(TAG_DONE, 0);
    ADD(g,BUTTON_KIND,GID_NEW,8,137,66,15,"New",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_REMOVE,82,137,68,15,"Remove",PLACETEXT_IN);
    tags[0].ti_Tag=GTCY_Labels; tags[0].ti_Data=(ULONG)panel_labels;
    tags[1].ti_Tag=GTCY_Active; tags[1].ti_Data=NP_PANEL_GENERAL;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(np.g_panel,CYCLE_KIND,GID_PANEL,236,5,170,15,"Settings",PLACETEXT_LEFT);
    tags[0].ti_Tag=GTTX_Text; tags[0].ti_Data=(ULONG)"Loading definitions...";
    tags[1].ti_Tag=GTTX_Border; tags[1].ti_Data=TRUE;
    tags[2].ti_Tag=GTTX_CopyText; tags[2].ti_Data=FALSE;
    tags[3].ti_Tag=TAG_DONE; tags[3].ti_Data=0;
    ADD(np.g_status,TEXT_KIND,GID_STATUS,166,137,456,15,NULL,0);
    TAG1(TAG_DONE, 0);
    ADD(g,BUTTON_KIND,GID_SAVE,8,157,66,17,"Save",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_APPLY,82,157,116,17,"Save & Start",PLACETEXT_IN);
    tags[0].ti_Tag=GTTX_Text; tags[0].ti_Data=(ULONG)"Unknown";
    tags[1].ti_Tag=GTTX_Border; tags[1].ti_Data=TRUE;
    tags[2].ti_Tag=GTTX_CopyText; tags[2].ti_Data=FALSE;
    tags[3].ti_Tag=GTTX_Justification; tags[3].ti_Data=GTJ_CENTER;
    tags[4].ti_Tag=TAG_DONE; tags[4].ti_Data=0;
    ADD(np.g_live_status,TEXT_KIND,0,206,158,144,15,NULL,0);
    TAG1(TAG_DONE, 0);
    ADD(g,BUTTON_KIND,GID_CLOSE,552,157,70,17,"Close",PLACETEXT_IN);

    /* General page. */
    context = CreateContext(&np.panel_gadgets[NP_PANEL_GENERAL]);
    if (context == NULL) return FALSE;
    g = context;
    TAG1(GTST_MaxChars, AMI_CFG_NAME_LEN - 1);
    ADD(np.g_name,STRING_KIND,GID_NAME,250,37,350,15,"Name",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_NAME_LEN - 1);
    ADD(np.g_id,STRING_KIND,GID_ID,250,59,350,15,"ID",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 4);
    ADD(np.g_priority,INTEGER_KIND,GID_PRIORITY,250,81,70,15,
        "Priority",PLACETEXT_LEFT);
    TAG1(GTCB_Checked, TRUE);
    ADD(np.g_state,CHECKBOX_KIND,GID_STATE,350,83,CHECKBOX_WIDTH,CHECKBOX_HEIGHT,
        "Start online",PLACETEXT_RIGHT);
    TAG1(GTCB_Checked, FALSE);
    ADD(np.g_boot,CHECKBOX_KIND,GID_BOOT,500,83,CHECKBOX_WIDTH,CHECKBOX_HEIGHT,
        "At boot",PLACETEXT_RIGHT);
    ADD(np.g_mdns,CHECKBOX_KIND,GID_MDNS,250,107,CHECKBOX_WIDTH,CHECKBOX_HEIGHT,
        "mDNS",PLACETEXT_RIGHT);

    /* IPv4 page. */
    context = CreateContext(&np.panel_gadgets[NP_PANEL_IPV4]);
    if (context == NULL) return FALSE;
    g = context;
    tags[0].ti_Tag=GTCY_Labels; tags[0].ti_Data=(ULONG)ipv4_labels;
    tags[1].ti_Tag=GTCY_Active; tags[1].ti_Data=AMI_IPTYPE_DHCP;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(np.g_ipv4,CYCLE_KIND,GID_IPV4,250,37,180,15,"Mode",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, 15);
    ADD(np.g_address,STRING_KIND,GID_ADDRESS,250,59,350,15,"Address",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, 15);
    ADD(np.g_netmask,STRING_KIND,GID_NETMASK,250,81,350,15,"Netmask",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, 15);
    ADD(np.g_gateway,STRING_KIND,GID_GATEWAY,250,103,350,15,"Gateway",PLACETEXT_LEFT);

    /* IPv6 page. */
    context = CreateContext(&np.panel_gadgets[NP_PANEL_IPV6]);
    if (context == NULL) return FALSE;
    g = context;
    tags[0].ti_Tag=GTCY_Labels; tags[0].ti_Data=(ULONG)ipv6_labels;
    tags[1].ti_Tag=GTCY_Active; tags[1].ti_Data=AMI_IP6TYPE_AUTO;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(np.g_ipv6,CYCLE_KIND,GID_IPV6,250,37,180,15,"Mode",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_IP6_STRLEN + 3);
    ADD(np.g_address6[0],STRING_KIND,GID_ADDRESS6_1,250,59,350,15,
        "Address 1",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_IP6_STRLEN + 3);
    ADD(np.g_address6[1],STRING_KIND,GID_ADDRESS6_2,250,81,350,15,
        "Address 2",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_IP6_STRLEN - 1);
    ADD(np.g_gateway6,STRING_KIND,GID_GATEWAY6,250,103,350,15,
        "Gateway",PLACETEXT_LEFT);

    /* Device page. */
    context = CreateContext(&np.panel_gadgets[NP_PANEL_DEVICE]);
    if (context == NULL) return FALSE;
    g = context;
    TAG1(GTST_MaxChars, AMI_CFG_PATH_LEN - 1);
    ADD(np.g_device,STRING_KIND,GID_DEVICE,250,37,350,15,"Device",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 3);
    ADD(np.g_unit,INTEGER_KIND,GID_UNIT,250,59,60,15,"Unit",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_NAME_LEN - 1);
    ADD(np.g_card,STRING_KIND,GID_CARD,390,59,210,15,"Card",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, 17);
    ADD(np.g_hwaddress,STRING_KIND,GID_HWADDRESS,250,81,350,15,
        "MAC",PLACETEXT_LEFT);
    TAG1(GTCB_Checked, FALSE);
    ADD(np.g_down_offline,CHECKBOX_KIND,GID_DOWN_OFFLINE,178,107,
        CHECKBOX_WIDTH,CHECKBOX_HEIGHT,"Offline on down",PLACETEXT_RIGHT);
    ADD(np.g_init_delay,CHECKBOX_KIND,GID_INIT_DELAY,342,107,
        CHECKBOX_WIDTH,CHECKBOX_HEIGHT,"Init delay",PLACETEXT_RIGHT);
    ADD(np.g_promiscuous,CHECKBOX_KIND,GID_PROMISCUOUS,478,107,
        CHECKBOX_WIDTH,CHECKBOX_HEIGHT,"Promiscuous",PLACETEXT_RIGHT);

    /* Tuning page. Zero means automatic for every value. */
    context = CreateContext(&np.panel_gadgets[NP_PANEL_TUNING]);
    if (context == NULL) return FALSE;
    g = context;
    TAG1(GTIN_MaxChars, 6);
    ADD(np.g_mtu,INTEGER_KIND,GID_MTU,250,37,90,15,"MTU",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 10);
    ADD(np.g_rxbuffer,INTEGER_KIND,GID_RXBUFFER,500,37,100,15,
        "RX buffer",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 3);
    ADD(np.g_iprequests,INTEGER_KIND,GID_IPREQUESTS,250,66,90,15,
        "IP reads",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 3);
    ADD(np.g_arprequests,INTEGER_KIND,GID_ARPREQUESTS,500,66,100,15,
        "ARP reads",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 3);
    ADD(np.g_writerequests,INTEGER_KIND,GID_WRITEREQUESTS,250,95,90,15,
        "Writes",PLACETEXT_LEFT);

    for (i = 0; i < NP_PANEL_COUNT; i++)
        np.panel_gadget_count[i] = gadget_count(np.panel_gadgets[i]);

    /* BUTTON_KIND has no supported tag for changing its caption.  Keep the
       two contextual actions as separate one-gadget lists and attach only
       the one whose label describes the operation currently available. */
    context = CreateContext(&np.live_online_gadgets);
    if (context == NULL) return FALSE;
    g = context;
    TAG1(GA_Disabled, TRUE);
    ADD(np.g_live_online,BUTTON_KIND,GID_LIVE_ACTION,358,157,90,17,
        "Online",PLACETEXT_IN);
    context = CreateContext(&np.live_offline_gadgets);
    if (context == NULL) return FALSE;
    g = context;
    TAG1(GA_Disabled, TRUE);
    ADD(np.g_live_offline,BUTTON_KIND,GID_LIVE_ACTION,358,157,90,17,
        "Offline",PLACETEXT_IN);

#undef ADD
#undef TAG1

    width = 640;
    height = 190;
    win[0].ti_Tag=WA_Left; win[0].ti_Data=(np.screen->Width-width)/2;
    win[1].ti_Tag=WA_Top; win[1].ti_Data=(np.screen->Height-height)/2;
    win[2].ti_Tag=WA_Width; win[2].ti_Data=width;
    win[3].ti_Tag=WA_Height; win[3].ti_Data=height;
    win[4].ti_Tag=WA_Title; win[4].ti_Data=(ULONG)"AmiNetXDuo Network Preferences";
    win[5].ti_Tag=WA_IDCMP; win[5].ti_Data=IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW|
        BUTTONIDCMP|CHECKBOXIDCMP|CYCLEIDCMP|STRINGIDCMP|LISTVIEWIDCMP|
        IDCMP_INTUITICKS;
    win[6].ti_Tag=WA_Flags; win[6].ti_Data=WFLG_DRAGBAR|WFLG_DEPTHGADGET|
        WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SMART_REFRESH|WFLG_GIMMEZEROZERO;
    win[7].ti_Tag=WA_Gadgets; win[7].ti_Data=(ULONG)np.gadgets;
    win[8].ti_Tag=WA_PubScreen; win[8].ti_Data=(ULONG)np.screen;
    win[9].ti_Tag=WA_AutoAdjust; win[9].ti_Data=TRUE;
    win[10].ti_Tag=TAG_DONE; win[10].ti_Data=0;
    np.window = OpenWindowTagList(NULL, win);
    if (np.window == NULL) return FALSE;
    GT_RefreshWindow(np.window, NULL);
    np.active_panel = NP_PANEL_COUNT;
    show_panel(NP_PANEL_GENERAL);
    return TRUE;
}

static VOID close_ui(VOID)
{
    if (np.window != NULL && np.active_panel < NP_PANEL_COUNT)
        detach_panel(np.active_panel);
    if (np.window != NULL && np.g_live_action != NULL)
    {
        (VOID)RemoveGList(np.window, np.g_live_action, 1);
        np.g_live_action->NextGadget = NULL;
    }
    if (np.window != NULL) CloseWindow(np.window);
    np.window = NULL;
    if (np.gadgets != NULL) FreeGadgets(np.gadgets);
    np.gadgets = NULL;
    {
        ULONG i;
        for (i = 0; i < NP_PANEL_COUNT; i++)
        {
            if (np.panel_gadgets[i] != NULL) FreeGadgets(np.panel_gadgets[i]);
            np.panel_gadgets[i] = NULL;
        }
    }
    if (np.live_online_gadgets != NULL) FreeGadgets(np.live_online_gadgets);
    np.live_online_gadgets = NULL;
    if (np.live_offline_gadgets != NULL) FreeGadgets(np.live_offline_gadgets);
    np.live_offline_gadgets = NULL;
    np.g_live_action = NULL;
    if (np.visual != NULL) FreeVisualInfo(np.visual);
    np.visual = NULL;
    if (np.screen != NULL) UnlockPubScreen(NULL, np.screen);
    np.screen = NULL;
}

static VOID event_loop(VOID)
{
    BOOL done = FALSE;
    UWORD ticks = 0;

    while (!done)
    {
        struct IntuiMessage *msg;
        Wait(1UL << np.window->UserPort->mp_SigBit);
        while ((msg = GT_GetIMsg(np.window->UserPort)) != NULL)
        {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            struct Gadget *g = (struct Gadget *)msg->IAddress;
            UWORD id = (g != NULL) ? g->GadgetID : 0;
            GT_ReplyIMsg(msg);

            if (cls == IDCMP_CLOSEWINDOW) done = TRUE;
            else if (cls == IDCMP_REFRESHWINDOW)
            {
                GT_BeginRefresh(np.window);
                GT_EndRefresh(np.window, TRUE);
                GT_RefreshWindow(np.window, NULL);
                draw_layout();
            }
            else if (cls == IDCMP_INTUITICKS)
            {
                /* IntuiTicks arrive roughly ten times a second.  A five-second
                   poll keeps external Online/Offline commands visible without
                   continually opening the status interface. */
                if (++ticks >= 50)
                {
                    ticks = 0;
                    update_live_state();
                }
            }
            else if (cls == IDCMP_GADGETUP)
            {
                const char *name = string_value(np.g_name);
                switch (id)
                {
                    case GID_INTERFACE:
                        load_form((LONG)code);
                        break;
                    case GID_PANEL:
                        show_panel((ULONG)code);
                        break;
                    case GID_NEW:
                        set_attr(np.g_interface, GTLV_Selected, (ULONG)~0UL);
                        clear_form();
                        break;
                    case GID_IPV4:
                        set_static_fields((BOOL)(code == AMI_IPTYPE_STATIC));
                        break;
                    case GID_IPV6:
                        set_static6_fields((BOOL)(code == AMI_IP6TYPE_STATIC));
                        break;
                    case GID_SAVE: (VOID)save_form(FALSE); break;
                    case GID_APPLY: (VOID)save_form(TRUE); break;
                    case GID_LIVE_ACTION:
                        update_live_state();
                        if (np.live_state == NP_LIVE_OFFLINE &&
                            run_command("Online", name, FALSE) == 0)
                            set_status("Interface is online.");
                        else if (np.live_state == NP_LIVE_ONLINE &&
                                 run_command("Offline", name, FALSE) == 0)
                            set_status("Interface is offline.");
                        else if (np.live_state == NP_LIVE_OFFLINE ||
                                 np.live_state == NP_LIVE_ONLINE)
                            requester("The state change failed. Run Online or Offline from Shell for details.");
                        break;
                    case GID_REMOVE: remove_form(); break;
                    case GID_CLOSE: done = TRUE; break;
                    default: break;
                }
                update_live_state();
            }
        }
    }
}

/*
 * Intuition, GadTools and System() run on the caller's stack, and a Shell
 * gives a command 4,096 bytes: the stack-frame gate sees the editor's own
 * calls but not an EasyRequest or a GadTools refresh on top of them, silently,
 * without an MMU.  The icon asks for 8 KB; a Shell does not.  Eight KB is
 * already sufficient; below that the editor runs
 * on its own 16 KB stack -- the same trampoline fetch uses (fetch.c).
 * netprefs_trampoline() has no locals and no arguments and stays noinline:
 * between the two StackSwap() calls a stack local of its own would read the
 * wrong memory.
 */
#define NETPREFS_STACK_SIZE (16UL * 1024UL)
#define NETPREFS_SAFE_STACK (8UL * 1024UL)

static struct StackSwapStruct np_sss;
static int                    np_result;

static int netprefs_run(void);

static __attribute__((noinline)) VOID netprefs_trampoline(VOID)
{
    StackSwap(&np_sss);
    np_result = netprefs_run();
    StackSwap(&np_sss);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    ULONG        have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    APTR         stack;

    (VOID)argc;
    (VOID)argv;

    if (have >= NETPREFS_SAFE_STACK)
        return netprefs_run();

    stack = AllocMem(NETPREFS_STACK_SIZE, MEMF_ANY);
    if (stack == NULL)
    {
        tool_error("not enough memory for NetPrefs' 16 KB working stack");
        return RETURN_FAIL;
    }

    np_sss.stk_Lower   = stack;
    np_sss.stk_Upper   = (ULONG)stack + NETPREFS_STACK_SIZE;
    np_sss.stk_Pointer = (APTR)((ULONG)stack + NETPREFS_STACK_SIZE);
    netprefs_trampoline();
    FreeMem(stack, NETPREFS_STACK_SIZE);
    return np_result;
}

static int netprefs_run(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (IntuitionBase == NULL || GfxBase == NULL || GadToolsBase == NULL)
    {
        if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
        if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
        if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
        return RETURN_FAIL;
    }

    scan_interfaces();
    if (!make_window())
    {
        requester("NetPrefs could not open its window. It needs a public\n"
                  "Workbench screen at least 640 by 200 pixels and the\n"
                  "standard OS 2.04 Intuition, Graphics and GadTools libraries.");
        close_ui();
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return RETURN_FAIL;
    }
    if (np.count != 0)
    {
        set_attr(np.g_interface, GTLV_Selected, 0);
        load_form(0);
    }
    else clear_form();
    update_live_state();
    draw_layout();
    event_loop();
    close_ui();
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return RETURN_OK;
}
