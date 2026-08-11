/*
 * CheckNetConfig, read the network configuration and report what is wrong
 * with it.
 *
 *     CheckNetConfig QUIET/S,VERBOSE/S
 *
 * Reads every file the stack would read and prints a file, a line and
 * something to type. Works with the network down: nothing here opens
 * bsdsocket.library or asks the running stack anything.
 *
 * src/config already reports bad syntax, unknown keywords, a missing DEVICE
 * line and a static interface with no ADDRESS, each with a line number,
 * through the reporter hook tool_config_watch() installs. Those are forwarded
 * rather than rewritten. The checks here are the ones needing more than a
 * single line:
 *
 *   * the driver named exists on this machine, and the unit it names opens;
 *   * the netmask is a mask at all, and the address is a host on it rather
 *     than the network or the broadcast address;
 *   * the router is on a network one of the interfaces is on;
 *   * no two interfaces claim the same card, or the same address;
 *   * a name server can be reached, or is at least on the far side of a
 *     default route that exists;
 *   * the netdb files parse as the columns they are meant to be.
 *
 * Return codes: 0 when nothing was found, 5 (RETURN_WARN) when something was,
 * so a startup script can say
 *
 *     CheckNetConfig QUIET
 *     IF WARN
 *         echo "The network configuration needs attention."
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

/* Long enough for any line in these files; longer ones are read in pieces. */
#define CNC_LINE_MAX        200

/* ---------------------------------------------------------------- output,
 *
 * QUIET suppresses the findings but not the counting, so the return code is
 * the same either way. Every printing path goes through these three;
 * tool_printf() is not called directly below.
 */

static BOOL  cnc_quiet;
static BOOL  cnc_verbose;
static UWORD cnc_errors;
static UWORD cnc_warnings;

/* tool_printf() with the QUIET gate. Same body; there is no v-form to share. */
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

/* The parser's own complaints, in the same shape as everything else here. */
static VOID cnc_report(const AmiCfgProblem *problem, APTR user)
{
    (VOID)user;

    finding(problem->file, problem->line, problem->severity);
    note(problem->text);

    if (problem->hint != NULL)
        note(problem->hint);
}

/* --------------------------------------------------------------- reading,
 *
 * The files are read a second time, by hand, to be able to name a line number:
 * a parsed AmiConfig no longer carries the positions.
 */

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

/* ------------------------------------------------------------ addressing, */

/*
 * A netmask is a run of ones followed by a run of zeroes and nothing else.
 * 0.0.0.0 and 255.255.255.255 both satisfy that; they are rejected elsewhere
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

/* TRUE when at least one interface will be given its address at run time. */
static BOOL any_dynamic(const AmiConfig *cfg)
{
    UWORD i;

    for (i = 0; i < cfg->interface_count; i++)
    {
        if (cfg->interfaces[i].iptype != AMI_IPTYPE_STATIC)
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

/* ------------------------------------------------------- the driver check,
 *
 * Catches the commonest way for a configuration to be wrong while looking
 * right: the file names a driver that is not on this machine, or the wrong
 * unit of one that is.
 *
 * tool_explain_device() asks the hardware, it probes the unit, and unit 0
 * too when another was asked for, and prints what to do about it, so the
 * finding here only says where.
 */
static VOID check_device(const char *path, const AmiIfConfig *ifc)
{
    const char *where;
    ULONG       line;

    if (ifc->device[0] == '\0')
        return;                     /* the parser has already said so */

    where = tool_device_where(ifc->device);
    line  = keyword_line(path, "DEVICE");

    /*
     * The probe is skipped while the network is running: the stack has the
     * driver open and a second OpenDevice() of a unit already in use fails,
     * which would report a working interface as broken. The network being up
     * already answers whether the card opens; ShowNetStatus reports the rest.
     */
    if (where != NULL && tool_stack_library_running())
        return;

    if (where == NULL)
    {
        finding(path, line, AMI_CFG_PROBLEM_ERROR);
        say("      this names %s, and %s cannot come up without it\n",
            (LONG)ifc->device, (LONG)ifc->name);

        if (!cnc_quiet)
            tool_explain_device(ifc->device, ifc->unit);

        return;
    }

    if (tool_device_probe(ifc->device, ifc->unit) == 0)
        return;                     /* installed, and it opens */

    /* The driver is present, so the line to look at is the UNIT one. */
    line = keyword_line(path, "UNIT");

    finding(path, line, AMI_CFG_PROBLEM_ERROR);
    say("      %s will not come up as it stands\n", (LONG)ifc->name);

    if (!cnc_quiet)
        tool_explain_device(ifc->device, ifc->unit);
}

/* ------------------------------------------------------ the address check, */

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
             "so an interface with one can never be reached from anywhere "
             "else.");
        note("Use an address on your own network, 192.168.x.y on nearly "
             "every home network, or CONFIGURE = DHCP to be given one.");
        return;
    }

    if ((ifc->address >> 24) >= 224UL)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("addresses from 224.0.0.0 upwards are reserved for multicast "
             "and for future use, and cannot be given to a machine.");
        return;
    }

    if (ifc->netmask == 0)
    {
        finding(path, 0, AMI_CFG_PROBLEM_ERROR);
        note("the interface has an address but no NETMASK, so the stack "
             "cannot tell which machines are on the same network as this "
             "one.");
        note("Add  NETMASK = 255.255.255.0, the right answer on "
             "almost every home network.");
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
        note("255.255.255.0 is what a home network almost always wants.");
        return;
    }

    if (host_bits == 0)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("this is the network's own address rather than a machine's, "
             "because every bit the netmask leaves free is zero. Nothing can "
             "talk to it.");
        note("Raise the last part of the address: .1 is usually the router, "
             "so .10 or higher is a safe choice for an Amiga.");
        return;
    }

    if (host_bits == (~ifc->netmask & 0xffffffffUL))
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_ERROR);
        note("this is the broadcast address of its own network, which "
             "reaches every machine at once and therefore cannot belong to "
             "one. Nothing will answer it.");
        note("Lower the last part of the address; .10 upwards is a safe "
             "choice.");
        return;
    }

    if ((ifc->address >> 16) == 0xa9feUL &&
        ifc->iptype == AMI_IPTYPE_STATIC)
    {
        finding(path, keyword_line(path, "ADDRESS"), AMI_CFG_PROBLEM_WARN);
        note("169.254.x.y is the range a machine picks for itself when "
             "nothing hands out addresses, so setting one by hand risks "
             "colliding with a machine that picked the same one.");
        note("Use CONFIGURE = LINKLOCAL to have one picked safely, or an "
             "address on your own network.");
    }
}

/* --------------------------------------------------------- the router check */

static VOID check_gateway(const AmiConfig *cfg)
{
    const char *path;
    ULONG       line;
    char        text[16];

    if (cfg->default_gateway == 0)
    {
        /*
         * No default route is only a problem when nothing will supply one: a
         * DHCP lease carries the router with it.
         */
        if (any_dynamic(cfg) || cfg->interface_count == 0)
            return;

        finding("DEVS:Internet/routes", 0, AMI_CFG_PROBLEM_WARN);
        note("there is no default route, so this machine can reach other "
             "machines on its own network and nothing at all beyond it.");
        note("Put  GATEWAY = <your router's address>  in "
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
    if (!any_static_address(cfg) || any_dynamic(cfg))
        return;

    if (network_holding(cfg, cfg->default_gateway) >= 0)
        return;

    ami_config_format_ip(cfg->default_gateway, text, sizeof(text));

    finding(path, line, AMI_CFG_PROBLEM_ERROR);
    say("      the router %s is not on any network this machine is\n",
        (LONG)text);
    say("      on, so nothing sent to it can arrive\n");
    note("A router has to be reachable directly. Check its address against "
         "the ADDRESS and NETMASK of your interface: all but the last part "
         "of the two addresses normally match.");
}

/* ------------------------------------------------------ the resolver check */

static VOID check_resolver(const AmiConfig *cfg)
{
    static const char *const path = "DEVS:Internet/name_resolution";
    UWORD                    i;

    if (cfg->resolver.nameserver_count == 0)
    {
        /* DHCP supplies name servers with the lease; see check_gateway(). */
        if (any_dynamic(cfg) || cfg->interface_count == 0)
            return;

        finding(path, 0, AMI_CFG_PROBLEM_WARN);
        note("no name server is configured, so names like www.example.com "
             "cannot be looked up. Numeric addresses still work.");
        note("Put  NAMESERVER <address>  in that file, or run NetSetup. On a "
             "home network the router is usually the name server too.");
        return;
    }

    if (any_dynamic(cfg) || !any_static_address(cfg))
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
        note("Either give this machine a router (GATEWAY in "
             "DEVS:Internet/routes), or use a name server on your own "
             "network, which on a home network is the router itself.");
    }
}

/* ------------------------------------------------- the whole-drawer checks */

/*
 * Two interfaces on one card, or two with one address: both parse cleanly and
 * leave one of the pair silently doing nothing.
 *
 * The finding is against the drawer rather than either file, since nothing
 * here can tell which of the two is the mistake. Both are named.
 */
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
                note("Delete whichever of the two files is the leftover, or "
                     "give one of them the UNIT of a second card.");
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
        }
    }
}

/*
 * Interface files the stack will never look at. The parsed configuration holds
 * AMI_CFG_MAX_INTERFACES and the drawer may hold more; the rest are dropped
 * silently, in alphabetical order rather than by write time.
 */
static VOID check_drawer_size(const AmiConfig *cfg)
{
    static char names[CNC_MAX_FILES][TOOL_NAME_LEN];
    ULONG       count = tool_list_dir(CNC_DIR_INTERFACES, names,
                                      (ULONG)CNC_MAX_FILES, NULL);

    if (count <= (ULONG)AMI_CFG_MAX_INTERFACES ||
        cfg->interface_count < (UWORD)AMI_CFG_MAX_INTERFACES)
    {
        return;
    }

    finding(CNC_DIR_INTERFACES, 0, AMI_CFG_PROBLEM_WARN);
    say("      the drawer holds %lu interface files and this stack has room\n",
        count);
    say("      for %ld, so the rest are ignored\n",
        (LONG)AMI_CFG_MAX_INTERFACES);
    note("The files are taken in alphabetical order, so it is the ones at "
         "the end of the alphabet that are dropped. Move the ones you do not "
         "use out of DEVS:NetInterfaces.");
}

/*
 * Roadshow keeps interface files it is not to start at boot in
 * SYS:Storage/NetInterfaces, and a machine migrated from it will have them.
 * Reported as information, not as a fault: they explain "I wrote the file and
 * nothing happened".
 */
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

/* ---------------------------------------------------------- the netdb files
 *
 * These back get{host,net,proto,serv}by*(). src/config/netdb.c drops a line it
 * cannot use without saying anything, which is right for a stack that must
 * come up regardless.
 *
 * DEVS:Internet/networks is not checked: its second column is a network
 * number, which may be written short ("10", "192.168.1"), so a checker would
 * fire on correct files.
 */

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

        /*
         * A file with the wrong format throughout would otherwise produce one
         * finding per line and bury every other finding.
         */
        if (said >= 5)
        {
            finding(spec->path, 0, AMI_CFG_PROBLEM_WARN);
            note("more lines after this one have the same problem; they are "
                 "not all listed.");
            break;
        }
    }

    Close(file);
}

/* --------------------------------------------------------------- the run, */

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

        verdict("The network has never been set up on this machine.\n");

        FreeArgs(rda);
        return RETURN_WARN;
    }

    /*
     * Everything the parser finds, bad syntax, unknown keywords, a missing
     * DEVICE line, a static interface with no address, arrives through this
     * hook with the file and line already attached.
     */
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
    check_drawer_size(&cnc_config);
    check_gateway(&cnc_config);
    check_resolver(&cnc_config);

    for (i = 0; i < (ULONG)(sizeof(cnc_netdb) / sizeof(cnc_netdb[0])); i++)
    {
        if (cnc_verbose && tool_exists(cnc_netdb[i].path))
            say("  checked %s\n", (LONG)cnc_netdb[i].path);

        check_netdb_file(&cnc_netdb[i]);
    }

    check_storage_drawer();

    /* ---- the verdict ---------------------------------------------------- */

    if (cnc_errors == 0 && cnc_warnings == 0)
    {
        verdict("\nThe network configuration has nothing wrong with it.\n");
        rc = RETURN_OK;
    }
    else
    {
        verdict("\n%lu problem(s) will stop the network working; %lu more are\n"
                "worth a look.\n",
                (ULONG)cnc_errors, (ULONG)cnc_warnings);
        rc = RETURN_WARN;
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    FreeArgs(rda);
    return (int)rc;
}
