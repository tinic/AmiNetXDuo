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
        say("\nWhat is wrong\n");

    if (line > 0)
        say("\n  %s, line %lu:\n", (LONG)file, line);
    else
        say("\n  %s:\n", (LONG)file);
}

#define CNC_MAX_NOTES   24      /* more than any real drawer produces */
#define CNC_NOTE_FILE   48
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

    say("\nLines that are read and do nothing\n");
    note("These are correct as written.  This stack reads them so that a "
         "Roadshow configuration file works here unchanged, and then acts on "
         "none of them.  Nothing below needs fixing, and no other command "
         "mentions it.");

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

/* TRUE when `line` begins with `keyword` as a whole word. */
static BOOL line_starts_with(const char *line, const char *keyword)
{
    ULONG n = 0;

    while (keyword[n] != '\0')
        n++;

    if (tool_stricmp_n(line, keyword, n) != 0)
        return FALSE;

    /* "DEVICELESS" does not begin with the keyword DEVICE. */
    return (BOOL)(line[n] == '\0' || line[n] == '=' ||
                  line[n] == ' '  || line[n] == '\t' ||
                  line[n] == '\n' || line[n] == '\r');
}

/* The 1-based line of `path` whose first word is `keyword`, or 0. */
static ULONG keyword_line(const char *path, const char *keyword)
{
    char  line[CNC_LINE_MAX];
    BPTR  file;
    ULONG lineno = 0;
    ULONG found  = 0;

    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (file == (BPTR)0)
        return 0;

    while (FGets(file, (STRPTR)line, (LONG)sizeof(line)) != NULL)
    {
        const char *p = line;

        lineno++;

        while (*p == ' ' || *p == '\t')
            p++;

        if (line_starts_with(p, keyword))
        {
            found = lineno;
            break;
        }
    }

    Close(file);

    return found;
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
    }

    return -1;
}

static BOOL any_dynamic_v4(const AmiConfig *cfg)
{
    UWORD i;

    for (i = 0; i < cfg->interface_count; i++)
    {
        const AmiIfConfig *ifc = &cfg->interfaces[i];

        if (ifc->iptype != AMI_IPTYPE_STATIC &&
            ifc->iptype != AMI_IPTYPE_NONE)
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
    const char *where;
    ULONG       line;

    if (ifc->device[0] == '\0')
        return;                     /* the parser has already said so */

    where = tool_device_where(ifc->device);
    line  = keyword_line(path, "DEVICE");

    if (where != NULL && tool_stack_library_running())
        return;

    if (where == NULL)
    {
        finding(path, line, AMI_CFG_PROBLEM_ERROR);
        say("      this names %s, and %s cannot come up without it\n",
            (LONG)ifc->device, (LONG)ifc->name);

        if (!cnc_quiet)
            tool_explain_device(ifc->device, ifc->unit, ifc->card);

        return;
    }

    if (tool_device_probe(ifc->device, ifc->unit, ifc->card) == 0)
        return;                     /* installed, and it opens */

    /* The driver is present, so the line to look at is the one that says which
       board: CARD when the file pins one, UNIT otherwise. */
    line = keyword_line(path, ifc->card[0] != '\0' ? "CARD" : "UNIT");

    finding(path, line, AMI_CFG_PROBLEM_ERROR);
    say("      %s will not come up with this configuration\n",
        (LONG)ifc->name);

    if (!cnc_quiet)
        tool_explain_device(ifc->device, ifc->unit, ifc->card);
}

static VOID check_addressing(const char *path, const AmiIfConfig *ifc)
{
    ULONG host_bits;
    UWORD prefix;

    /*
     * An interface addressed at run time has nothing to check: the file says
     * DHCP and the fields are zero. An ADDRESS line alongside CONFIGURE=DHCP
     * requests a particular lease, and is checked like a static address.
     */
    if (ifc->address == 0)
        return;

    if ((ifc->address >> 24) == 127UL)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("this is a loopback address. It always means \"this machine\", "
             "so no other machine can reach an interface that has one.");
        note("Use an address on the local network, usually 192.168.x.y, or "
             "set CONFIGURE = DHCP to be given one.");
        return;
    }

    if ((ifc->address >> 24) >= 224UL)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("addresses from 224.0.0.0 upwards are reserved for multicast "
             "and for future use. A machine cannot have one.");
        return;
    }

    if (ifc->netmask == 0)
    {
        finding(path, 0, AMI_CFG_PROBLEM_ERROR);
        note("the interface has an address and no NETMASK. Without a netmask "
             "the stack cannot tell which machines are on this network.");
        note("Add  NETMASK = 255.255.255.0, which is correct on almost "
             "every home network.");
        return;
    }

    if (!mask_is_contiguous(ifc->netmask))
    {
        char text[16];

        ami_config_format_ip(ifc->netmask, text, sizeof(text));

        finding(path, keyword_line(path, "NETMASK"), AMI_CFG_PROBLEM_ERROR);
        say("      %s is not a netmask: a netmask is all ones and then\n",
            (LONG)text);
        say("      all zeroes, with nothing mixed in between\n");
        note("The usual ones are 255.255.255.0, 255.255.0.0 and 255.0.0.0.");
        return;
    }

    prefix    = tool_prefix_len(ifc->netmask);
    host_bits = ifc->address & ~ifc->netmask;

    if (prefix >= 31)
    {
        finding(path, keyword_line(path, "NETMASK"), AMI_CFG_PROBLEM_WARN);
        say("      a /%ld netmask leaves no room for anything else on this\n",
            (LONG)prefix);
        say("      network, so nothing here can be reached directly\n");
        note("255.255.255.0 is correct on almost every home network.");
        return;
    }

    if (host_bits == 0)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("this is the network's own address rather than a machine's, "
             "because every bit the netmask leaves free is zero. Nothing can "
             "reach it.");
        note("Raise the last part of the address. The router is usually .1, "
             "so use .10 or higher.");
        return;
    }

    if (host_bits == (~ifc->netmask & 0xffffffffUL))
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("this is the broadcast address of its own network. That address "
             "reaches every machine at once, so it cannot belong to one. "
             "Nothing answers it.");
        note("Lower the last part of the address. Use .10 or higher.");
        return;
    }

    if ((ifc->address >> 16) == 0xa9feUL &&
        ifc->iptype == AMI_IPTYPE_STATIC)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_WARN);
        note("169.254.x.y is the range a machine picks for itself when "
             "nothing hands out addresses. An address set by hand can "
             "collide with a machine that picked the same one.");
        note("Use CONFIGURE = LINKLOCAL to have one picked safely, or an "
             "address on the local network.");
    }
}

static VOID check_gateway(const AmiConfig *cfg)
{
    const char *path;
    ULONG       line;
    char        text[16];

    if (cfg->default_gateway == 0)
    {
        /*
         * No default route is only a problem when nothing will supply one: a
         * DHCP lease carries the router with it. A machine with no static IPv4
         * address has no IPv4 to route, which is the IPv6-only case.
         */
        if (any_dynamic_v4(cfg) || cfg->interface_count == 0 ||
            !any_static_address(cfg))
            return;

        finding("DEVS:Internet/routes", 0, AMI_CFG_PROBLEM_WARN);
        note("there is no default route. This machine can reach other "
             "machines on its own network, and nothing beyond it.");
        note("Put  GATEWAY = <router address>  in "
             "DEVS:Internet/routes, or run NetSetup. On a home network the "
             "router is the box the broadband comes into, usually at .1.");
        return;
    }

    /* Which file it came from, so the finding can name the one to edit. */
    path = "DEVS:Internet/routes";
    line = keyword_line(path, "GATEWAY");
    if (line == 0)
    {
        if (keyword_line("DEVS:Internet/default_gateway", "GATEWAY") != 0)
        {
            path = "DEVS:Internet/default_gateway";
            line = keyword_line(path, "GATEWAY");
        }
    }

    if ((cfg->default_gateway >> 24) == 127UL ||
        (cfg->default_gateway >> 24) >= 224UL)
    {
        ami_config_format_ip(cfg->default_gateway, text, sizeof(text));

        finding(path, line, AMI_CFG_PROBLEM_ERROR);
        say("      %s cannot be a router: that address never belongs to\n",
            (LONG)text);
        say("      another machine\n");
        return;
    }

    /*
     * The router has to be on a network this machine is on, or packets sent
     * to it never leave. Only checkable with a static address: with DHCP the
     * lease decides, and the router it hands out is correct by construction.
     */
    if (!any_static_address(cfg) || any_dynamic_v4(cfg))
        return;

    if (network_holding(cfg, cfg->default_gateway) >= 0)
        return;

    ami_config_format_ip(cfg->default_gateway, text, sizeof(text));

    finding(path, line, AMI_CFG_PROBLEM_ERROR);
    say("      the router %s is not on any network this machine is\n",
        (LONG)text);
    say("      on, so nothing sent to it can arrive\n");
    note("A router must be reachable directly. Check its address against "
         "the ADDRESS and NETMASK of the interface. All but the last part "
         "of the two addresses normally match.");
}

static VOID check_resolver(const AmiConfig *cfg)
{
    static const char *const path = "DEVS:Internet/name_resolution";
    UWORD                    i;

    if (cfg->resolver.nameserver_count == 0)
    {
        /* DHCP supplies name servers with the lease, as check_gateway() says. */
        if (any_dynamic_v4(cfg) || cfg->interface_count == 0 ||
            !any_static_address(cfg))
            return;

        finding(path, 0, AMI_CFG_PROBLEM_WARN);
        note("no name server is configured, so names like www.example.com "
             "cannot be looked up. Numeric addresses still work.");
        note("Put  NAMESERVER <address>  in that file, or run NetSetup. On a "
             "home network the router is usually the name server too.");
        return;
    }

    if (any_dynamic_v4(cfg) || !any_static_address(cfg))
        return;

    for (i = 0; i < cfg->resolver.nameserver_count; i++)
    {
        ULONG server = cfg->resolver.nameserver[i];
        char  text[16];

        if (network_holding(cfg, server) >= 0)
            continue;               /* directly reachable */
        if (cfg->default_gateway != 0)
            continue;               /* reachable through the router */

        ami_config_format_ip(server, text, sizeof(text));

        finding(path, keyword_line(path, "NAMESERVER"), AMI_CFG_PROBLEM_WARN);
        say("      the name server %s is not on this machine's network\n",
            (LONG)text);
        say("      and there is no default route to reach it through\n");
        note("Give this machine a router (GATEWAY in "
             "DEVS:Internet/routes), or use a name server on the local "
             "network. On a home network that is the router itself.");
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
                say("      %s and %s both claim %s unit %lu, and one card\n",
                    (LONG)a->name, (LONG)b->name, (LONG)b->device, b->unit);
                say("      cannot be two interfaces\n");
                note("Remove the file that is left over, or give one of the "
                     "two the UNIT of a second card.");
                continue;
            }

            if (a->address != 0 && a->address == b->address)
            {
                char text[16];

                ami_config_format_ip(b->address, text, sizeof(text));

                finding(CNC_DIR_INTERFACES, 0, AMI_CFG_PROBLEM_ERROR);
                say("      %s and %s are both %s, and two interfaces cannot\n",
                    (LONG)a->name, (LONG)b->name, (LONG)text);
                say("      share an address\n");
                note("Give one of them an address of its own, on its own "
                     "network.");
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
                            say("      %s and %s are both %s, and two "
                                "interfaces cannot\n",
                                (LONG)a->name, (LONG)b->name, (LONG)text);
                            say("      share an address\n");
                            note("Give one of them an ADDRESS6 of its own.");
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

    say("\n  %s holds %lu interface file(s):\n", (LONG)CNC_DIR_STORAGE, count);
    for (i = 0; i < count; i++)
        say("      %s\n", (LONG)names[i]);
    note("Nothing starts these: only DEVS:NetInterfaces is read at boot. "
         "Move one there to use it.");
}

#define CNC_COL_ADDRESS     0       /* a dotted quad                          */
#define CNC_COL_NUMBER      1       /* a plain decimal number                 */
#define CNC_COL_PORT        2       /* <number>/<protocol>                    */

typedef struct NetdbFile
{
    const char *path;
    UWORD       column;             /* which column, 0-based                  */
    UWORD       kind;               /* CNC_COL_*                              */
    const char *shape;              /* what a line looks like                 */
} NetdbFile;

static const NetdbFile cnc_netdb[] =
{
    { "DEVS:Internet/hosts",     0, CNC_COL_ADDRESS,
      "<address> <name> [alias...]" },
    { "DEVS:Internet/protocols", 1, CNC_COL_NUMBER,
      "<name> <number> [alias...]" },
    { "DEVS:Internet/services",  1, CNC_COL_PORT,
      "<name> <port>/<protocol> [alias...]" }
};

/* The `index`th whitespace-separated word of `line`, copied out. */
static BOOL word_at(const char *line, UWORD index, char *out, ULONG outlen)
{
    UWORD n = 0;

    out[0] = '\0';

    for (;;)
    {
        ULONG len = 0;

        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '\0' || *line == '\n' || *line == '\r' || *line == '#')
            return FALSE;

        while (line[len] != '\0' && line[len] != ' ' && line[len] != '\t' &&
               line[len] != '\n' && line[len] != '\r')
        {
            len++;
        }

        if (n == index)
        {
            ULONG i;

            for (i = 0; i < len && i + 1 < outlen; i++)
                out[i] = line[i];
            out[i] = '\0';
            return TRUE;
        }

        line += len;
        n++;
    }
}

static BOOL is_decimal(const char *text)
{
    ULONG i;

    if (text[0] == '\0')
        return FALSE;

    for (i = 0; text[i] != '\0'; i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return FALSE;
    }

    return TRUE;
}

/* "80/tcp": a number, a slash, and a protocol name that is not empty. */
static BOOL is_port_and_protocol(const char *text)
{
    ULONG i;

    for (i = 0; text[i] != '\0' && text[i] != '/'; i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return FALSE;
    }

    return (BOOL)(i > 0 && text[i] == '/' && text[i + 1] != '\0');
}

static BOOL column_is_valid(UWORD kind, const char *text)
{
    ULONG parsed;

    switch (kind)
    {
        case CNC_COL_ADDRESS:   return ami_config_parse_ip(text, &parsed);
        case CNC_COL_NUMBER:    return is_decimal(text);
        case CNC_COL_PORT:      return is_port_and_protocol(text);
        default:                return TRUE;
    }
}

/*
 * AmiTCP installations put resolver settings in the hosts file, and
 * ami_config_load() reads them from there. They are not netdb entries, so they
 * must not be reported as broken ones.
 */
static BOOL is_resolver_line(const char *line)
{
    return (BOOL)(line_starts_with(line, "NAMESERVER") ||
                  line_starts_with(line, "DOMAIN") ||
                  line_starts_with(line, "SEARCH") ||
                  line_starts_with(line, "HOSTNAME") ||
                  line_starts_with(line, "HOST"));
}

static VOID check_netdb_file(const NetdbFile *spec)
{
    char  line[CNC_LINE_MAX];
    char  word[64];
    BPTR  file;
    ULONG lineno = 0;
    UWORD said   = 0;

    file = Open((CONST_STRPTR)spec->path, MODE_OLDFILE);
    if (file == (BPTR)0)
        return;                     /* missing is normal: there are built-ins */

    while (FGets(file, (STRPTR)line, (LONG)sizeof(line)) != NULL)
    {
        const char *p = line;

        lineno++;

        while (*p == ' ' || *p == '\t')
            p++;

        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#' || *p == ';')
            continue;
        if (is_resolver_line(p))
            continue;

        if (!word_at(p, spec->column, word, sizeof(word)))
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_WARN);
            say("      this line has too few columns, so it is ignored\n");
            note(spec->shape);
            said++;
        }
        else if (!column_is_valid(spec->kind, word))
        {
            finding(spec->path, lineno, AMI_CFG_PROBLEM_WARN);
            say("      \"%s\" is not what this column holds, so the line is\n",
                (LONG)word);
            say("      ignored\n");
            note(spec->shape);
            said++;
        }

        if (said >= 5)
        {
            finding(spec->path, 0, AMI_CFG_PROBLEM_WARN);
            note("more lines after this one have the same problem. They are "
                 "not all listed.");
            break;
        }
    }

    Close(file);
}

static VOID check_one_address6(const char *path, const ULONG addr[4])
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
        finding(path, keyword_line(path, "ADDRESS6"), AMI_CFG_PROBLEM_ERROR);
        note("::1 is the loopback address. It always means \"this machine\", "
             "so no other machine can reach an interface that has one.");
        note("Use an address from the prefix this network uses, or leave "
             "ADDRESS6 out and set CONFIGURE6 = AUTO to be given one.");
        return;
    }

    /* ff00::/8, RFC 4291 2.7.  A group, not a machine. */
    if ((addr[0] & 0xFF000000UL) == 0xFF000000UL)
    {
        finding(path, keyword_line(path, "ADDRESS6"), AMI_CFG_PROBLEM_ERROR);
        say("      %s is a multicast address: it names a group of\n",
            (LONG)text);
        say("      machines and cannot be one machine's own address\n");
        return;
    }

    /* fe80::/10, RFC 4291 2.5.6.  The interface derives its own link-local
       from the MAC in every mode, so a written one never reaches off-wire. */
    if ((addr[0] & 0xFFC00000UL) == 0xFE800000UL)
    {
        finding(path, keyword_line(path, "ADDRESS6"), AMI_CFG_PROBLEM_WARN);
        say("      %s is a link-local address, which reaches only this\n",
            (LONG)text);
        say("      wire, and the interface gives itself one already\n");
        note("For an address that reaches further, use the prefix this "
             "network uses, or CONFIGURE6 = AUTO to be given one.");
    }
}

static VOID check_addressing6(const char *path, const AmiIfConfig *ifc)
{
    UWORD m;
    UWORD n;

    for (m = 0; m < ifc->address6_count; m++)
        check_one_address6(path, ifc->address6[m].addr);

    /* Two ADDRESS6 lines on ONE interface naming one address, which the
       cross-interface walk above cannot see because it never compares an
       interface with itself. */
    for (m = 0; m + 1 < ifc->address6_count; m++)
    {
        for (n = (UWORD)(m + 1); n < ifc->address6_count; n++)
        {
            char text[AMI_CFG_IP6_STRLEN];

            if (!same_address6(ifc->address6[m].addr, ifc->address6[n].addr))
                continue;

            tool_format_ip6(ifc->address6[n].addr, text, sizeof(text));

            finding(path, keyword_line(path, "ADDRESS6"),
                    AMI_CFG_PROBLEM_ERROR);
            say("      ADDRESS6 names %s twice, and the second line adds\n",
                (LONG)text);
            say("      nothing the first did not\n");
            note("Remove the repeated line, or give it an address of its "
                 "own.");
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
                   "Check the network configuration files and say what is "
                   "wrong.");
        return RETURN_ERROR;
    }

    cnc_quiet   = (args[ARG_QUIET]   != 0) ? TRUE : FALSE;
    cnc_verbose = (args[ARG_VERBOSE] != 0) ? TRUE : FALSE;

    if (!tool_exists("DEVS:Internet") && !tool_exists(CNC_DIR_INTERFACES))
    {
        cnc_errors++;

        tool_error("this machine has no network configuration at all");
        tool_explain_no_interfaces();

        verdict("The network has never been configured on this machine.\n");

        FreeArgs(rda);
        return RETURN_WARN;
    }

    ami_config_set_reporter(cnc_report, NULL);
    (VOID)ami_config_load(&cnc_config);
    ami_config_set_reporter(NULL, NULL);

    if (cnc_verbose)
    {
        say("Reading the configuration in DEVS:Internet and %s\n",
            (LONG)CNC_DIR_INTERFACES);
    }

    check_interfaces(&cnc_config);
    check_collisions(&cnc_config);
    check_gateway(&cnc_config);
    check_resolver(&cnc_config);

    for (i = 0; i < (ULONG)(sizeof(cnc_netdb) / sizeof(cnc_netdb[0])); i++)
    {
        if (cnc_verbose && tool_exists(cnc_netdb[i].path))
            say("  checked %s\n", (LONG)cnc_netdb[i].path);

        check_netdb_file(&cnc_netdb[i]);
    }

    check_storage_drawer();

    /* Last, and under a heading of their own: they are not faults, and the
       verdict below does not count them. */
    show_notes();

    if (cnc_errors == 0 && cnc_warnings == 0)
    {
        verdict("\nThe network configuration has nothing wrong with it.\n");
        rc = RETURN_OK;
    }
    else
    {
        verdict("\n%lu problem(s) will stop the network from working.\n"
                "%lu more need attention.\n",
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
