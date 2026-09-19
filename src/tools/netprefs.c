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
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
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

enum
{
    GID_INTERFACE = 1,
    GID_NEW,
    GID_NAME,
    GID_DEVICE,
    GID_UNIT,
    GID_CARD,
    GID_IPV4,
    GID_ADDRESS,
    GID_NETMASK,
    GID_GATEWAY,
    GID_IPV6,
    GID_MDNS,
    GID_STATE,
    GID_BOOT,
    GID_PRIORITY,
    GID_STATUS,
    GID_SAVE,
    GID_APPLY,
    GID_ONLINE,
    GID_OFFLINE,
    GID_REMOVE,
    GID_CLOSE
};

typedef struct NetPrefs
{
    struct Screen *screen;
    struct Window *window;
    struct Gadget *gadgets;
    APTR           visual;
    struct Gadget *g_interface;
    struct Gadget *g_name;
    struct Gadget *g_device;
    struct Gadget *g_unit;
    struct Gadget *g_card;
    struct Gadget *g_ipv4;
    struct Gadget *g_address;
    struct Gadget *g_netmask;
    struct Gadget *g_gateway;
    struct Gadget *g_ipv6;
    struct Gadget *g_mdns;
    struct Gadget *g_state;
    struct Gadget *g_boot;
    struct Gadget *g_priority;
    struct Gadget *g_status;
    char           names[NP_MAX_INTERFACES][TOOL_NAME_LEN];
    STRPTR         labels[NP_MAX_INTERFACES + 2];
    ULONG          count;
    LONG           selected;
} NetPrefs;

static NetPrefs np;
static char np_address[16];
static char np_netmask[16];
static char np_gateway[16];
static char np_status[128];

static const STRPTR ipv4_labels[] =
{
    (STRPTR)"Static", (STRPTR)"DHCP", (STRPTR)"Link-local",
    (STRPTR)"Off", NULL
};

static const STRPTR ipv6_labels[] =
{
    (STRPTR)"Off", (STRPTR)"Link-local", (STRPTR)"Automatic",
    (STRPTR)"Static (keep ADDRESS6)", (STRPTR)"DHCPv6", NULL
};

static ULONG text_len(const char *s)
{
    ULONG n = 0;
    while (s != NULL && s[n] != '\0') n++;
    return n;
}

static BOOL sane_name(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
    {
        char c = s[n++];
        if (n >= AMI_CFG_NAME_LEN || c == '/' || c == ':' || c == ' ' ||
            c == '\t' || c == '\r' || c == '\n' || c == '#' || c == ';')
            return FALSE;
    }
    return (BOOL)(n != 0);
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

static VOID set_static_fields(BOOL enabled)
{
    set_attr(np.g_address, GA_Disabled, (ULONG)!enabled);
    set_attr(np.g_netmask, GA_Disabled, (ULONG)!enabled);
    set_attr(np.g_gateway, GA_Disabled, (ULONG)!enabled);
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

static BOOL boot_state(const char *name, BOOL *by_wildcard)
{
    ULONG len;
    char *data = read_file("S:Network-Startup", &len, TRUE);
    ULONG at = 0;
    BOOL found = FALSE;

    *by_wildcard = FALSE;
    if (data == NULL) return FALSE;
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
            if (wildcard) *by_wildcard = TRUE;
        }
    }
    ami_free(data);
    return found;
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

static LONG run_command(const char *command, const char *name)
{
    char line[NP_PATH_LEN];
    const char *prefix;

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
    tool_copy_string(line + text_len(line), sizeof(line) - text_len(line), " QUIET");
    return SystemTagList((CONST_STRPTR)line, NULL);
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

    np.count = tool_list_dir(dir, np.names, NP_MAX_INTERFACES, NULL);
    sort_names();
    np.labels[0] = (STRPTR)"<new interface>";
    for (i = 0; i < np.count; i++) np.labels[i + 1] = (STRPTR)np.names[i];
    np.labels[np.count + 1] = NULL;
}

static VOID clear_form(VOID)
{
    set_attr(np.g_name, GA_Disabled, FALSE);
    set_attr(np.g_name, GTST_String, (ULONG)"");
    set_attr(np.g_device, GTST_String, (ULONG)"");
    set_attr(np.g_unit, GTIN_Number, 0);
    set_attr(np.g_card, GTST_String, (ULONG)"");
    set_attr(np.g_ipv4, GTCY_Active, AMI_IPTYPE_DHCP);
    set_attr(np.g_address, GTST_String, (ULONG)"");
    set_attr(np.g_netmask, GTST_String, (ULONG)"");
    set_attr(np.g_gateway, GTST_String, (ULONG)"");
    set_static_fields(FALSE);
    set_attr(np.g_ipv6, GTCY_Active, AMI_IP6TYPE_AUTO);
    set_attr(np.g_mdns, GTCB_Checked, FALSE);
    set_attr(np.g_state, GTCB_Checked, TRUE);
    set_attr(np.g_boot, GTCB_Checked, FALSE);
    set_attr(np.g_priority, GTIN_Number, 0);
    np.selected = -1;
    set_status("New interface: enter a name and device, then Save.");
}

static VOID load_form(LONG index)
{
    AmiIfConfig cfg;
    BOOL wildcard;

    if (index < 0 || (ULONG)index >= np.count) { clear_form(); return; }
    if (ami_config_load_interface(np.names[index], &cfg) != AMI_CFG_OK)
    {
        requester("This interface definition could not be parsed.\n"
                  "Run CheckNetConfig for the exact line and reason.");
        return;
    }
    np.selected = index;
    ami_config_format_ip(cfg.address, np_address, sizeof(np_address));
    ami_config_format_ip(cfg.netmask, np_netmask, sizeof(np_netmask));
    ami_config_format_ip(cfg.gateway, np_gateway, sizeof(np_gateway));
    set_attr(np.g_name, GTST_String, (ULONG)cfg.name);
    set_attr(np.g_name, GA_Disabled, TRUE);
    set_attr(np.g_device, GTST_String, (ULONG)cfg.device);
    set_attr(np.g_unit, GTIN_Number, cfg.unit);
    set_attr(np.g_card, GTST_String, (ULONG)cfg.card);
    set_attr(np.g_ipv4, GTCY_Active, (ULONG)cfg.iptype);
    set_attr(np.g_address, GTST_String,
             (ULONG)(cfg.address != 0 ? np_address : ""));
    set_attr(np.g_netmask, GTST_String,
             (ULONG)(cfg.netmask != 0 ? np_netmask : ""));
    set_attr(np.g_gateway, GTST_String,
             (ULONG)(cfg.gateway != 0 ? np_gateway : ""));
    set_static_fields((BOOL)(cfg.iptype == AMI_IPTYPE_STATIC));
    set_attr(np.g_ipv6, GTCY_Active, (ULONG)cfg.ip6type);
    set_attr(np.g_mdns, GTCB_Checked, (ULONG)cfg.mdns);
    set_attr(np.g_state, GTCB_Checked, (ULONG)cfg.up);
    set_attr(np.g_boot, GTCB_Checked,
             (ULONG)boot_state(cfg.name, &wildcard));
    set_attr(np.g_priority, GTIN_Number, (ULONG)(LONG)cfg.priority);
    set_status(wildcard ? "Starts at boot through the all-interface wildcard."
                        : "Definition loaded. Save keeps advanced settings and comments.");
}

static BOOL get_cycle(struct Gadget *g, ULONG *value)
{
    struct TagItem tags[2];
    tags[0].ti_Tag = GTCY_Active; tags[0].ti_Data = (ULONG)value;
    tags[1].ti_Tag = TAG_DONE; tags[1].ti_Data = 0;
    return (BOOL)(GT_GetGadgetAttrsA(g, np.window, NULL, tags) != 0);
}

static BOOL save_form(BOOL apply)
{
    char name[AMI_CFG_NAME_LEN], path[NP_PATH_LEN], resolved[AMI_CFG_PATH_LEN];
    char unit[12], priority[12];
    char *old, *patched;
    ULONG oldlen = 0, newlen = 0, ipv4 = 0, ipv6 = 0;
    LONG pri = integer_value(np.g_priority);
    ULONG address, mask, gateway = 0;
    NpTextField fields[11];
    BOOL existed, old_boot, wildcard;
    ULONG i;

    tool_copy_string(name, sizeof(name), string_value(np.g_name));
    if (!sane_name(name))
    {
        requester("The name must be 1-63 characters and cannot contain\n"
                  "spaces, a slash, colon, semicolon or #.");
        return FALSE;
    }
    if (string_value(np.g_device)[0] == '\0')
    {
        requester("Choose a SANA-II device before saving.");
        return FALSE;
    }
    if (integer_value(np.g_unit) < 0 || pri < -128 || pri > 127)
    {
        requester("Unit must not be negative; priority must be -128 through 127.");
        return FALSE;
    }
    (VOID)get_cycle(np.g_ipv4, &ipv4);
    (VOID)get_cycle(np.g_ipv6, &ipv6);
    if (ipv6 == AMI_IP6TYPE_STATIC && np.selected < 0)
    {
        requester("NetPrefs preserves existing ADDRESS6 lines, including two-address\n"
                  "configurations, but does not reduce them to one GUI field.\n"
                  "Create a static IPv6 definition with NetSetup or a text editor.");
        return FALSE;
    }
    if (ipv4 == AMI_IPTYPE_STATIC)
    {
        if (!ami_config_parse_ip(string_value(np.g_address), &address) ||
            address == 0 || !ami_config_parse_ip(string_value(np.g_netmask), &mask) ||
            mask == 0 || (((~mask) & ((~mask) + 1UL)) != 0))
        {
            requester("A static IPv4 interface needs a valid address and netmask.");
            return FALSE;
        }
        if (string_value(np.g_gateway)[0] != '\0' &&
            (!ami_config_parse_ip(string_value(np.g_gateway), &gateway) ||
             gateway == 0))
        {
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
    fields[0].key = "DEVICE"; fields[0].value = string_value(np.g_device);
    fields[1].key = "UNIT"; fields[1].value = unit;
    fields[2].key = "CARD"; fields[2].value = string_value(np.g_card)[0] != '\0'
                                           ? string_value(np.g_card) : NULL;
    fields[3].key = "CONFIGURE"; fields[3].value =
        ipv4 == AMI_IPTYPE_DHCP ? "DHCP" :
        ipv4 == AMI_IPTYPE_LINKLOCAL ? "LINKLOCAL" :
        ipv4 == AMI_IPTYPE_NONE ? "NONE" : "STATIC";
    fields[4].key = "ADDRESS"; fields[4].value =
        ipv4 == AMI_IPTYPE_STATIC ? string_value(np.g_address) : NULL;
    fields[5].key = "NETMASK"; fields[5].value =
        ipv4 == AMI_IPTYPE_STATIC ? string_value(np.g_netmask) : NULL;
    fields[6].key = "GATEWAY"; fields[6].value =
        ipv4 == AMI_IPTYPE_STATIC && gateway != 0 ? string_value(np.g_gateway) : NULL;
    fields[7].key = "CONFIGURE6"; fields[7].value =
        ipv6 == AMI_IP6TYPE_LINKLOCAL ? "LINKLOCAL" :
        ipv6 == AMI_IP6TYPE_AUTO ? "AUTO" :
        ipv6 == AMI_IP6TYPE_STATIC ? "STATIC" :
        ipv6 == AMI_IP6TYPE_DHCP ? "DHCP" : "OFF";
    fields[8].key = "MDNS"; fields[8].value = checked(np.g_mdns) ? "YES" : "NO";
    fields[9].key = "PRIORITY"; fields[9].value = priority;
    fields[10].key = "STATE"; fields[10].value = checked(np.g_state) ? "UP" : "DOWN";
    for (i = 0; i < 11; i++) fields[i].seen = FALSE;

    tool_copy_string(path, sizeof(path),
        ami_cfg_resolve("DEVS:NetInterfaces", resolved, sizeof(resolved)));
    if (!ensure_dir(path))
    {
        requester("DEVS:NetInterfaces could not be created.");
        return FALSE;
    }
    tool_copy_string(path + text_len(path), sizeof(path) - text_len(path), "/");
    tool_copy_string(path + text_len(path), sizeof(path) - text_len(path), name);
    existed = tool_exists(path);
    old = read_file(path, &oldlen, (BOOL)!existed);
    /* Do not flatten an existing file merely because it became unreadable. */
    if (old == NULL && existed) return FALSE;
    if (old == NULL)
    {
        static const char heading[] =
            "# Network interface, written by AmiNetXDuo NetPrefs.\n"
            "# Unknown keywords and comments are preserved when edited.\n\n";
        oldlen = sizeof(heading) - 1;
        old = (char *)ami_alloc(oldlen + 1);
        if (old == NULL) return FALSE;
        for (i = 0; i <= oldlen; i++) old[i] = heading[i];
    }
    patched = patch_fields(old, oldlen, fields, 11, &newlen);
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

    old_boot = boot_state(name, &wildcard);
    if (old_boot != checked(np.g_boot) &&
        !set_boot_state(name, checked(np.g_boot), FALSE))
    {
        requester("The definition was saved, but its boot setting was not changed.");
        return FALSE;
    }
    scan_interfaces();
    set_attr(np.g_interface, GTCY_Labels, (ULONG)np.labels);
    for (i = 0; i < np.count; i++)
    {
        if (tool_stricmp(np.names[i], name) == 0)
        {
            np.selected = (LONG)i;
            set_attr(np.g_interface, GTCY_Active, i + 1);
            set_attr(np.g_name, GA_Disabled, TRUE);
            break;
        }
    }

    if (apply)
    {
        if (existed) (VOID)run_command("RemoveNetInterface", name);
        if (run_command("AddNetInterface", name) != 0)
        {
            requester("The definition was saved, but the interface did not start.\n"
                      "Run AddNetInterface from Shell to see the detailed reason.");
            return FALSE;
        }
        set_status("Saved and applied. The interface was restarted.");
    }
    else
        set_status("Saved. Use Apply to restart the interface with these settings.");
    return TRUE;
}

static VOID remove_form(VOID)
{
    char name[AMI_CFG_NAME_LEN], path[NP_PATH_LEN], parked[NP_PATH_LEN];
    char resolved[AMI_CFG_PATH_LEN];
    BOOL wildcard;

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
    if (!confirm("Remove this interface definition?\n\n"
                 "It will be taken offline and parked as an ignored .info\n"
                 "backup, not deleted.", "Remove")) return;
    if (boot_state(name, &wildcard) && !set_boot_state(name, FALSE, TRUE)) return;
    (VOID)run_command("RemoveNetInterface", name);
    tool_copy_string(parked, sizeof(parked), path);
    tool_copy_string(parked + text_len(parked),
                     sizeof(parked) - text_len(parked), ".disabled.info");
    (VOID)DeleteFile((CONST_STRPTR)parked);
    if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)parked))
    {
        requester("The interface could not be parked; its definition was kept.");
        return;
    }
    scan_interfaces();
    set_attr(np.g_interface, GTCY_Labels, (ULONG)np.labels);
    set_attr(np.g_interface, GTCY_Active, 0);
    clear_form();
    set_status("Removed. The old definition remains beside the drawer as .disabled.info.");
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

static BOOL make_window(VOID)
{
    struct Gadget *context, *g;
    struct TagItem tags[5];
    struct TagItem win[12];
    WORD width, height;

    np.screen = LockPubScreen(NULL);
    if (np.screen == NULL) return FALSE;
    /* Fit the stock 640x200 NTSC Workbench as well as PAL and taller modes. */
    if (np.screen->Width < 620 || np.screen->Height < 200) return FALSE;
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

    TAG1(GTCY_Labels, np.labels);
    ADD(np.g_interface,CYCLE_KIND,GID_INTERFACE,86,5,164,15,"Interface",PLACETEXT_LEFT);
    TAG1(TAG_DONE, 0);
    ADD(g,BUTTON_KIND,GID_NEW,260,5,58,15,"New",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_REMOVE,324,5,66,15,"Remove",PLACETEXT_IN);
    TAG1(GTCB_Checked, TRUE);
    ADD(np.g_state,CHECKBOX_KIND,GID_STATE,414,6,CHECKBOX_WIDTH,CHECKBOX_HEIGHT,
        "Start online",PLACETEXT_RIGHT);
    TAG1(GTCB_Checked, FALSE);
    ADD(np.g_boot,CHECKBOX_KIND,GID_BOOT,528,6,CHECKBOX_WIDTH,CHECKBOX_HEIGHT,
        "At boot",PLACETEXT_RIGHT);

    tags[0].ti_Tag=GTTX_Text; tags[0].ti_Data=(ULONG)"Hardware";
    tags[1].ti_Tag=GTTX_CopyText; tags[1].ti_Data=FALSE;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(g,TEXT_KIND,0,18,23,58,10,NULL,0);
    tags[0].ti_Tag=GTTX_Text; tags[0].ti_Data=(ULONG)"Addressing";
    tags[1].ti_Tag=GTTX_CopyText; tags[1].ti_Data=FALSE;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(g,TEXT_KIND,0,308,23,70,10,NULL,0);

    TAG1(GTST_MaxChars, AMI_CFG_NAME_LEN - 1);
    ADD(np.g_name,STRING_KIND,GID_NAME,72,39,202,15,"Name",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_PATH_LEN - 1);
    ADD(np.g_device,STRING_KIND,GID_DEVICE,72,59,202,15,"Device",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 3);
    ADD(np.g_unit,INTEGER_KIND,GID_UNIT,72,79,38,15,"Unit",PLACETEXT_LEFT);
    TAG1(GTIN_MaxChars, 4);
    ADD(np.g_priority,INTEGER_KIND,GID_PRIORITY,200,79,66,15,"Priority",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, AMI_CFG_NAME_LEN - 1);
    ADD(np.g_card,STRING_KIND,GID_CARD,72,99,202,15,"Card",PLACETEXT_LEFT);

    tags[0].ti_Tag=GTCY_Labels; tags[0].ti_Data=(ULONG)ipv4_labels;
    tags[1].ti_Tag=GTCY_Active; tags[1].ti_Data=AMI_IPTYPE_DHCP;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(np.g_ipv4,CYCLE_KIND,GID_IPV4,350,39,105,15,"IPv4",PLACETEXT_LEFT);
    TAG1(GTCB_Checked, FALSE);
    ADD(np.g_mdns,CHECKBOX_KIND,GID_MDNS,500,40,CHECKBOX_WIDTH,CHECKBOX_HEIGHT,
        "mDNS",PLACETEXT_RIGHT);
    TAG1(GTST_MaxChars, 15);
    ADD(np.g_address,STRING_KIND,GID_ADDRESS,362,59,100,15,"Address",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, 15);
    ADD(np.g_netmask,STRING_KIND,GID_NETMASK,512,59,68,15,"Mask",PLACETEXT_LEFT);
    TAG1(GTST_MaxChars, 15);
    ADD(np.g_gateway,STRING_KIND,GID_GATEWAY,370,79,100,15,"Gateway",PLACETEXT_LEFT);
    tags[0].ti_Tag=GTCY_Labels; tags[0].ti_Data=(ULONG)ipv6_labels;
    tags[1].ti_Tag=GTCY_Active; tags[1].ti_Data=AMI_IP6TYPE_AUTO;
    tags[2].ti_Tag=TAG_DONE; tags[2].ti_Data=0;
    ADD(np.g_ipv6,CYCLE_KIND,GID_IPV6,350,99,146,15,"IPv6",PLACETEXT_LEFT);

    tags[0].ti_Tag=GTTX_Text; tags[0].ti_Data=(ULONG)"Loading definitions...";
    tags[1].ti_Tag=GTTX_Border; tags[1].ti_Data=TRUE;
    tags[2].ti_Tag=GTTX_CopyText; tags[2].ti_Data=FALSE;
    tags[3].ti_Tag=TAG_DONE; tags[3].ti_Data=0;
    ADD(np.g_status,TEXT_KIND,GID_STATUS,8,130,584,15,NULL,0);
    TAG1(TAG_DONE, 0);
    ADD(g,BUTTON_KIND,GID_SAVE,8,152,72,17,"Save",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_APPLY,88,152,100,17,"Save & Start",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_ONLINE,350,152,70,17,"Online",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_OFFLINE,428,152,70,17,"Offline",PLACETEXT_IN);
    ADD(g,BUTTON_KIND,GID_CLOSE,522,152,70,17,"Close",PLACETEXT_IN);

#undef ADD
#undef TAG1

    width = 610;
    height = 190;
    win[0].ti_Tag=WA_Left; win[0].ti_Data=(np.screen->Width-width)/2;
    win[1].ti_Tag=WA_Top; win[1].ti_Data=(np.screen->Height-height)/2;
    win[2].ti_Tag=WA_Width; win[2].ti_Data=width;
    win[3].ti_Tag=WA_Height; win[3].ti_Data=height;
    win[4].ti_Tag=WA_Title; win[4].ti_Data=(ULONG)"AmiNetXDuo Network Preferences";
    win[5].ti_Tag=WA_IDCMP; win[5].ti_Data=IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW|
        BUTTONIDCMP|CHECKBOXIDCMP|CYCLEIDCMP|STRINGIDCMP;
    win[6].ti_Tag=WA_Flags; win[6].ti_Data=WFLG_DRAGBAR|WFLG_DEPTHGADGET|
        WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SMART_REFRESH|WFLG_GIMMEZEROZERO;
    win[7].ti_Tag=WA_Gadgets; win[7].ti_Data=(ULONG)np.gadgets;
    win[8].ti_Tag=WA_PubScreen; win[8].ti_Data=(ULONG)np.screen;
    win[9].ti_Tag=WA_AutoAdjust; win[9].ti_Data=TRUE;
    win[10].ti_Tag=TAG_DONE; win[10].ti_Data=0;
    np.window = OpenWindowTagList(NULL, win);
    if (np.window == NULL) return FALSE;
    tags[0].ti_Tag=GTBB_Recessed; tags[0].ti_Data=TRUE;
    tags[1].ti_Tag=TAG_DONE; tags[1].ti_Data=0;
    DrawBevelBoxA(np.window->RPort, 8, 28, 278, 94, tags);
    DrawBevelBoxA(np.window->RPort, 298, 28, 294, 94, tags);
    GT_RefreshWindow(np.window, NULL);
    return TRUE;
}

static VOID close_ui(VOID)
{
    if (np.window != NULL) CloseWindow(np.window);
    np.window = NULL;
    if (np.gadgets != NULL) FreeGadgets(np.gadgets);
    np.gadgets = NULL;
    if (np.visual != NULL) FreeVisualInfo(np.visual);
    np.visual = NULL;
    if (np.screen != NULL) UnlockPubScreen(NULL, np.screen);
    np.screen = NULL;
}

static VOID event_loop(VOID)
{
    BOOL done = FALSE;

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
            }
            else if (cls == IDCMP_GADGETUP)
            {
                const char *name = string_value(np.g_name);
                switch (id)
                {
                    case GID_INTERFACE:
                        if (code == 0) clear_form();
                        else load_form((LONG)code - 1);
                        break;
                    case GID_NEW: set_attr(np.g_interface, GTCY_Active, 0); clear_form(); break;
                    case GID_IPV4:
                        set_static_fields((BOOL)(code == AMI_IPTYPE_STATIC));
                        break;
                    case GID_SAVE: (VOID)save_form(FALSE); break;
                    case GID_APPLY: (VOID)save_form(TRUE); break;
                    case GID_ONLINE:
                        if (sane_name(name) && run_command("Online", name) == 0)
                            set_status("Interface is online.");
                        else requester("Online failed. Run it from Shell for details.");
                        break;
                    case GID_OFFLINE:
                        if (sane_name(name) && run_command("Offline", name) == 0)
                            set_status("Interface is offline.");
                        else requester("Offline failed. Run it from Shell for details.");
                        break;
                    case GID_REMOVE: remove_form(); break;
                    case GID_CLOSE: done = TRUE; break;
                    default: break;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    (VOID)argc;
    (VOID)argv;
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
                  "Workbench screen at least 620 by 200 pixels and the\n"
                  "standard OS 2.04 Intuition, Graphics and GadTools libraries.");
        close_ui();
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return RETURN_FAIL;
    }
    if (np.count != 0)
    {
        set_attr(np.g_interface, GTCY_Active, 1);
        load_form(0);
    }
    else clear_form();
    event_loop();
    close_ui();
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return RETURN_OK;
}
