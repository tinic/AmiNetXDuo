/*
 * AddNetInterface, bring up the interface described by
 * DEVS:NetInterfaces/<name>.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "libfit.h"
#include "addif_wb.h"

#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/icon.h>
#include <proto/intuition.h>

const char *const tool_name = "AddNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("AddNetInterface");

/* The AmigaDOS Command Reference's template; test_refcompat holds TEMPLATE
   to it. */
#define REFERENCE_TEMPLATE "INTERFACE/M,QUIET/S,TIMEOUT/K/N"
#define TEMPLATE    "INTERFACE/M,QUIET/S,TIMEOUT/K/N"

enum
{
    ARG_INTERFACE = 0,
    ARG_QUIET,
    ARG_TIMEOUT,
    ARG_COUNT
};

/* Seconds: the reference's default, and its floor, below which a DHCP
   exchange reports a failure that is not one. */
#define ADDIF_TIMEOUT       60UL
#define ADDIF_TIMEOUT_MIN   10UL

/* What the stack needs beyond its own load file. */
#define ADDNETIF_MIN_FREE   (200UL * 1024UL)
#define ADDIF_MATCH_PATH    512UL

/* ReadArgs owns its result only until FreeArgs(), and MatchFirst reuses its
 * path buffer. Keep the explicit expansion in command-owned storage. */
static char   addif_expanded[AMI_CFG_MAX_ATTACHED][ADDIF_MATCH_PATH];
static STRPTR addif_names[AMI_CFG_MAX_ATTACHED + 1];

static VOID advise_out_of_memory(ULONG freemem)
{
    tool_printf("  %lu bytes free; about 450K needed\n", freemem);
}

/*
 * Turn a stack error code into advice. `ifc` is the interface file already
 * parsed, which lets the device explanation name the driver and unit.
 */
static VOID explain_startup_failure(LONG err, const AmiIfConfig *ifc)
{
    switch (err)
    {
        case AMI_NET_ERR_NODEV:
            tool_explain_device(ifc->device, ifc->unit, ifc->card);
            break;

        case AMI_NET_ERR_DEVBAD:
            tool_explain_device_refused(ifc->device, ifc->unit);
            break;

        case AMI_NET_ERR_CONFIG:
            if (ifc->iptype == AMI_IPTYPE_DHCP)
            {
                tool_explain_dhcp(ifc->name);
            }
            else
            {
                tool_printf("  %s has no usable address.\n", (LONG)ifc->name);
            }
            break;

        case AMI_NET_ERR_NOMEM:
            advise_out_of_memory(AvailMem(MEMF_PUBLIC));
            break;

        case AMI_NET_ERR_KERNEL:
            break;

        default:
            break;
    }
}

/* What LoadSeg of LIBS:bsdsocket.library allocates; zero when unreadable. */
static VOID library_need(LibFitNeed *need)
{
    UBYTE head[LIBFIT_HEAD_MAX];
    BPTR  fh;
    LONG  got = 0;

    fh = Open((CONST_STRPTR)"LIBS:bsdsocket.library", MODE_OLDFILE);
    if (fh != 0)
    {
        got = Read(fh, head, (LONG)sizeof(head));
        Close(fh);
    }

    (VOID)libfit_need(head, (got > 0) ? (ULONG)got : 0UL, need);
}

/*
 * bsdsocket.library did not open. That has exactly two causes. The free
 * figures are the ones taken before the open: a LoadSeg that fails gives
 * everything back, so what is free afterwards says nothing.
 */
static VOID explain_library_failure(const AmiIfConfig *ifc, ULONG freemem,
                                    ULONG largest, BOOL was_resident)
{
    LibFitNeed need = { 0, 0 };

    if (!tool_stack_installed())
    {
        tool_printf("  LIBS:bsdsocket.library is not installed.\n");
        return;
    }

    /* In LibList before the open: nothing had to be loaded. Asked after it,
       a library that loaded and then refused would count as resident. */
    if (!was_resident)
        library_need(&need);

    if (libfit_short(freemem, largest, &need, ADDNETIF_MIN_FREE))
    {
        advise_out_of_memory(freemem);
        return;
    }

    /* Probe the hardware rather than guess at it. */
    tool_explain_device(ifc->device, ifc->unit, ifc->card);
}

static BOOL load_interface(const char *name, AmiIfConfig *ifc, BOOL again)
{
    LONG err;

    if (!again)
        tool_config_watch();

    err = ami_config_load_interface(name, ifc);
    tool_config_unwatch();

    if (err == AMI_CFG_OK)
        return TRUE;

    if (again)
        return FALSE;

    if (err == AMI_CFG_ERR_IO)
    {
        tool_error("no interface \"%s\"", (LONG)name);
        tool_explain_interface_file(name);
    }
    else if (err == AMI_CFG_ERR_SYNTAX)
    {
        tool_error("DEVS:NetInterfaces/%s: invalid", (LONG)name);
    }
    else
    {
        tool_error("out of memory reading DEVS:NetInterfaces/%s", (LONG)name);
    }

    return FALSE;
}

/* --------------------------------------------------------- Workbench, icons */

/*
 * The AmigaDOS Command Reference: an interface file with a project icon whose
 * default tool is AddNetInterface starts that interface when it is opened.
 * The icon's QUIET and TIMEOUT tool types are the two arguments; PRI or
 * PRIORITY orders several interfaces, from the Shell as well.  What the
 * command would print goes to a T: file and, unless QUIET, into a requester
 * when it did not succeed.
 */
struct Library       *IconBase;
struct IntuitionBase *IntuitionBase;

static LONG addif_pri[AMI_CFG_MAX_ATTACHED];
static BOOL addif_quiet;

static BPTR addif_wb_out;
static BPTR addif_wb_old_out;
static BPTR addif_wb_old_ces;
static BOOL addif_wb_temp;          /* addif_wb_out is the T: file          */

static char addif_wb_log[32];       /* addif_log_name()                     */

/* A tool type of the icon beside `path`, or NULL.  The caller frees *dob. */
static const char *icon_tool_type(const char *path, const char *key,
                                  struct DiskObject **dob)
{
    if (IconBase == NULL)
        return NULL;

    if (*dob == NULL)
        *dob = GetDiskObject((STRPTR)path);
    if (*dob == NULL || (*dob)->do_ToolTypes == NULL)
        return NULL;

    return (const char *)FindToolType((CONST_STRPTR *)(*dob)->do_ToolTypes,
                                      (CONST_STRPTR)key);
}

/* PRI= or PRIORITY= in the icon beside `path`; 0 without one. */
static __attribute__((noinline)) LONG icon_priority(const char *path)
{
    struct DiskObject *dob = NULL;
    const char        *v   = icon_tool_type(path, "PRI", &dob);
    LONG               pri = 0;

    if (v == NULL)
        v = icon_tool_type(path, "PRIORITY", &dob);
    if (v != NULL)
        (VOID)StrToLong((STRPTR)v, &pri);
    if (dob != NULL)
        FreeDiskObject(dob);

    return pri;
}

/*
 * The project icons Workbench passed, as paths, with their QUIET and TIMEOUT.
 * QUIET in any of them makes the run quiet; the longest TIMEOUT wins.  A
 * TIMEOUT that is not a number, or is negative, is refused as the Shell
 * refuses it; the floor of 10 is applied by the caller for both.
 */
static __attribute__((noinline)) LONG wb_arguments(const struct WBStartup *wbs, ULONG *count,
                         BOOL *quiet, LONG *timeout)
{
    LONG i;

    for (i = 1; i < wbs->sm_NumArgs; i++)
    {
        const struct WBArg *wa   = &wbs->sm_ArgList[i];
        struct DiskObject  *dob  = NULL;
        char               *path = addif_expanded[*count];
        const char         *v;

        if (*count >= (ULONG)AMI_CFG_MAX_ATTACHED)
        {
            tool_error("more than %ld interfaces", (LONG)AMI_CFG_MAX_ATTACHED);
            return RETURN_FAIL;
        }

        if (wa->wa_Lock == (BPTR)0 ||
            !NameFromLock(wa->wa_Lock, (STRPTR)path, ADDIF_MATCH_PATH) ||
            !AddPart((STRPTR)path, wa->wa_Name, ADDIF_MATCH_PATH))
        {
            tool_error("%s: path too long", (LONG)wa->wa_Name);
            return RETURN_FAIL;
        }

        addif_names[*count] = (STRPTR)path;
        (*count)++;
        addif_names[*count] = NULL;

        /* Before anything is refused: QUIET is what decides whether the
           refusal comes up as a requester. */
        if (icon_tool_type(path, "QUIET", &dob) != NULL)
        {
            *quiet      = TRUE;
            addif_quiet = TRUE;
            /* The refusals below then return WARN, as the Shell's do. */
            tool_quiet_begin();
        }
        v = icon_tool_type(path, "TIMEOUT", &dob);
        if (v != NULL)
        {
            long given = 0;

            if (!addif_seconds(v, &given))
            {
                tool_error("TIMEOUT \"%s\" is not a number", (LONG)v);
                FreeDiskObject(dob);
                return RETURN_ERROR;
            }
            if (given < 0)
            {
                tool_error("TIMEOUT cannot be negative");
                FreeDiskObject(dob);
                return RETURN_ERROR;
            }
            if ((LONG)given > *timeout)
                *timeout = (LONG)given;
        }
        if (dob != NULL)
            FreeDiskObject(dob);
    }

    return RETURN_OK;
}

/* Output() and the error stream into a T: file for the requester, NIL: when
   T: is not there. */
static __attribute__((noinline)) VOID wb_capture_begin(VOID)
{
    struct Process *pr = (struct Process *)FindTask(NULL);

    IconBase      = OpenLibrary((CONST_STRPTR)"icon.library", 37);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 37);

    addif_log_name(addif_wb_log, sizeof(addif_wb_log), (ULONG)pr);
    addif_wb_out  = Open((CONST_STRPTR)addif_wb_log, MODE_NEWFILE);
    addif_wb_temp = (BOOL)(addif_wb_out != (BPTR)0);
    if (!addif_wb_temp)
        addif_wb_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (addif_wb_out == (BPTR)0)
        return;

    addif_wb_old_out = SelectOutput(addif_wb_out);
    addif_wb_old_ces = pr->pr_CES;
    pr->pr_CES       = addif_wb_out;
}

static __attribute__((noinline)) VOID wb_capture_end(LONG rc)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    static char     text[512];
    LONG            got = 0;

    if (addif_wb_out != (BPTR)0)
    {
        if (addif_wb_temp && rc != RETURN_OK && !addif_quiet)
        {
            Flush(addif_wb_out);
            (VOID)Seek(addif_wb_out, 0, OFFSET_BEGINNING);
            got = Read(addif_wb_out, text, (LONG)sizeof(text) - 1);
        }

        (VOID)SelectOutput(addif_wb_old_out);
        pr->pr_CES = addif_wb_old_ces;
        Close(addif_wb_out);
        if (addif_wb_temp)
            (VOID)DeleteFile((CONST_STRPTR)addif_wb_log);
    }

    if (rc != RETURN_OK && !addif_quiet && IntuitionBase != NULL)
    {
        struct EasyStruct es;

        if (got <= 0)
        {
            tool_copy_string(text, sizeof(text), "The interface did not start.");
            got = 1;
        }
        else
        {
            text[got] = '\0';
            while (got > 0 && text[got - 1] == '\n')
                text[--got] = '\0';
        }

        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)"AddNetInterface";
        es.es_TextFormat   = (UBYTE *)"%s";
        es.es_GadgetFormat = (UBYTE *)"OK";
        (VOID)EasyRequest(NULL, &es, NULL, (LONG)text);
    }

    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);
    if (IconBase != NULL)
        CloseLibrary(IconBase);
}

/*
 * Sort the names, so a list of interfaces is brought up in an order that does
 * not depend on how it was typed: the icon's PRI first, highest first, then
 * the name.  A bare name's icon is looked for where its file is.  Insertion
 * sort: the list is at most AMI_CFG_MAX_ATTACHED long.
 */
static __attribute__((noinline)) VOID sort_names(STRPTR *names, ULONG count)
{
    static char path[ADDIF_MATCH_PATH];
    ULONG       i;

    for (i = 0; i < count; i++)
    {
        const char *name = (const char *)names[i];

        if (tool_basename(name) != name)
        {
            addif_pri[i] = icon_priority(name);
            continue;
        }

        tool_join_path(path, sizeof(path), "DEVS:NetInterfaces", name);
        if (!tool_exists(path))
            tool_join_path(path, sizeof(path), "SYS:Storage/NetInterfaces",
                           name);
        addif_pri[i] = icon_priority(path);
    }

    for (i = 1; i < count; i++)
    {
        STRPTR hold = names[i];
        LONG   pri  = addif_pri[i];
        ULONG  j    = i;

        while (j > 0 &&
               (addif_pri[j - 1] < pri ||
                (addif_pri[j - 1] == pri &&
                 tool_stricmp(tool_basename((const char *)names[j - 1]),
                              tool_basename((const char *)hold)) > 0)))
        {
            names[j]     = names[j - 1];
            addif_pri[j] = addif_pri[j - 1];
            j--;
        }

        names[j]     = hold;
        addif_pri[j] = pri;
    }
}

static BOOL add_expanded_name(const char *path, ULONG *count)
{
    if (*count >= (ULONG)AMI_CFG_MAX_ATTACHED)
    {
        tool_error("attach: more than %ld interface definitions matched",
                   (LONG)AMI_CFG_MAX_ATTACHED);
        return FALSE;
    }

    tool_copy_string(addif_expanded[*count], ADDIF_MATCH_PATH, path);
    addif_names[*count] = (STRPTR)addif_expanded[*count];
    (*count)++;
    addif_names[*count] = NULL;
    return TRUE;
}

/* A wildcard in the argument, by DOS's own reading of it.  The scratch
 * ParsePatternNoCase() needs is 2n+2 bytes, from the pool, not the stack. */
static BOOL argument_is_pattern(const char *argument)
{
    ULONG len = 0;
    char *scratch;
    LONG  kind;

    while (argument[len] != '\0')
        len++;

    scratch = (char *)AllocVec(len * 2UL + 2UL, MEMF_PUBLIC);
    if (scratch == NULL)
        return FALSE;

    kind = ParsePatternNoCase((STRPTR)argument, (STRPTR)scratch,
                              (LONG)(len * 2UL + 2UL));
    FreeVec(scratch);

    return kind > 0;
}

/* Roadshow and AmiTCP_NG both ship a Network-Startup which explicitly passes
 * DEVS:NetInterfaces/~(#?.info). Opening the library remains inert; this is
 * the user-owned, command-line request to enumerate the drawer.
 *
 * ONLY A WILDCARD IS EXPANDED.  MatchFirst() on a plain path locks the
 * drawer part first, and through a DEVS: multi-assign that is the FIRST
 * member with such a drawer: on a machine whose system Devs/NetInterfaces
 * still exists, empty, a self-contained installation's
 * DEVS:NetInterfaces/eth0 -- the line its own S:Network-Startup carries --
 * was "no interface configuration matched", and the machine booted with no
 * network (A3000, 2026-09-19).  A plain path goes to the loader as given;
 * ami_config_load_interface() resolves DEVS: into the drawer there.  A
 * pattern's drawer part is resolved the same way before it is walked, so
 * DEVS:NetInterfaces/~(#?.info) enumerates the drawer that is read. */
static BOOL expand_interface_argument(const char *argument, ULONG *count)
{
    struct AnchorPath *anchor;
    LONG               err;
    BOOL               matched = FALSE;
    BOOL               ok = TRUE;
    char              *pattern = NULL;

    if (FilePart((STRPTR)argument) == (STRPTR)argument ||
        !argument_is_pattern(argument))
        return add_expanded_name(argument, count);

    {
        const char *file = (const char *)FilePart((STRPTR)argument);
        ULONG       dirlen = (ULONG)(file - argument);
        char       *dir = (char *)AllocVec(ADDIF_MATCH_PATH * 2UL, MEMF_PUBLIC);
        const char *where;

        if (dir == NULL)
        {
            tool_error("out of memory expanding interface pattern");
            return FALSE;
        }
        if (dirlen >= ADDIF_MATCH_PATH)
            dirlen = ADDIF_MATCH_PATH - 1UL;
        tool_copy_string(dir, dirlen + 1UL, argument);
        /* The drawer without its trailing separator: that is what the
           resolver locks; the separator goes back on below. */
        if (dirlen > 0 && dir[dirlen - 1] == '/')
            dir[dirlen - 1] = '\0';

        where = ami_cfg_resolve(dir, dir + ADDIF_MATCH_PATH, ADDIF_MATCH_PATH);
        if (where != dir)
        {
            pattern = dir;
            tool_copy_string(pattern, ADDIF_MATCH_PATH, where);
            if (!AddPart((STRPTR)pattern, (STRPTR)file, ADDIF_MATCH_PATH))
            {
                FreeVec(dir);
                tool_error("interface pattern is too long");
                return FALSE;
            }
            argument = pattern;
        }
        else
        {
            FreeVec(dir);
        }
    }

    anchor = (struct AnchorPath *)AllocVec(
        (ULONG)sizeof(*anchor) + ADDIF_MATCH_PATH, MEMF_PUBLIC | MEMF_CLEAR);
    if (anchor == NULL)
    {
        if (pattern != NULL)
            FreeVec(pattern);
        tool_error("out of memory expanding interface pattern");
        return FALSE;
    }

    anchor->ap_Strlen = ADDIF_MATCH_PATH;
    anchor->ap_BreakBits = SIGBREAKF_CTRL_C;

    err = MatchFirst((STRPTR)argument, anchor);
    while (err == 0)
    {
        const char *path = (const char *)anchor->ap_Buf;
        const char *name = tool_basename(path);
        ULONG       len = 0;

        while (name[len] != '\0')
            len++;

        if (anchor->ap_Info.fib_DirEntryType < 0 &&
            (len < 5 || tool_stricmp(name + len - 5, ".info") != 0))
        {
            matched = TRUE;
            /* THE PATH MatchFirst FOUND, not its basename.  A pattern can
               match outside DEVS:NetInterfaces, and reducing the result to a
               name sends the loader back to the drawer to find something
               else with that name -- or nothing. */
            if (!add_expanded_name(path, count))
            {
                ok = FALSE;
                break;
            }
        }

        err = MatchNext(anchor);
    }

    MatchEnd(anchor);
    FreeVec(anchor);

    if (ok && !matched)
    {
        tool_error("no interface configuration matched \"%s\"",
                   (LONG)argument);
        ok = FALSE;
    }
    else if (ok && err != 0 && err != ERROR_NO_MORE_ENTRIES)
    {
        tool_fault(err);
        ok = FALSE;
    }
    /* After the messages: `argument` may be the resolved pattern. */
    if (pattern != NULL)
        FreeVec(pattern);
    if (!ok)
        return FALSE;

    return TRUE;
}

/* Static: this table is most of a Shell command's 4 KB stack on its own. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} addif_ifaces;

/* And its IPv6 counterpart, for the same reason.  Three entries per
   interface, because that is what NX_MAX_IPV6_ADDRESSES is. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusAddress6   e[NX_MAX_PHYSICAL_INTERFACES * 3];
} addif_addr6;

static struct
{
    NetStatusHeader hdr;
    NetStatusSystem e;
} addif_system;

static VOID report_refused_gateway(struct Library *base, const char *name,
                                   ULONG asked_for)
{
    char text[16];

    if (asked_for == 0UL)
        return;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &addif_system,
                             sizeof(addif_system), sizeof(NetStatusSystem)) < 1)
        return;

    if (addif_system.e.nss_Gateway == asked_for)
        return;

    ami_config_format_ip(asked_for, text, sizeof(text));
    tool_printf("%s: gateway %s not on a local network\n",
                (LONG)name, (LONG)text);
}

/*
 * TENTATIVE is skipped: RFC 4862 5.4 says an address under duplicate address
 * detection is not one anything may use yet, and reporting it would mean this
 * command declares success a second before the address is real.
 */
static BOOL running_address6(struct Library *base, UWORD nx_index,
                             char *text, ULONG text_len)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_ADDRESSES6, &addif_addr6,
                             sizeof(addif_addr6), sizeof(NetStatusAddress6));
    if (n <= 0)
        return FALSE;

    for (i = 0; i < n && i < (LONG)(NX_MAX_PHYSICAL_INTERFACES * 3); i++)
    {
        const NetStatusAddress6 *a6 = &addif_addr6.e[i];

        if (a6->nsn_Interface != nx_index)
            continue;
        if (a6->nsn_State == NETSTATUS_IP6_TENTATIVE)
            continue;

        if (text != NULL)
            tool_format_ip6(a6->nsn_Address, text, text_len);
        return TRUE;
    }

    return FALSE;
}

/*
 * What the running stack has by that name: its index, or -1 when there is no
 * such interface and -2 when the stack did not answer. `addr_out` receives
 * the address it holds, which is 0 until one arrives.
 */
static LONG running_index(struct Library *base, const char *name,
                          ULONG *addr_out)
{
    LONG n;
    LONG i;

    if (addr_out != NULL)
        *addr_out = 0;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &addif_ifaces,
                             sizeof(addif_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
        return -2;

    /* nsh_Count is the library's number, not ours: bound it by the table it is
       being used to index, as removenetinterface.c and tool_nx.c do. */
    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!(addif_ifaces.e[i].nsi_Flags & NETSTATUS_IF_NAMED))
            continue;

        if (tool_stricmp(addif_ifaces.e[i].nsi_Name, name) != 0)
            continue;

        if (addr_out != NULL)
            *addr_out = addif_ifaces.e[i].nsi_Address;

        return (LONG)addif_ifaces.e[i].nsi_Index;
    }

    return -1;
}

/* Whether the attached interface at `index` has its link up. */
static BOOL running_is_up(struct Library *base, UWORD index)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &addif_ifaces,
                             sizeof(addif_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
        return TRUE;                    /* cannot tell: leave it alone */
    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (addif_ifaces.e[i].nsi_Index != index)
            continue;
        return (addif_ifaces.e[i].nsi_Flags & NETSTATUS_IF_LINKUP)
                   ? TRUE : FALSE;
    }
    return TRUE;
}

static LONG add_to_running_stack(struct Library *base, const char *name,
                                 const char *spec)
{
    return tool_stack_add_interface(base, name, spec, FALSE);
}

static VOID explain_no_slot(struct Library *base, const char *name)
{
    LONG n;
    LONG i;
    LONG shown = 0;

    tool_printf("  %s: all %ld interface slots in use",
                (LONG)name, (LONG)NX_MAX_PHYSICAL_INTERFACES);

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &addif_ifaces,
                             sizeof(addif_ifaces), sizeof(NetStatusInterface));

    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!(addif_ifaces.e[i].nsi_Flags & NETSTATUS_IF_NAMED))
            continue;

        tool_printf((shown == 0) ? " by %s" : " and %s",
                    (LONG)addif_ifaces.e[i].nsi_Name);
        shown++;
    }

    tool_printf("\n");
}

/* Why the add was refused, in the words the rest of this command uses. */
static VOID explain_add_failure(struct Library *base, LONG err,
                                const char *name, const AmiIfConfig *ifc)
{
    switch (err)
    {
        case ENOENT:
            tool_explain_interface_file(name);
            break;

        case ENXIO:
            tool_explain_device(ifc->device, ifc->unit, ifc->card);
            break;

        case EIO:
            tool_explain_device_refused(ifc->device, ifc->unit);
            break;

        case EEXIST:
            tool_printf("  %s: already attached\n", (LONG)name);
            break;

        case ENOSPC:
            explain_no_slot(base, name);
            break;

        case EBUSY:
            tool_printf("  %s unit %lu: requests pending from a removed "
                        "interface\n", (LONG)ifc->device, ifc->unit);
            break;

        case ENOBUFS:
            advise_out_of_memory(AvailMem(MEMF_PUBLIC));
            break;

        default:
            break;
    }
}

static BOOL needs_async_address(const AmiIfConfig *ifc)
{
    return (BOOL)((ifc->iptype != AMI_IPTYPE_STATIC &&
                   ifc->iptype != AMI_IPTYPE_NONE) ||
                  (!ami_config_iface_wants_ipv4(ifc) &&
                   ami_config_iface_wants_ipv6(ifc)));
}

/* Probe once even for static/no-address cases; ToolWait's zero limit means
 * unlimited, so the caller's no-wait decision must be separate. */
static BOOL wait_for_running_ready(struct Library *base, const char *name,
                                   const AmiIfConfig *ifc, ULONG seconds,
                                   BOOL no_wait,
                                   ULONG *addr_out, char *text6,
                                   ULONG text6_len, BOOL *broken)
{
    ToolWait wait;

    tool_wait_init(&wait, seconds);

    for (;;)
    {
        ULONG addr  = 0;
        LONG  where = running_index(base, name, &addr);

        if (where < 0)
            return FALSE;

        if (ifc->iptype == AMI_IPTYPE_DHCP)
        {
            if (tool_wait_dhcp_bound(base, (UWORD)where, &wait, &addr))
            {
                *addr_out = addr;
                (VOID)running_address6(base, (UWORD)where, text6, text6_len);
                return TRUE;
            }

            *broken = wait.broken;
            return FALSE;
        }
        else
        {
            if (addr != 0)
            {
                *addr_out = addr;
                return TRUE;
            }

            if (running_address6(base, (UWORD)where, text6, text6_len))
                return TRUE;
        }

        if (no_wait || !tool_wait_second(&wait))
        {
            *broken = wait.broken;
            return FALSE;
        }
    }
}

/* The same wait, against an interface of a stack that is linked in here. */
static BOOL wait_for_interface_ready(LONG index, const AmiIfConfig *ifc,
                                     ULONG seconds, BOOL no_wait,
                                     ULONG *addr_out,
                                     BOOL *broken)
{
    ToolWait wait;

    tool_wait_init(&wait, seconds);

    for (;;)
    {
        NX_IP *ip   = netstack_ip();
        ULONG  addr = 0;

        if (ip != NULL)
            addr = ip->nx_ip_interface[index].nx_interface_ip_address;

        if ((ifc->iptype == AMI_IPTYPE_DHCP &&
             netstack_interface_dhcp_state((UWORD)index) == AMI_DHCP_BOUND) ||
            (ifc->iptype != AMI_IPTYPE_DHCP && addr != 0))
        {
            *addr_out = addr;
            return TRUE;
        }

        if (no_wait || !tool_wait_second(&wait))
        {
            *broken = wait.broken;
            return FALSE;
        }
    }
}

static int addif_main(int argc, char **argv);

int main(int argc, char **argv)
{
    LONG rc;

    /* Workbench hands the WBStartup over as argv (tool_startup.S). */
    if (argc == 0)
        wb_capture_begin();
    else
        IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37);

    rc = tool_quiet_end(addif_main(argc, argv));

    if (argc == 0)
        wb_capture_end(rc);
    else if (IconBase != NULL)
        CloseLibrary(IconBase);

    return (int)rc;
}

/* Static: this command is one process, and 4 KB of Shell stack. */
static LONG args[ARG_COUNT];

static int addif_main(int argc, char **argv)
{
    struct RDArgs  *rda;
    AmiIfConfig     ifc;
    STRPTR         *arguments;
    STRPTR         *names = addif_names;
    const char     *name;
    const char     *primary;
    ULONG           count = 0;
    ULONG           timeout;
    ULONG           allowance;
    BOOL            quiet = FALSE;
    LONG            given = -1;
    BOOL            have_timeout = FALSE;
    BOOL            dynamic = FALSE;
    BOOL            broken = FALSE;
    ULONG           n;
    LONG            index;
    LONG            err;
    LONG            rc = RETURN_OK;

    tool_break_arm();

    if (argc == 0)
    {
        LONG wrc = wb_arguments((const struct WBStartup *)argv, &count,
                                &quiet, &given);

        if (wrc != RETURN_OK)
            return (int)wrc;
        have_timeout = (BOOL)(given >= 0);
    }
    else
    {
        args[ARG_INTERFACE] = 0;
        args[ARG_QUIET]     = 0;
        args[ARG_TIMEOUT]   = 0;

        rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
        if (rda == NULL)
        {
            tool_fault(IoErr());
            tool_usage("<interface name> [<interface name>...]",
                       "The name of a file in DEVS:NetInterfaces, for "
                       "example eth0.");
            return RETURN_ERROR;
        }

        arguments = (STRPTR *)args[ARG_INTERFACE];
        quiet     = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
        if (quiet)
            tool_quiet_begin();
        if (args[ARG_TIMEOUT] != 0)
        {
            have_timeout = TRUE;
            given        = *(const LONG *)args[ARG_TIMEOUT];
        }

        /* Copied into addif_expanded, so nothing below needs rda. */
        for (n = 0; arguments != NULL && arguments[n] != NULL; n++)
        {
            if (!expand_interface_argument((const char *)arguments[n],
                                           &count))
            {
                FreeArgs(rda);
                return RETURN_FAIL;
            }
        }

        FreeArgs(rda);
    }

    addif_quiet = quiet;
    if (quiet)
        tool_quiet_begin();

    timeout = ADDIF_TIMEOUT;
    if (have_timeout)
    {
        if (given < 0)
        {
            tool_error("TIMEOUT cannot be negative");
            return RETURN_ERROR;
        }

        timeout = ((ULONG)given > ADDIF_TIMEOUT_MIN) ? (ULONG)given
                                                     : ADDIF_TIMEOUT_MIN;
    }

    if (count == 0)
    {
        tool_error("no interface was named");
        tool_usage("<interface name> [<interface name>...]",
                   "The name of a file in DEVS:NetInterfaces, for example "
                   "eth0.");
        return RETURN_ERROR;
    }

    sort_names(names, count);
    primary = tool_basename((const char *)names[0]);

    /*
     * First pass: every named file must parse before anything is started, so
     * that a typo in User-Startup gives a message rather than a stack that
     * comes up with nothing on it.
     */
    for (n = 0; n < count; n++)
    {
        name = (const char *)names[n];

        if (!load_interface(name, &ifc, FALSE))
        {
            return RETURN_FAIL;
        }

        if (needs_async_address(&ifc))
            dynamic = TRUE;
    }

    if (!quiet)
    {
        for (n = 0; n < count; n++)
        {
            /* Load by what the user gave; NAME it by its file part. */
            name = tool_basename((const char *)names[n]);
            (VOID)load_interface((const char *)names[n], &ifc, TRUE);
            if (ifc.card[0] != '\0')
                tool_printf("%s: %s unit %ld card %s\n", (LONG)name,
                            (LONG)ifc.device, (LONG)ifc.unit, (LONG)ifc.card);
            else
                tool_printf("%s: %s unit %ld\n", (LONG)name, (LONG)ifc.device,
                            (LONG)ifc.unit);
        }
    }

    allowance = dynamic ? timeout : 0UL;

    /* Second pass. The first file's configuration is what the explainers use. */
    /* Keep an explicit path: reloading only its basename could select a
       different DEVS: or SYS: file and contradict the first-pass policy. */
    (VOID)load_interface((const char *)names[0], &ifc, TRUE);
    name = primary;

    err = netstack_startup();

    if (err == AMI_NET_ERR_STATE)
    {
        struct Library *base;
        BOOL            was_running = tool_stack_library_running();
        ULONG           freemem;
        ULONG           largest;
        BOOL            resident;
        char            idstr[8];

        if (!quiet && !was_running)
            tool_printf("%s: starting the network...\n", (LONG)name);

        resident = tool_stack_version(idstr, sizeof(idstr));
        freemem  = AvailMem(MEMF_PUBLIC);
        largest  = AvailMem(MEMF_PUBLIC | MEMF_LARGEST);
        base     = tool_stack_start();

        if (base == NULL)
        {
            tool_error("bsdsocket.library did not open");
            explain_library_failure(&ifc, freemem, largest, resident);
            return RETURN_FAIL;
        }

        if (!tool_stack_is_ours(base))
        {
            tool_error("another TCP/IP stack is installed on this machine");
            tool_explain_foreign_stack(base);

            tool_stack_release(base);
            return RETURN_WARN;
        }

        for (n = 0; n < count; n++)
        {
            ULONG addr = 0;
            char  text[16];
            char  text6[AMI_CFG_IP6_STRLEN];
            LONG  where;
            BOOL  left_down = FALSE;

            name = tool_basename((const char *)names[n]);
            (VOID)load_interface((const char *)names[n], &ifc, TRUE);

            where = running_index(base, name, &addr);

            if (where == -2)
            {
                tool_error("cannot read the interface list");
                tool_explain_no_netstatus(base);
                tool_stack_release(base);
                return RETURN_FAIL;
            }

            {
                LONG add_err;

                add_err = add_to_running_stack(base, name,
                                               (const char *)names[n]);

                if (add_err == EEXIST)
                {
                    /*
                     * Already attached.  If it is down -- an Offline, or
                     * a shutdown that left it -- this is the request to
                     * bring it back, not a report that it never left: the
                     * command used to answer "online, address x" here for
                     * an interface whose link was down and whose address was
                     * a memory (mja65, 2026-09-18).  The same operation
                     * Online performs; the wait below then sees the real
                     * state.
                     */
                    LONG idx = running_index(base, name, NULL);

                    add_err = 0;

                    /* STATE=DOWN in the file: left down, as a fresh add
                       leaves it (F-150), and reported as configured down,
                       not by an address it is only remembering. */
                    if (idx >= 0 && !ifc.up &&
                        !running_is_up(base, (UWORD)idx))
                        left_down = TRUE;
                    else if (idx >= 0 && !running_is_up(base, (UWORD)idx))
                    {
                        NetStatusControl upctl;
                        ULONG            w;
                        LONG             uperr = 0;

                        for (w = 0; w < (ULONG)(sizeof(upctl) / sizeof(ULONG));
                             w++)
                            ((ULONG *)&upctl)[w] = 0;
                        upctl.nsc_Index = (UWORD)idx;
                        if (tool_netstatus_control(base, NETCTRL_INTERFACE_UP,
                                                   &upctl, &uperr) != 0)
                        {
                            tool_error("%s is attached but did not come "
                                       "online", (LONG)name);
                            explain_add_failure(base, uperr, name, &ifc);
                            rc = RETURN_FAIL;
                            continue;
                        }
                    }
                }

                if (add_err != 0)
                {
                    tool_error("NETCTRL_INTERFACE_ADD refused %s: %s (%ld)",
                               (LONG)name, (LONG)tool_code_errno(add_err),
                               add_err);
                    explain_add_failure(base, add_err, name, &ifc);
                    rc = RETURN_FAIL;
                    continue;
                }

                if (!tool_stack_hold(base))
                {
                    tool_error("cannot hold the network open");
                    rc = RETURN_FAIL;
                    continue;
                }

                /* The add may have created the interface, so the address read
                   before it is stale. */
                where = running_index(base, name, &addr);
                if (where == -2)
                {
                    tool_error("cannot read the interface list");
                    tool_explain_no_netstatus(base);
                    tool_stack_release(base);
                    return RETURN_FAIL;
                }
            }

            text6[0] = '\0';

            if (left_down)
                addr = 0;
            else if (ifc.iptype == AMI_IPTYPE_DHCP || addr == 0)
                (VOID)wait_for_running_ready(base, name, &ifc, allowance,
                                             !needs_async_address(&ifc),
                                             &addr, text6, sizeof(text6),
                                             &broken);
            else if (where >= 0)
                (VOID)running_address6(base, (UWORD)where, text6,
                                       sizeof(text6));

            if (addr != 0)
            {
                if (!quiet)
                {
                    ami_config_format_ip(addr, text, sizeof(text));
                    if (text6[0] != '\0')
                        tool_printf("%s: online, address %s and %s\n",
                                    (LONG)name, (LONG)text, (LONG)text6);
                    else
                        tool_printf("%s: online, address %s\n", (LONG)name,
                                    (LONG)text);

                    /* An interface with an address and no route off its own
                       subnet is the one failure that looks like success. */
                    report_refused_gateway(base, name, ifc.gateway);
                }
            }
            else if (text6[0] != '\0')
            {
                if (!quiet)
                    tool_printf("%s: online, address %s\n", (LONG)name,
                                (LONG)text6);
            }
            else if (!ifc.up)
            {
                if (!quiet)
                    tool_printf("%s: configured down\n", (LONG)name);
            }
            else
            {
                if (!quiet)
                {
                    tool_printf("%s: online, no address\n", (LONG)name);

                    if (ifc.iptype == AMI_IPTYPE_DHCP)
                        tool_explain_dhcp_waiting(name, timeout);
                }

                if (rc == RETURN_OK)
                    rc = RETURN_WARN;
            }

            if (broken)
                break;
        }

        if (broken)
        {
            tool_fault(ERROR_BREAK);
            tool_stack_release(base);
            return RETURN_WARN;
        }

        tool_stack_release(base);
        return (int)rc;
    }

    if (err != AMI_NET_OK)
    {
        tool_error("netstack_startup: %s (%s, %ld)",
                   (LONG)tool_net_error(err), (LONG)tool_code_net(err), err);
        explain_startup_failure(err, &ifc);
        return RETURN_FAIL;
    }

    for (n = 0; n < count; n++)
    {
        name = tool_basename((const char *)names[n]);
        (VOID)load_interface((const char *)names[n], &ifc, TRUE);

        index = tool_find_interface(name);
        if (index < 0)
        {
            UWORD slot = 0;

            err = netstack_interface_start(&ifc, &slot);
            if (err != AMI_NET_OK)
            {
                tool_error("%s was not added to the running network: %s",
                           (LONG)name, (LONG)tool_net_error(err));
                explain_startup_failure(err, &ifc);
                return RETURN_FAIL;
            }

            index = (LONG)slot;
        }

        /* STATE=DOWN: netstack_interface_start() left it down, and so does
           this, whether it was just started or already there (F-150). */
        if (!ifc.up)
        {
            if (!quiet)
                tool_printf("%s: configured down\n", (LONG)name);
            continue;
        }

        if (!netstack_interface_is_up((UWORD)index))
        {
            err = netstack_interface_up((UWORD)index);
            if (err != AMI_NET_OK)
            {
                tool_error("netstack_interface_up (S2_ONLINE): %s: %s "
                           "(%s, %ld)", (LONG)name,
                           (LONG)tool_net_error(err),
                           (LONG)tool_code_net(err), err);
                tool_explain_device(ifc.device, ifc.unit, ifc.card);
                return RETURN_FAIL;
            }
        }

        {
            char  addr[16];
            char  mask[16];
            char  live6[AMI_CFG_IP6_STRLEN];
            ULONG live_addr = 0;
            ULONG live_mask = 0;
            BOOL  no_wait = (ifc.iptype == AMI_IPTYPE_STATIC ||
                             ifc.iptype == AMI_IPTYPE_NONE);
            NX_IP *ip;

            (VOID)wait_for_interface_ready(index, &ifc,
                                           no_wait ? 0UL : timeout, no_wait,
                                           &live_addr, &broken);

            ip = netstack_ip();
            if (ip != NULL)
                live_mask =
                    ip->nx_ip_interface[index].nx_interface_ip_network_mask;

            /* The other family, for the same reason as in the library build:
               an interface can be perfectly online and hold no IPv4 address
               because none was asked for. */
            live6[0] = '\0';
#ifdef AMINETXDUO_IPV6
            {
                UWORD slot6;

                for (slot6 = 0; ; slot6++)
                {
                    ULONG a6[4];
                    ULONG state6 = 0;

                    if (!netstack_ipv6_address_get((UWORD)index, slot6, a6,
                                                   NULL, &state6))
                        break;
                    if (state6 == (ULONG)NETSTATUS_IP6_TENTATIVE)
                        continue;

                    tool_format_ip6(a6, live6, sizeof(live6));
                    break;
                }
            }
#endif

            /* Online with nothing on it is not a success. The library build
               above answers the same case the same way. */
            if (live_addr == 0 && live6[0] == '\0' && ifc.up &&
                rc == RETURN_OK)
                rc = RETURN_WARN;

            if (quiet)
            {
                /* nothing to say */
            }
            else if (live_addr == 0 && live6[0] != '\0')
            {
                tool_printf("%s: online, address %s\n", (LONG)name,
                            (LONG)live6);
            }
            else if (live_addr == 0)
            {
                tool_printf("%s: online, no address\n", (LONG)name);

                if (ifc.iptype == AMI_IPTYPE_DHCP)
                    tool_explain_dhcp_waiting(name, timeout);
            }
            else
            {
                ami_config_format_ip(live_addr, addr, sizeof(addr));
                ami_config_format_ip(live_mask, mask, sizeof(mask));

                tool_printf("%s: %s netmask %s (%s)\n",
                            (LONG)name, (LONG)addr, (LONG)mask,
                            (LONG)(ifc.iptype == AMI_IPTYPE_DHCP      ? "DHCP" :
                                   ifc.iptype == AMI_IPTYPE_LINKLOCAL ? "link-local"
                                                                      : "static"));
            }
        }

        if (broken)
            break;
    }

    if (broken || tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    return (int)rc;
}
