/*
 * CheckNetConfig, read the network configuration and report what is wrong
 * with it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"


const char *const tool_name = "CheckNetConfig";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("CheckNetConfig");

#define TEMPLATE    "QUIET/S,VERBOSE/S"

enum
{
    ARG_QUIET = 0,
    ARG_VERBOSE,
    ARG_COUNT
};

#define CNC_DIR_INTERFACES  "DEVS:NetInterfaces"
#define CNC_DIR_STORAGE     "SYS:Storage/NetInterfaces"

/* As many interface files as the drawer is scanned for. */
#define CNC_MAX_FILES       16

/* Long enough for any line in these files. Longer ones are read in pieces. */
#define CNC_LINE_MAX        200

static BOOL  cnc_quiet;
static BOOL  cnc_verbose;
static UWORD cnc_errors;
static UWORD cnc_warnings;

static VOID say(const char *fmt, ...)
{
    va_list args;

    if (cnc_quiet)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);         /* (APTR): see tool_util.c */
    va_end(args);
}

/* The body of a finding: wrapped to the window, indented under its heading. */
static VOID note(const char *text)
{
    if (!cnc_quiet)
        tool_wrap(6, text);
}

/*
 * The verdict: the one line QUIET does not suppress when VERBOSE is also
 * given, so a script can print a summary without the full report.
 */
static VOID verdict(const char *fmt, ...)
{
    va_list args;

    if (cnc_quiet && !cnc_verbose)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
}

/*
 * The heading of a finding: which file, and which line of it. Counts before
 * printing, so QUIET changes what is shown and never what is returned.
 */
static VOID finding(const char *file, ULONG line, UWORD severity)
{
    if (severity == AMI_CFG_PROBLEM_ERROR)
        cnc_errors++;
    else
        cnc_warnings++;

    if ((UWORD)(cnc_errors + cnc_warnings) == 1)
        say("\nProblems\n");

    if (line > 0)
        say("\n  %s, line %lu:\n", (LONG)file, line);
    else
        say("\n  %s:\n", (LONG)file);
}

#define CNC_MAX_NOTES   24      /* more than any real drawer produces */
/* The longest file a note can name: the loader joins every interface path
   into char path[AMI_CFG_PATH_LEN + AMI_CFG_NAME_LEN + 8] (config_list.c)
   and hands the reporter that, so the same bound holds here whatever the
   filesystem allows a name to be (F-161). */
#define CNC_NOTE_FILE   (AMI_CFG_PATH_LEN + AMI_CFG_NAME_LEN + 8)
#define CNC_NOTE_TEXT   144
#define CNC_NOTE_HINT   144

static struct CncNote
{
    char  file[CNC_NOTE_FILE];
    ULONG line;
    char  text[CNC_NOTE_TEXT];
    char  hint[CNC_NOTE_HINT];
} cnc_note[CNC_MAX_NOTES];

/* How many arrived, which is not how many were kept -- see show_notes(). */
static UWORD cnc_notes;

/* The sentence for a problem: the assembled string when there is one,
   otherwise the words for its code. */
static const char *problem_text(const AmiCfgProblem *problem)
{
    if (problem->text != (const char *)0)
        return problem->text;

    return ami_cfg_advice(problem->text_code);
}

/* The advice for a problem: the assembled string when there is one, otherwise
   the words for its code.  NULL when the problem carries no advice. */
static const char *problem_advice(const AmiCfgProblem *problem)
{
    if (problem->hint_text != (const char *)0)
        return problem->hint_text;

    return ami_cfg_advice(problem->hint);
}

static VOID remember_note(const AmiCfgProblem *problem)
{
    struct CncNote *n;

    cnc_notes++;

    if (cnc_notes > (UWORD)CNC_MAX_NOTES)
        return;

    n = &cnc_note[cnc_notes - 1];

    tool_copy_string(n->file, sizeof(n->file), problem->file);
    n->line = problem->line;
    tool_copy_string(n->text, sizeof(n->text), problem_text(problem));
    tool_copy_string(n->hint, sizeof(n->hint),
                     (problem_advice(problem) != NULL)
                         ? problem_advice(problem) : "");
}

static VOID show_notes(VOID)
{
    UWORD shown = (cnc_notes < (UWORD)CNC_MAX_NOTES) ? cnc_notes
                                                     : (UWORD)CNC_MAX_NOTES;
    UWORD i;

    if (cnc_notes == 0)
        return;

    say("\nIgnored (Roadshow compatibility)\n");

    for (i = 0; i < shown; i++)
    {
        if (cnc_note[i].line > 0)
            say("\n  %s, line %lu:\n", (LONG)cnc_note[i].file,
                cnc_note[i].line);
        else
            say("\n  %s:\n", (LONG)cnc_note[i].file);

        note(cnc_note[i].text);

        if (cnc_note[i].hint[0] != '\0')
            note(cnc_note[i].hint);
    }

    if (cnc_notes > shown)
        say("\n  ...and %lu more.\n", (ULONG)(cnc_notes - shown));
}

/* The parser's own complaints, in the same shape as everything else here. */
static VOID cnc_report(const AmiCfgProblem *problem, APTR user)
{
    (VOID)user;

    if (problem->severity == AMI_CFG_PROBLEM_NOTE)
    {
        remember_note(problem);
        return;
    }

    finding(problem->file, problem->line, problem->severity);
    note(problem_text(problem));

    if (problem_advice(problem) != NULL)
        note(problem_advice(problem));
}

/*
 * The file the stack reads.  In a self-contained installation DEVS: means
 * AmiNetXDuo:Devs, the LAST member of the multi-assign, so the name alone
 * would open another stack's file on a machine that has one; the library
 * redirects through the same ami_cfg_resolve().  The findings keep naming
 * the DEVS: form, which is the documented name and stays a valid one.
 * Static: 128 bytes against a Shell command's 4 KB stack, and every caller
 * uses the result before the next call.
 */
static const char *cnc_where(const char *path)
{
    static char where[AMI_CFG_PATH_LEN];

    return ami_cfg_resolve(path, where, sizeof(where));
}

/*
 * A netmask is a run of ones followed by a run of zeroes and nothing else.
 * 0.0.0.0 and 255.255.255.255 both satisfy that. Both are rejected elsewhere
 * for being useless rather than for being malformed.
 */
static BOOL mask_is_contiguous(ULONG mask)
{
    ULONG inverted = ~mask;

    return (BOOL)(((inverted + 1UL) & inverted) == 0UL);
}

static BOOL same_network(ULONG a, ULONG b, ULONG mask)
{
    return (BOOL)((a & mask) == (b & mask));
}

/*
 * The first static interface whose network `addr` is on, or -1. DHCP
 * interfaces have no address until the lease arrives, so they are skipped.
 */
static LONG network_holding(const AmiConfig *cfg, ULONG addr)
{
    UWORD i;

    for (i = 0; i < cfg->interface_count; i++)
    {
        const AmiIfConfig *ifc = &cfg->interfaces[i];

        if (ifc->address == 0 || ifc->netmask == 0)
            continue;

        if (same_network(addr, ifc->address, ifc->netmask))
            return (LONG)i;
#ifdef AMINETXDUO_PTP
        /* The peer of a point-to-point link is reached through it. */
        if (ifc->destination != 0 && ifc->destination == addr)
            return (LONG)i;
#endif
    }

    return -1;
}

/* TRUE when an interface takes its IPv4 address by DHCP, whose lease brings
   the router and the name servers.  A LINKLOCAL (AutoIP) address brings
   neither, so it does not count (F-160). */
static BOOL any_dhcp_v4(const AmiConfig *cfg)
{
    UWORD i;

    for (i = 0; i < cfg->interface_count; i++)
    {
        if (cfg->interfaces[i].iptype == AMI_IPTYPE_DHCP)
            return TRUE;
    }

    return FALSE;
}

/* TRUE when at least one interface has an address written in its file. */
static BOOL any_static_address(const AmiConfig *cfg)
{
    UWORD i;

    for (i = 0; i < cfg->interface_count; i++)
    {
        if (cfg->interfaces[i].address != 0 && cfg->interfaces[i].netmask != 0)
            return TRUE;
    }

    return FALSE;
}

static VOID check_device(const char *path, const AmiIfConfig *ifc)
{
    const char *where;              /* where the device lives, or NULL */
    const char *src;                /* the file/drawer a finding names */
    BOOL        truncated;
    ULONG       line;

    if (ifc->device[0] == '\0')
        return;                     /* the parser has already said so */

    /* The loader truncates an interface name at AMI_CFG_IFNAME_MAX, so a name
       at that ceiling may not be the file's real name; the reconstructed path
       then names a different or nonexistent file.  The drawer is the truthful
       source then, at line 0 (F-158, as check_gateway() does). */
    truncated = ami_cfg_ifname_may_be_truncated(ifc->name);
    src       = truncated ? CNC_DIR_INTERFACES : path;

    where = tool_device_where(ifc->device);

    if (where != NULL && tool_stack_library_running())
        return;

    if (where == NULL)
    {
        /* Name the DEVICE the parser kept, not the first DEVICE keyword line:
           that may be empty or an earlier value a later line overwrote. */
        line = truncated ? 0 : ami_cfg_interface_device_line_file(path, ifc->device);

        finding(src, line, AMI_CFG_PROBLEM_ERROR);
        say("      %s not found; %s cannot come up\n",
            (LONG)ifc->device, (LONG)ifc->name);

        if (!cnc_quiet)
            tool_explain_device(ifc->device, ifc->unit, ifc->card);

        return;
    }

    if (tool_device_probe(ifc->device, ifc->unit, ifc->card) == 0)
        return;                     /* installed, and it opens */

    /* The driver is present, so the line to look at is the one that says which
       board: CARD when the file pins one, UNIT otherwise.  Name the value the
       parser kept, not the first occurrence (F-158). */
    if (ifc->card[0] != '\0')
        line = truncated ? 0 : ami_cfg_interface_card_line_file(path, ifc->card);
    else
        line = truncated ? 0 : ami_cfg_interface_unit_line_file(path, ifc->unit);

    finding(src, line, AMI_CFG_PROBLEM_ERROR);
    say("      %s cannot come up\n", (LONG)ifc->name);

    if (!cnc_quiet)
        tool_explain_device(ifc->device, ifc->unit, ifc->card);
}

/* The line an ADDRESS/NETMASK finding should name, or 0 when the file cannot
   be named: the loader truncates an interface name at AMI_CFG_IFNAME_MAX, so a
   name at that ceiling may not be the file's real name and the reconstructed
   path names a different or nonexistent file (F-158).  `netmask` selects the
   NETMASK helper; the ADDRESS helper otherwise. */
static ULONG addressing_line(const char *path, const AmiIfConfig *ifc,
                             ULONG want, BOOL netmask)
{
    if (ami_cfg_ifname_may_be_truncated(ifc->name))
        return 0;

    return netmask
        ? ami_cfg_interface_netmask_line_file(path, want)
        : ami_cfg_interface_address_line_file(path, want);
}

static VOID check_addressing(const char *path, const AmiIfConfig *ifc)
{
    const char *where;
    ULONG       host_bits;
    UWORD       prefix;

    /*
     * An interface addressed at run time has nothing to check: the file says
     * DHCP and the fields are zero. An ADDRESS line alongside CONFIGURE=DHCP
     * requests a particular lease, and is checked like a static address.
     */
    if (ifc->address == 0)
        return;

    /* The loader truncates an interface name at AMI_CFG_IFNAME_MAX, so a name
       at that ceiling may not be the file's real name; the reconstructed path
       then names a different or nonexistent file.  The drawer is the truthful
       source then, at line 0 (F-158, as check_gateway() does). */
    where = ami_cfg_ifname_may_be_truncated(ifc->name)
              ? CNC_DIR_INTERFACES : path;

    if ((ifc->address >> 24) == 127UL)
    {
        finding(where, addressing_line(path, ifc, ifc->address, FALSE), AMI_CFG_PROBLEM_ERROR);
        note("loopback address");
        return;
    }

    if ((ifc->address >> 24) >= 224UL)
    {
        finding(where, addressing_line(path, ifc, ifc->address, FALSE), AMI_CFG_PROBLEM_ERROR);
        note("multicast or reserved address (224.0.0.0 and up)");
        return;
    }

    if (ifc->netmask == 0)
    {
        finding(where, 0, AMI_CFG_PROBLEM_ERROR);
        note("no NETMASK");
        return;
    }

    if (!mask_is_contiguous(ifc->netmask))
    {
        char text[16];

        ami_config_format_ip(ifc->netmask, text, sizeof(text));

        finding(where, addressing_line(path, ifc, ifc->netmask, TRUE), AMI_CFG_PROBLEM_ERROR);
        say("      %s is not a netmask\n", (LONG)text);
        return;
    }

    prefix    = tool_prefix_len(ifc->netmask);
    host_bits = ifc->address & ~ifc->netmask;

#ifdef AMINETXDUO_PTP
    /* A point-to-point link has no network beyond its two ends: /32 is
       what it is meant to have, and the peer is DESTINATION. */
    if (ifc->destination != 0 || ifc->point_to_point == AMI_CFG_PTP_YES)
        return;
#endif

    if (prefix >= 31)
    {
        finding(where, addressing_line(path, ifc, ifc->netmask, TRUE), AMI_CFG_PROBLEM_WARN);
        say("      /%ld netmask: no other hosts on this network\n",
            (LONG)prefix);
        return;
    }

    if (host_bits == 0)
    {
        finding(where, addressing_line(path, ifc, ifc->address, FALSE), AMI_CFG_PROBLEM_ERROR);
        note("network address, not a host address");
        return;
    }

    if (host_bits == (~ifc->netmask & 0xffffffffUL))
    {
        finding(where, addressing_line(path, ifc, ifc->address, FALSE), AMI_CFG_PROBLEM_ERROR);
        note("broadcast address, not a host address");
        return;
    }

    if ((ifc->address >> 16) == 0xa9feUL &&
        ifc->iptype == AMI_IPTYPE_STATIC)
    {
        finding(where, addressing_line(path, ifc, ifc->address, FALSE), AMI_CFG_PROBLEM_WARN);
        note("static address in 169.254.0.0/16");
    }
}

static VOID check_gateway(const AmiConfig *cfg)
{
    const char *path;
    char        iface_path[TOOL_NAME_LEN * 2];
    ULONG       line;
    char        text[16];

    if (cfg->default_gateway == 0)
    {
        /*
         * No default route is only a problem when nothing will supply one: a
         * DHCP lease carries the router with it. A machine with no static IPv4
         * address has no IPv4 to route, which is the IPv6-only case.
         */
        if (any_dhcp_v4(cfg) || cfg->interface_count == 0 ||
            !any_static_address(cfg))
            return;

        finding("DEVS:Internet/routes", 0, AMI_CFG_PROBLEM_WARN);
        note("no default route");
        return;
    }

    /* Which file it came from, so the finding can name the one to edit.
       load_gateway() reads the compatibility file first and it wins, then the
       Roadshow routes file, then falls back to a GATEWAY= in the first
       interface file.  ami_cfg_default_gateway_line_file() replays that
       acceptance -- a bare GATEWAY=, DEFAULT= or DEFAULTGATEWAY=, first wins --
       so the named line is the one the loader took, and a GATEWAY on a line
       that also carries a DESTINATION is not mistaken for the default. */
    path = "DEVS:Internet/default_gateway";
    line = ami_cfg_default_gateway_line_file(path);
    if (line == 0)
    {
        path = "DEVS:Internet/routes";
        line = ami_cfg_default_gateway_line_file(path);
    }
    if (line == 0)
    {
        UWORD i;

        /* Neither file set the default, so load_gateway() fell back to a
           GATEWAY= in the first interface file.  The parser keeps the LAST
           GATEWAY whose value parses (IF_KEY_GATEWAY), so name that line, not
           the first GATEWAY keyword -- which could be an invalid value the
           parser skipped.  But the interface name is truncated to 15
           characters; a name at that ceiling may not be the file's real name,
           so the reconstructed path names a different or nonexistent file, and
           the drawer is the truthful source then. */
        for (i = 0; i < cfg->interface_count; i++)
        {
            if (cfg->interfaces[i].gateway != 0)
            {
                if (ami_cfg_ifname_may_be_truncated(cfg->interfaces[i].name))
                {
                    path = CNC_DIR_INTERFACES;
                    line = 0;
                }
                else
                {
                    tool_join_path(iface_path, sizeof(iface_path),
                                   CNC_DIR_INTERFACES, cfg->interfaces[i].name);
                    path = iface_path;
                    line = ami_cfg_interface_gateway_line_file(
                               path, cfg->interfaces[i].gateway);
                }
                break;
            }
        }
    }

    if ((cfg->default_gateway >> 24) == 127UL ||
        (cfg->default_gateway >> 24) >= 224UL)
    {
        ami_config_format_ip(cfg->default_gateway, text, sizeof(text));

        finding(path, line, AMI_CFG_PROBLEM_ERROR);
        say("      %s is not a router address\n", (LONG)text);
        return;
    }

    /*
     * The router has to be on a network this machine is on, or packets sent
     * to it never leave. Only checkable with a static address: with DHCP the
     * lease decides, and the router it hands out is correct by construction.
     */
    if (!any_static_address(cfg) || any_dhcp_v4(cfg))
        return;

    if (network_holding(cfg, cfg->default_gateway) >= 0)
        return;

    ami_config_format_ip(cfg->default_gateway, text, sizeof(text));

    finding(path, line, AMI_CFG_PROBLEM_ERROR);
    say("      router %s is not on a local network\n", (LONG)text);
}

static VOID check_resolver(const AmiConfig *cfg)
{
    UWORD i;

    if (cfg->resolver.nameserver_count == 0)
    {
        /* DHCP supplies name servers with the lease, as check_gateway() says. */
        if (any_dhcp_v4(cfg) || cfg->interface_count == 0 ||
            !any_static_address(cfg))
            return;

        finding("DEVS:Internet/name_resolution", 0, AMI_CFG_PROBLEM_WARN);
        note("no name server");
        return;
    }

    if (any_dhcp_v4(cfg) || !any_static_address(cfg))
        return;

    for (i = 0; i < cfg->resolver.nameserver_count; i++)
    {
        ULONG      server = cfg->resolver.nameserver[i];
        char       text[16];
        const char *path;
        ULONG      line;
        UWORD      occur = 0;
        UWORD      k;

        if (network_holding(cfg, server) >= 0)
            continue;               /* directly reachable */
        if (cfg->default_gateway != 0)
            continue;               /* reachable through the router */

        /*
         * Which file supplied this name server, so the finding names the one to
         * edit.  load_resolver() parses name_resolution first, then fills a
         * still-empty resolver from hosts, then from an interface file
         * (AmiTCP_NG's installer writes it there).  The first two are single
         * fixed files and replay exactly.  The interface fallback does not:
         * resolver_from_one() scans the raw drawer in enumeration order and
         * takes a NAMESERVER from any file -- even one ami_cfg_take_interface()
         * rejected -- while cfg->interfaces[] holds only accepted files,
         * alphabetically sorted, so the exact file is not recoverable here.
         * Name the drawer, not a guessed file.
         */
        for (k = 0; k < i; k++)
            if (cfg->resolver.nameserver[k] == server)
                occur++;            /* a duplicated value: its own occurrence */

        path = "DEVS:Internet/name_resolution";
        line = ami_cfg_nameserver_line_file(path, server, occur);

        if (line == 0)
        {
            path = "DEVS:Internet/hosts";
            line = ami_cfg_nameserver_line_file(path, server, occur);
        }

        if (line == 0)
        {
            path = CNC_DIR_INTERFACES;  /* drawer: exact file not provable */
            line = 0;
        }

        ami_config_format_ip(server, text, sizeof(text));

        finding(path, line, AMI_CFG_PROBLEM_WARN);
        say("      name server %s: not local, no default route\n",
            (LONG)text);
    }
}

static BOOL same_address6(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] &&
                  a[2] == b[2] && a[3] == b[3] &&
                  (a[0] | a[1] | a[2] | a[3]) != 0);
}

static VOID check_collisions(const AmiConfig *cfg)
{
    UWORD i;
    UWORD j;

    for (i = 0; i < cfg->interface_count; i++)
    {
        for (j = (UWORD)(i + 1); j < cfg->interface_count; j++)
        {
            const AmiIfConfig *a = &cfg->interfaces[i];
            const AmiIfConfig *b = &cfg->interfaces[j];

            if (tool_stricmp(tool_basename(a->device),
                             tool_basename(b->device)) == 0 &&
                a->unit == b->unit)
            {
                finding(CNC_DIR_INTERFACES, 0, AMI_CFG_PROBLEM_ERROR);
                say("      %s and %s both use %s unit %lu\n",
                    (LONG)a->name, (LONG)b->name, (LONG)b->device, b->unit);
                continue;
            }

            if (a->address != 0 && a->address == b->address)
            {
                char text[16];

                ami_config_format_ip(b->address, text, sizeof(text));

                finding(CNC_DIR_INTERFACES, 0, AMI_CFG_PROBLEM_ERROR);
                say("      %s and %s both have address %s\n",
                    (LONG)a->name, (LONG)b->name, (LONG)text);
            }

            /* The same fault one family over, which nothing checked: two
               ADDRESS6 lines naming one address is duplicate address
               detection failing on this machine's own wire.  Every pair of
               lines, now that an interface may carry more than one. */
            {
                UWORD m;
                UWORD n;

                for (m = 0; m < a->address6_count; m++)
                {
                    for (n = 0; n < b->address6_count; n++)
                    {
                        if (!same_address6(a->address6[m].addr,
                                           b->address6[n].addr))
                            continue;

                        {
                            char text[AMI_CFG_IP6_STRLEN];

                            tool_format_ip6(b->address6[n].addr, text,
                                            sizeof(text));

                            finding(CNC_DIR_INTERFACES, 0,
                                    AMI_CFG_PROBLEM_ERROR);
                            say("      %s and %s both have address %s\n",
                                (LONG)a->name, (LONG)b->name, (LONG)text);
                        }
                    }
                }
            }
        }
    }
}

static VOID check_storage_drawer(VOID)
{
    static char names[CNC_MAX_FILES][TOOL_NAME_LEN];
    ULONG       count;
    ULONG       i;

    if (!tool_exists(CNC_DIR_STORAGE))
        return;

    count = tool_list_dir(CNC_DIR_STORAGE, names, (ULONG)CNC_MAX_FILES, NULL);
    if (count == 0)
        return;

    say("\n  %s holds %lu interface file(s), not read at boot:\n",
        (LONG)CNC_DIR_STORAGE, count);
    for (i = 0; i < count; i++)
        say("      %s\n", (LONG)names[i]);
}

/*
 * The four netdb files, each line judged by the loader's own rules
 * (ami_netdb_line_verdict, F-094): what this says is dropped is what
 * bsdsocket.library drops.
 */
typedef struct NetdbFile
{
    const char *path;
    UWORD       kind;               /* AMI_NETDB_*                            */
    const char *shape;              /* what a line looks like                 */
} NetdbFile;

static const NetdbFile cnc_netdb[] =
{
    { "DEVS:Internet/hosts",     AMI_NETDB_HOSTS,
      "<address> <name> [alias...]" },
    { "DEVS:Internet/networks",  AMI_NETDB_NETWORKS,
      "<name> <network> [alias...]" },
    { "DEVS:Internet/protocols", AMI_NETDB_PROTOCOLS,
      "<name> <number 0-255> [alias...]" },
    { "DEVS:Internet/services",  AMI_NETDB_SERVICES,
      "<name> <port 0-65535>/<protocol> [alias...]" }
};

/*
 * A netdb file being read for check_netdb_file().  left is what remains of the
 * size Seek() measured, which is all the loader's one Read() asks for: the
 * checker reads no further.  FGetC() answers -1 for an error as for the end of
 * the file, and IoErr() is not specified after it, so a -1 with bytes still to
 * come means the check did not see the whole file (F-094); whether that was a
 * failed read or the file shrinking, what the loader got cannot be known.
 */
typedef struct CncNetdbRead
{
    BPTR  file;
    ULONG left;
    BOOL  cut;                      /* the line was longer than the buffer   */
    BOOL  nul;                      /* a NUL ended it                        */
    BOOL  failed;                   /* -1 before the measured end            */
} CncNetdbRead;

static LONG cnc_netdb_getc(CncNetdbRead *r)
{
    LONG c;

    if (r->left == 0)
        return -1;                  /* the measured end: bytes added since are
                                       not read by the loader either */

    c = FGetC(r->file);
    if (c == -1)
        r->failed = TRUE;
    else
        r->left--;

    return c;
}

/*
 * One line of a netdb file, ended as ami_cfg_next_line() ends the loader's:
 * '\n', '\r', CRLF or LFCR.  FGets() ends a line only at '\n', so a file with
 * bare CRs reached the verdict as one line, a broken record hidden behind a
 * good one, while the loader read two (F-094).  The length, or -1 at the end
 * of the file or on a failed read; r->cut when the line was longer than the
 * buffer, the rest read and dropped; r->nul when a NUL ended it, where the
 * loader's text ends too.
 */
static LONG cnc_netdb_line(CncNetdbRead *r, char *buf, ULONG size)
{
    LONG  c;
    ULONG n    = 0;
    BOOL  seen = FALSE;

    r->cut = FALSE;
    r->nul = FALSE;

    while ((c = cnc_netdb_getc(r)) != -1)
    {
        seen = TRUE;

        if (c == 0)
        {
            r->nul = TRUE;
            break;
        }

        if (c == '\n' || c == '\r')
        {
            LONG pair = (c == '\r') ? '\n' : '\r';
            LONG next = cnc_netdb_getc(r);

            if (next != -1 && next != pair)
            {
                (VOID)UnGetC(r->file, next);
                r->left++;
            }
            break;
        }

        if (n + 1 < size)
            buf[n++] = (char)c;
        else
            r->cut = TRUE;
    }

    buf[n] = '\0';

    if (r->failed)
        return -1;

    return seen ? (LONG)n : -1L;
}

static VOID check_netdb_file(const NetdbFile *spec)
{
    char  line[CNC_LINE_MAX];
    char  word[CNC_LINE_MAX];       /* a column is never longer than its line */
    BPTR  file;
    ULONG lineno = 0;
    UWORD said   = 0;
    LONG  size;
    CncNetdbRead rd;

    file = Open((CONST_STRPTR)cnc_where(spec->path), MODE_OLDFILE);
    if (file == (BPTR)0)
        return;                     /* missing is normal: there are built-ins */

    /*
     * The loader measures the file the same way (ami_cfg_read_file()) and does
     * not read one it cannot measure, or one past AMI_CFG_FILE_MAX: its lines
     * are not the ones in use, so they are not judged as if they were.
     */
    size = (Seek(file, 0, OFFSET_END) >= 0) ? Seek(file, 0, OFFSET_BEGINNING)
                                             : -1L;
    if (size < 0)
    {
        finding(spec->path, 0, AMI_CFG_PROBLEM_WARN);
        say("      size unreadable; built-in list used\n");
        Close(file);
        return;
    }
    if (size > (LONG)AMI_CFG_FILE_MAX)
    {
        finding(spec->path, 0, AMI_CFG_PROBLEM_WARN);
        say("      %ld bytes, above the %ld-byte limit; built-in list used\n",
            size, (LONG)AMI_CFG_FILE_MAX);
        Close(file);
        return;
    }

    rd.file   = file;
    rd.left   = (ULONG)size;
    rd.failed = FALSE;

    while (cnc_netdb_line(&rd, line, sizeof(line)) >= 0)
    {
        UWORD verdict;

        lineno++;

        /*
         * The loader reads a long line whole, so it is said that this one was
         * not looked at rather than judging a piece; it counts towards the
         * five like any other finding.  ';' is not a comment to the loader,
         * so it is not one here either.
         */
        verdict = rd.cut ? AMI_NETDB_LINE_SKIP
                         : ami_netdb_line_verdict(spec->kind, line, word,
                                                  sizeof(word));

        if (rd.cut)
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_NOTE);
            say("      line longer than %ld characters; not checked\n",
                (LONG)(sizeof(line) - 1));
            said++;
        }
        else if (verdict == AMI_NETDB_LINE_SHORT)
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_WARN);
            say("      too few columns; ignored\n");
            note(spec->shape);
            said++;
        }
        else if (verdict == AMI_NETDB_LINE_BAD)
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_WARN);
            say("      \"%s\": invalid value; line ignored\n", (LONG)word);
            note(spec->shape);
            said++;
        }
        else if (verdict == AMI_NETDB_LINE_CUT)
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_WARN);
            say("      only %ld words read; \"%s\" and after ignored\n",
                (LONG)AMI_NETDB_WORDS, (LONG)word);
            said++;
        }

        /* ami_cfg_read_file() hands the parser a C string, so the loader
           reads up to here and no further. */
        if (rd.nul)
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_WARN);
            say("      NUL character; rest of file ignored\n");
            break;
        }

        if (said >= 5)
        {
            finding(spec->path, 0, AMI_CFG_PROBLEM_WARN);
            note("more lines like this not listed");
            break;
        }
    }

    if (rd.failed)
    {
        finding(spec->path, 0, AMI_CFG_PROBLEM_WARN);
        say("      read error; not completely checked\n");
    }

    Close(file);
}

static VOID check_one_address6(const char *path, const ULONG addr[4], ULONG line)
{
    char text[AMI_CFG_IP6_STRLEN];

    if ((addr[0] | addr[1] | addr[2] | addr[3]) == 0)
        return;

    tool_format_ip6(addr, text, sizeof(text));

    /* ::1, RFC 4291 2.5.3.  The IPv6 127.0.0.1: it always means "this
       machine", so no other machine can reach an interface that has one. */
    if (addr[0] == 0 && addr[1] == 0 &&
        addr[2] == 0 && addr[3] == 1)
    {
        finding(path, line, AMI_CFG_PROBLEM_ERROR);
        note("loopback address");
        return;
    }

    /* ff00::/8, RFC 4291 2.7.  A group, not a machine. */
    if ((addr[0] & 0xFF000000UL) == 0xFF000000UL)
    {
        finding(path, line, AMI_CFG_PROBLEM_ERROR);
        say("      %s is a multicast address\n", (LONG)text);
        return;
    }

    /* fe80::/10, RFC 4291 2.5.6.  The interface derives its own link-local
       from the MAC in every mode, so a written one never reaches off-wire. */
    if ((addr[0] & 0xFFC00000UL) == 0xFE800000UL)
    {
        finding(path, line, AMI_CFG_PROBLEM_WARN);
        say("      %s is link-local; the interface has one already\n",
            (LONG)text);
    }
}

static VOID check_addressing6(const char *path, const AmiIfConfig *ifc)
{
    UWORD m;
    UWORD n;

    /* The line an address was read from, without the loader carrying it in
       AmiIp6Address: that struct is public ABI through
       ami_config_load_interface(), so the loader cannot grow it for a
       diagnostic (F-158).  The first line the parser keeps with this value is
       the one it took. */
    for (m = 0; m < ifc->address6_count; m++)
        check_one_address6(path, ifc->address6[m].addr,
                           ami_cfg_address6_line_file(path, ifc->name,
                                                      ifc->address6[m].addr,
                                                      0));

#ifdef AMINETXDUO_PTP
    /* The file says point-to-point the two ways check_addressing() reads. */
    if (ifc->have_destination6 && ifc->destination == 0 &&
        ifc->point_to_point != AMI_CFG_PTP_YES)
    {
        finding(path, ami_cfg_keyword_line_file(path, "DESTINATION6"),
                AMI_CFG_PROBLEM_WARN);
        note("DESTINATION6 is the peer of a point-to-point interface and is "
             "ignored on any other; this file has neither POINTTOPOINT=YES "
             "nor DESTINATION");
    }
#endif

    /* Two ADDRESS6 lines on ONE interface naming one address, which the
       cross-interface walk above cannot see because it never compares an
       interface with itself. */
    for (m = 0; m + 1 < ifc->address6_count; m++)
    {
        for (n = (UWORD)(m + 1); n < ifc->address6_count; n++)
        {
            char  text[AMI_CFG_IP6_STRLEN];
            UWORD prior;
            UWORD k;

            if (!same_address6(ifc->address6[m].addr, ifc->address6[n].addr))
                continue;

            /* The offending entry is the later of the two; its line is the
               occurrence after the equal addresses before it. */
            prior = 0;
            for (k = 0; k < n; k++)
                if (same_address6(ifc->address6[k].addr,
                                  ifc->address6[n].addr))
                    prior++;

            tool_format_ip6(ifc->address6[n].addr, text, sizeof(text));

            finding(path,
                    ami_cfg_address6_line_file(path, ifc->name,
                                               ifc->address6[n].addr, prior),
                    AMI_CFG_PROBLEM_ERROR);
            say("      ADDRESS6 %s repeated\n", (LONG)text);
        }
    }
}

/* Static: an AmiConfig is far larger than a Shell command's 4 KB stack. */
static AmiConfig cnc_config;

static VOID check_interfaces(const AmiConfig *cfg)
{
    char  path[TOOL_NAME_LEN * 2];
    UWORD i;

    for (i = 0; i < cfg->interface_count; i++)
    {
        const AmiIfConfig *ifc = &cfg->interfaces[i];

        tool_join_path(path, sizeof(path), CNC_DIR_INTERFACES, ifc->name);

        if (cnc_verbose)
            say("  checked %s\n", (LONG)path);

        check_device(path, ifc);
        check_addressing(path, ifc);
        check_addressing6(path, ifc);

        if (tool_break())
            return;
    }
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    ULONG          i;
    LONG           rc;
    LONG           load;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_QUIET]   = 0;
    args[ARG_VERBOSE] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[QUIET] [VERBOSE]",
                   "Check the network configuration files.");
        return RETURN_ERROR;
    }

    cnc_quiet   = (args[ARG_QUIET]   != 0) ? TRUE : FALSE;
    cnc_verbose = (args[ARG_VERBOSE] != 0) ? TRUE : FALSE;

    if (!tool_exists(cnc_where("DEVS:Internet")) &&
        !tool_exists(cnc_where(CNC_DIR_INTERFACES)))
    {
        cnc_errors++;

        tool_error("no network configuration");
        tool_explain_no_interfaces();

        verdict("Not configured.\n");

        FreeArgs(rda);
        return RETURN_WARN;
    }

    ami_config_set_reporter(cnc_report, NULL);
    load = ami_config_load(&cnc_config);
    ami_config_set_reporter(NULL, NULL);

    /* A configuration only partly read, for want of memory, is not a clean
       one: nothing below could say what the unread part holds (F-157). */
    if (load == AMI_CFG_ERR_NOMEM)
    {
        tool_error("out of memory; configuration not checked");
        ami_config_free(&cnc_config);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (cnc_verbose)
    {
        say("Reading DEVS:Internet and %s\n",
            (LONG)CNC_DIR_INTERFACES);
    }

    check_interfaces(&cnc_config);
    check_collisions(&cnc_config);
    check_gateway(&cnc_config);
    check_resolver(&cnc_config);

    for (i = 0; i < (ULONG)(sizeof(cnc_netdb) / sizeof(cnc_netdb[0])); i++)
    {
        /* Said before the check, which can find the file is not used. */
        if (cnc_verbose && tool_exists(cnc_where(cnc_netdb[i].path)))
            say("  checking %s\n", (LONG)cnc_netdb[i].path);

        check_netdb_file(&cnc_netdb[i]);
    }

    check_storage_drawer();

    /* Last, and under a heading of their own: they are not faults, and the
       verdict below does not count them. */
    show_notes();

    if (cnc_errors == 0 && cnc_warnings == 0)
    {
        verdict("\nNo problems found.\n");
        rc = RETURN_OK;
    }
    else
    {
        verdict("\n%lu error(s), %lu warning(s).\n",
                (ULONG)cnc_errors, (ULONG)cnc_warnings);
        rc = RETURN_WARN;
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    /* ami_config_load() allocates the interface list and this command is its
       owner. One exit, so one free. */
    ami_config_free(&cnc_config);

    FreeArgs(rda);
    return (int)rc;
}
