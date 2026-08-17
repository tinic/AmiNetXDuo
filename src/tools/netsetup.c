/*
 * NetSetup, set up a network interface by answering questions.
 *
 *     NetSetup NAME,DEVICE/K,UNIT/K/N,DHCP/S,ADDRESS/K,NETMASK/K,GATEWAY/K,
 *              DNS/K,IPV6=CONFIGURE6/K,ONLINE/S,NOONLINE/S,FORCE/S,QUIET/S
 *
 * Writes the DEVS:NetInterfaces/<name> keyword file from a few questions,
 * instead of requiring the SANA-II driver details to be known up front.
 *
 * In order:
 *
 *   1. lists the network drivers installed on this machine as a numbered
 *      choice
 *   2. opens the chosen driver to check the card answers, before writing
 *      anything, so a wrong unit number is caught at the question rather than
 *      three commands later
 *   3. asks whether the address is handed out (DHCP) or set here, and checks
 *      every value as it is typed
 *   4. shows what it is about to write and asks
 *   5. writes DEVS:NetInterfaces/<name>, and DEVS:Internet/routes and
 *      name_resolution when a fixed address needs them
 *   6. offers to start the network there and then
 *
 * Nothing is written until the last question is answered: every file is
 * composed in memory first, and an existing file is renamed to .old rather
 * than overwritten, so an abort or a full disk cannot leave a half-written
 * configuration behind. Q or Ctrl-C at any question stops with the disk
 * untouched.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "NetSetup";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetSetup");

/*
 * IPV6 takes the CONFIGURE6 word, so an interface file for an IPv6-only
 * machine can be written by the command that writes every other one.  There is
 * no ADDRESS6 argument: a static IPv6 address is a two-line hand edit and
 * NetSetup's job is the machine that has nothing yet.
 */
#define TEMPLATE \
    "NAME,DEVICE/K,UNIT/K/N,DHCP/S,ADDRESS/K,NETMASK/K,GATEWAY/K,DNS/K," \
    "IPV6=CONFIGURE6/K,ONLINE/S,NOONLINE/S,FORCE/S,QUIET/S"

enum
{
    ARG_NAME = 0,
    ARG_DEVICE,
    ARG_UNIT,
    ARG_DHCP,
    ARG_ADDRESS,
    ARG_NETMASK,
    ARG_GATEWAY,
    ARG_DNS,
    ARG_IPV6,
    ARG_ONLINE,
    ARG_NOONLINE,
    ARG_FORCE,
    ARG_QUIET,
    ARG_COUNT
};

#define DIR_INTERFACES  "DEVS:NetInterfaces"
#define DIR_INTERNET    "DEVS:Internet"

#define ANSWER_LEN      80
#define FILE_LEN        512
#define PATH_LEN        (TOOL_NAME_LEN * 2)

/* What the answers so far add up to. */
typedef struct Plan
{
    char  device[TOOL_NAME_LEN];
    ULONG unit;
    char  name[TOOL_NAME_LEN];
    BOOL  dhcp;
    ULONG address;
    ULONG netmask;
    ULONG gateway;
    ULONG dns;
    BOOL  have_gateway;
    BOOL  have_dns;
    /*
     * The CONFIGURE6 word to write, as given, or empty for "say nothing and
     * let the default stand".  Not an AmiIp6Type: this command writes a file,
     * it does not configure a stack, and the parser is the one place that
     * decides what the word means.
     */
    char  configure6[TOOL_NAME_LEN];
    /* IPV6 was given AND no IPv4 addressing was: an IPv6-only interface. */
    BOOL  ipv6_only;
} Plan;

/* ------------------------------------------------------------------ input, */

/*
 * Abort is a state rather than a return code, because every question can hit
 * it: Ctrl-C, "Q", or end of input. End of input is what NetSetup driven from
 * a script that ran out of answers looks like. Nothing has been written when
 * it stops.
 */
static BOOL setup_aborted;

static VOID abort_setup(const char *why)
{
    setup_aborted = TRUE;
    tool_printf("\n%s  Nothing was changed.\n", (LONG)why);
}

/*
 * One line from the user, with the prompt flushed first. Returns NULL once the
 * setup has been aborted. Callers test setup_aborted rather than threading an
 * error code through every question.
 */
static char *ask(const char *prompt, const char *suggestion, char *buf)
{
    LONG len;

    if (setup_aborted)
        return NULL;

    if (suggestion != NULL && *suggestion != '\0')
        tool_printf("%s [%s]: ", (LONG)prompt, (LONG)suggestion);
    else
        tool_printf("%s: ", (LONG)prompt);

    Flush(Output());

    if (FGets(Input(), (STRPTR)buf, (ULONG)ANSWER_LEN) == NULL)
    {
        /* End of input, or the console went away. */
        abort_setup("No answer.");
        return NULL;
    }

    if (tool_break())
    {
        abort_setup("Stopped.");
        return NULL;
    }

    /* FGets keeps the newline, so trim that and any surrounding blanks. */
    for (len = 0; buf[len] != '\0'; len++)
        ;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                       buf[len - 1] == ' '  || buf[len - 1] == '\t'))
        buf[--len] = '\0';

    {
        char *start = buf;

        while (*start == ' ' || *start == '\t')
            start++;

        if (start != buf)
        {
            ULONG i = 0;

            while (start[i] != '\0')
            {
                buf[i] = start[i];
                i++;
            }
            buf[i] = '\0';
        }
    }

    /*
     * A console echoes what was typed and a file does not, so a scripted run
     * would print its questions run together with no sign of the answers. Echo
     * before acting on the answer, so a transcript shows the Q that stopped it.
     */
    if (!IsInteractive(Input()))
        tool_printf("%s\n", (LONG)buf);

    if (tool_stricmp(buf, "q") == 0 || tool_stricmp(buf, "quit") == 0)
    {
        abort_setup("Stopped.");
        return NULL;
    }

    if (buf[0] == '\0' && suggestion != NULL)
        tool_copy_string(buf, ANSWER_LEN, suggestion);

    return buf;
}

/* A yes/no question. `preset` is what Return means. */
static BOOL ask_yes(const char *prompt, BOOL preset)
{
    char answer[ANSWER_LEN];

    for (;;)
    {
        if (ask(prompt, preset ? "Y" : "N", answer) == NULL)
            return FALSE;

        if (answer[0] == 'y' || answer[0] == 'Y')
            return TRUE;
        if (answer[0] == 'n' || answer[0] == 'N')
            return FALSE;

        tool_printf("  Answer Y or N.\n");
    }
}

/* An address, re-asked until it parses. Empty is allowed when `optional`. */
static BOOL ask_address(const char *prompt, const char *suggestion,
                        ULONG *out, BOOL optional)
{
    char answer[ANSWER_LEN];

    for (;;)
    {
        if (ask(prompt, suggestion, answer) == NULL)
            return FALSE;

        if (answer[0] == '\0')
        {
            if (optional)
                return FALSE;       /* no value, but not an abort */

            tool_printf("  An address is needed here.\n");
            continue;
        }

        if (tool_stricmp(answer, "none") == 0)
            return FALSE;

        if (ami_config_parse_ip(answer, out))
            return TRUE;

        tool_printf("  \"%s\" is not an address. An address is four numbers\n",
                    (LONG)answer);
        tool_printf("  from 0 to 255 with dots between them, like 192.168.1.10.\n");
    }
}

/* ------------------------------------------------------------ validation, */

/* A netmask is a run of ones then a run of zeroes, so 255.255.0.255 is not one. */
static BOOL netmask_is_sane(ULONG mask)
{
    ULONG inverted = ~mask;

    if (mask == 0)
        return FALSE;

    /* Contiguous ones <=> ~mask + 1 is a power of two (or zero). */
    return (BOOL)((inverted & (inverted + 1UL)) == 0UL);
}

/* 10.x -> /8, 172.16-31 -> /16, 192.168 -> /24, otherwise by old class rules. */
static ULONG default_netmask(ULONG address)
{
    ULONG a = (address >> 24) & 0xFFUL;
    ULONG b = (address >> 16) & 0xFFUL;

    if (a == 10UL)
        return 0xFF000000UL;
    if (a == 172UL && b >= 16UL && b <= 31UL)
        return 0xFFFF0000UL;
    if (a == 192UL && b == 168UL)
        return 0xFFFFFF00UL;

    if (a < 128UL)
        return 0xFF000000UL;
    if (a < 192UL)
        return 0xFFFF0000UL;

    return 0xFFFFFF00UL;
}

static BOOL name_is_sane(const char *name)
{
    ULONG len = 0;

    while (name[len] != '\0')
    {
        char c = name[len];

        if (c == '/' || c == ':' || c == ' ' || c == '*' || c == '?')
            return FALSE;
        len++;
    }

    return (BOOL)(len > 0 && len <= 15);
}

/* ---------------------------------------------------------------- writing, */

/*
 * Files are built here in full and written in one go at the end, so an abort,
 * an out-of-disk or a Ctrl-C cannot leave DEVS:NetInterfaces holding half a
 * file for the stack to read and complain about.
 */
typedef struct Blob
{
    char  text[FILE_LEN];
    ULONG len;
} Blob;

static VOID blob_reset(Blob *b)
{
    b->len     = 0;
    b->text[0] = '\0';
}

static VOID blob_add(Blob *b, const char *s)
{
    while (s != NULL && *s != '\0' && b->len + 1 < (ULONG)FILE_LEN)
        b->text[b->len++] = *s++;

    b->text[b->len] = '\0';
}

static VOID blob_add_ip(Blob *b, ULONG addr)
{
    char text[16];

    ami_config_format_ip(addr, text, sizeof(text));
    blob_add(b, text);
}

static VOID blob_add_ulong(Blob *b, ULONG value)
{
    char  text[12];
    ULONG i = sizeof(text) - 1;

    text[i] = '\0';
    do
    {
        text[--i] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }
    while (value != 0UL && i > 0);

    blob_add(b, &text[i]);
}

/* Make a drawer if it is not there. FALSE only when it could not be made. */
static BOOL ensure_dir(const char *path)
{
    BPTR lock;

    if (tool_exists(path))
        return TRUE;

    lock = CreateDir((CONST_STRPTR)path);
    if (lock == (BPTR)0)
    {
        tool_error("cannot create %s", (LONG)path);
        tool_fault(IoErr());
        return FALSE;
    }

    UnLock(lock);
    tool_printf("Created %s\n", (LONG)path);

    return TRUE;
}

/*
 * Write one file, keeping any existing one as <path>.old. Returns FALSE
 * having said why. The caller then puts back whatever it renamed.
 */
static BOOL write_file(const char *path, const Blob *blob, BOOL *kept_old)
{
    char keep[PATH_LEN + 8];
    BPTR fh;
    LONG written;

    *kept_old = FALSE;

    tool_copy_string(keep, sizeof(keep), path);
    {
        ULONG n = 0;

        while (keep[n] != '\0')
            n++;
        tool_copy_string(keep + n, sizeof(keep) - n, ".old");
    }

    if (tool_exists(path))
    {
        if (tool_exists(keep))
            (VOID)DeleteFile((CONST_STRPTR)keep);

        if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)keep))
        {
            tool_error("cannot rename the old %s", (LONG)path);
            tool_fault(IoErr());
            return FALSE;
        }

        *kept_old = TRUE;
    }

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        tool_error("cannot write %s", (LONG)path);
        tool_fault(IoErr());
        return FALSE;
    }

    written = Write(fh, (APTR)blob->text, (LONG)blob->len);
    Close(fh);

    if (written != (LONG)blob->len)
    {
        tool_error("only part of %s was written. A full disk is the usual "
                   "cause", (LONG)path);
        (VOID)DeleteFile((CONST_STRPTR)path);
        return FALSE;
    }

    return TRUE;
}

static VOID restore_file(const char *path, BOOL kept_old)
{
    char keep[PATH_LEN + 8];
    ULONG n = 0;

    if (!kept_old)
    {
        (VOID)DeleteFile((CONST_STRPTR)path);
        return;
    }

    tool_copy_string(keep, sizeof(keep), path);
    while (keep[n] != '\0')
        n++;
    tool_copy_string(keep + n, sizeof(keep) - n, ".old");

    (VOID)DeleteFile((CONST_STRPTR)path);
    (VOID)Rename((CONST_STRPTR)keep, (CONST_STRPTR)path);
}

/* --------------------------------------------------------------- the plan, */

static VOID build_interface_file(const Plan *plan, Blob *out)
{
    blob_reset(out);

    blob_add(out, "# Network interface, written by NetSetup.\n");
    blob_add(out, "# One keyword per line. # starts a comment. Safe to edit.\n");
    blob_add(out, "\n");

    blob_add(out, "DEVICE    = ");
    blob_add(out, plan->device);
    blob_add(out, "\n");

    blob_add(out, "UNIT      = ");
    blob_add_ulong(out, plan->unit);
    blob_add(out, "\n");

    if (plan->dhcp)
    {
        blob_add(out, "CONFIGURE = DHCP\n");
    }
    else if (plan->ipv6_only)
    {
        /* Written out rather than left to the default: the whole point of
           this file is that IPv4 is off on purpose. */
        blob_add(out, "CONFIGURE = NONE\n");
    }
    else
    {
        blob_add(out, "CONFIGURE = STATIC\n");
        blob_add(out, "ADDRESS   = ");
        blob_add_ip(out, plan->address);
        blob_add(out, "\n");
        blob_add(out, "NETMASK   = ");
        blob_add_ip(out, plan->netmask);
        blob_add(out, "\n");
    }

    if (plan->configure6[0] != '\0')
    {
        blob_add(out, "CONFIGURE6 = ");
        blob_add(out, plan->configure6);
        blob_add(out, "\n");
    }

    blob_add(out, "STATE     = UP\n");
}

static VOID build_routes_file(const Plan *plan, Blob *out)
{
    blob_reset(out);

    blob_add(out, "# Routing, written by NetSetup.\n");
    blob_add(out, "# DEFAULT is the address of the router that reaches "
                  "everything else.\n");
    blob_add(out, "\n");
    blob_add(out, "DEFAULT = ");
    blob_add_ip(out, plan->gateway);
    blob_add(out, "\n");
}

static VOID build_resolver_file(const Plan *plan, Blob *out)
{
    blob_reset(out);

    blob_add(out, "# Name lookup, written by NetSetup.\n");
    blob_add(out, "# NAMESERVER is the machine that turns names into "
                  "addresses.\n");
    blob_add(out, "\n");
    blob_add(out, "NAMESERVER ");
    blob_add_ip(out, plan->dns);
    blob_add(out, "\n");
}

static VOID show_plan(const Plan *plan, const char *ifpath)
{
    char text[16];

    tool_printf("\nThis is what will be written:\n\n");
    tool_printf("  %s\n", (LONG)ifpath);
    tool_printf("      DEVICE    = %s\n", (LONG)plan->device);
    tool_printf("      UNIT      = %lu\n", plan->unit);

    if (plan->dhcp)
    {
        tool_printf("      CONFIGURE = DHCP\n");
    }
    else if (plan->ipv6_only)
    {
        tool_printf("      CONFIGURE = NONE\n");
    }
    else
    {
        tool_printf("      CONFIGURE = STATIC\n");
        ami_config_format_ip(plan->address, text, sizeof(text));
        tool_printf("      ADDRESS   = %s\n", (LONG)text);
        ami_config_format_ip(plan->netmask, text, sizeof(text));
        tool_printf("      NETMASK   = %s\n", (LONG)text);
    }

    if (plan->configure6[0] != '\0')
        tool_printf("      CONFIGURE6 = %s\n", (LONG)plan->configure6);

    if (plan->have_gateway)
    {
        ami_config_format_ip(plan->gateway, text, sizeof(text));
        tool_printf("\n  %s/routes\n", (LONG)DIR_INTERNET);
        tool_printf("      DEFAULT = %s\n", (LONG)text);
    }

    if (plan->have_dns)
    {
        ami_config_format_ip(plan->dns, text, sizeof(text));
        tool_printf("\n  %s/name_resolution\n", (LONG)DIR_INTERNET);
        tool_printf("      NAMESERVER %s\n", (LONG)text);
    }
}

/* ------------------------------------------------------------- questions, */

/* The card. Numbered list of what is installed, or type a name. */
static BOOL ask_device(Plan *plan)
{
    char  answer[ANSWER_LEN];
    ULONG found = tool_scan_devices();
    ULONG i;

    tool_printf("\nWhich network card does this Amiga have?\n");

    if (found > 0)
    {
        for (i = 0; i < found; i++)
        {
            const ToolDevice *dev = tool_scan_device(i);

            tool_printf("   %lu  %-22s (%s)\n", i + 1UL, (LONG)dev->name,
                        (LONG)dev->where);
        }
        tool_printf("   If the card is not listed, type the name of its driver.\n");
    }
    else
    {
        tool_printf("   No network card driver was found on this machine.\n");
        tool_printf("   Drivers belong in DEVS:Networks/ and come with the\n");
        tool_printf("   card. If the driver is installed somewhere else,\n");
        tool_printf("   type its name.\n");
    }

    for (;;)
    {
        const char *suggestion = (found > 0) ? "1" : "";

        if (ask("Card", suggestion, answer) == NULL)
            return FALSE;

        if (answer[0] == '\0')
        {
            tool_printf("  Type a number from the list, or a driver name.\n");
            continue;
        }

        /* A number picks from the list. */
        if (answer[0] >= '1' && answer[0] <= '9' && answer[1] == '\0')
        {
            ULONG pick = (ULONG)(answer[0] - '1');

            if (pick < found)
            {
                tool_copy_string(plan->device, sizeof(plan->device),
                                 tool_scan_device(pick)->name);
                return TRUE;
            }

            tool_printf("  There is no %s in the list.\n", (LONG)answer);
            continue;
        }

        /* A name. "ariadne" is almost certainly "ariadne.device". */
        {
            ULONG len = 0;

            while (answer[len] != '\0')
                len++;

            if (len < 7 || tool_stricmp(answer + len - 7, ".device") != 0)
            {
                tool_copy_string(plan->device, sizeof(plan->device), answer);
                tool_copy_string(plan->device + len,
                                 sizeof(plan->device) - len, ".device");
                tool_printf("  The driver is %s.\n", (LONG)plan->device);
            }
            else
            {
                tool_copy_string(plan->device, sizeof(plan->device), answer);
            }
        }

        return TRUE;
    }
}

/*
 * Ask the card whether it is really there, before anything is written. A wrong
 * unit, or a driver for a card that is not installed, is otherwise found out
 * much later by a command that can only say "did not open".
 */
static BOOL check_device(Plan *plan, BOOL quiet)
{
    LONG probe;

    if (setup_aborted)
        return FALSE;

    probe = tool_device_probe(plan->device, plan->unit, NULL);

    if (probe == 0)
    {
        if (!quiet)
            tool_printf("  %s unit %lu answers.\n",
                        (LONG)plan->device, plan->unit);
        return TRUE;
    }

    tool_printf("\n  %s unit %lu did not answer.\n",
                (LONG)plan->device, plan->unit);

    if (tool_device_where(plan->device) == NULL)
    {
        tool_printf("  That driver is not installed. It is not in\n");
        tool_printf("  DEVS:Networks/ or anywhere else this command looked.\n");
    }
    else if (plan->unit != 0 && tool_device_probe(plan->device, 0, NULL) == 0)
    {
        tool_printf("  Unit 0 of the same driver does answer. Almost every\n");
        tool_printf("  card is unit 0.\n");

        if (ask_yes("  Use unit 0 instead", TRUE))
        {
            plan->unit = 0;
            return TRUE;
        }

        return !setup_aborted;
    }
    else
    {
        tool_printf("  The driver is installed but the card does not answer:\n");
        tool_printf("  the card is not installed, or not seated properly.\n");
    }

    if (setup_aborted)
        return FALSE;

    return ask_yes("  Write the configuration for it anyway", FALSE);
}

static BOOL ask_unit(Plan *plan)
{
    char  answer[ANSWER_LEN];
    ULONG value;

    for (;;)
    {
        if (ask("Unit number", "0", answer) == NULL)
            return FALSE;

        value = 0;
        {
            ULONG i = 0;
            BOOL  ok = (answer[0] != '\0');

            while (answer[i] != '\0')
            {
                if (answer[i] < '0' || answer[i] > '9')
                {
                    ok = FALSE;
                    break;
                }
                value = value * 10UL + (ULONG)(answer[i] - '0');
                i++;
            }

            if (ok)
            {
                plan->unit = value;
                return TRUE;
            }
        }

        tool_printf("  The unit is a plain number, and 0 on almost every card.\n");
    }
}

static BOOL ask_name(Plan *plan)
{
    char answer[ANSWER_LEN];

    tool_printf("\nThe interface needs a name. It is only a label: it becomes\n");
    tool_printf("the name of the file in %s, and the name to type\n", (LONG)DIR_INTERFACES);
    tool_printf("after Online, Offline and ShowNetStatus.\n");

    for (;;)
    {
        if (ask("Name", "eth0", answer) == NULL)
            return FALSE;

        if (name_is_sane(answer))
        {
            tool_copy_string(plan->name, sizeof(plan->name), answer);
            return TRUE;
        }

        tool_printf("  A name is 1 to 15 characters and cannot contain a\n");
        tool_printf("  slash, a colon or a space.\n");
    }
}

static BOOL ask_addressing(Plan *plan)
{
    char answer[ANSWER_LEN];

    tool_printf("\nHow does this Amiga get its address?\n");
    tool_printf("   1  Automatically, from the network (DHCP)\n");
    tool_printf("      Almost every home network works this way: the broadband\n");
    tool_printf("      router hands out addresses.\n");
    tool_printf("   2  A fixed address, typed in here\n");

    for (;;)
    {
        if (ask("Address", "1", answer) == NULL)
            return FALSE;

        if (answer[0] == '1' && answer[1] == '\0')
        {
            plan->dhcp = TRUE;
            return TRUE;
        }
        if (answer[0] == '2' && answer[1] == '\0')
        {
            plan->dhcp = FALSE;
            return TRUE;
        }

        tool_printf("  Type 1 or 2.\n");
    }
}

static BOOL ask_static_details(Plan *plan)
{
    char suggestion[16];

    tool_printf("\nA fixed address must be one that nothing else on the\n");
    tool_printf("network uses, and it must be on the same network as\n");
    tool_printf("everything else. If the router is 192.168.1.1, then\n");
    tool_printf("192.168.1.50 is an address of the right kind.\n");

    if (!ask_address("Address for this Amiga", "", &plan->address, FALSE))
        return FALSE;

    ami_config_format_ip(default_netmask(plan->address), suggestion,
                         sizeof(suggestion));

    for (;;)
    {
        if (!ask_address("Netmask", suggestion, &plan->netmask, FALSE))
            return FALSE;

        if (netmask_is_sane(plan->netmask))
            break;

        tool_printf("  That is not a usable netmask: it must be a run of\n");
        tool_printf("  255s followed by 0s, like 255.255.255.0.\n");
    }

    tool_printf("\nThe router (gateway) is what reaches everything outside\n");
    tool_printf("this network. If there is no router, leave this empty.\n");

    plan->have_gateway = ask_address("Router address", "", &plan->gateway, TRUE);
    if (setup_aborted)
        return FALSE;

    if (plan->have_gateway &&
        ((plan->gateway & plan->netmask) != (plan->address & plan->netmask)))
    {
        char a[16];
        char g[16];

        ami_config_format_ip(plan->address, a, sizeof(a));
        ami_config_format_ip(plan->gateway, g, sizeof(g));

        tool_printf("\n  %s is not on the same network as %s, so this\n",
                    (LONG)g, (LONG)a);
        tool_printf("  machine cannot reach it. That is usually a typing\n");
        tool_printf("  mistake.\n");

        if (!ask_yes("  Keep it anyway", FALSE))
        {
            if (setup_aborted)
                return FALSE;
            plan->have_gateway = FALSE;
        }
    }

    tool_printf("\nThe name server turns names like www.example.com into\n");
    tool_printf("addresses. On a home network it is usually the router.\n");

    if (plan->have_gateway)
        ami_config_format_ip(plan->gateway, suggestion, sizeof(suggestion));
    else
        suggestion[0] = '\0';

    plan->have_dns = ask_address("Name server address", suggestion,
                                 &plan->dns, TRUE);

    return !setup_aborted;
}

/* --------------------------------------------------------------- bring up, */

static VOID bring_up(const Plan *plan)
{
    char line[PATH_LEN + 32];
    LONG rc;

    tool_copy_string(line, sizeof(line), "AddNetInterface ");
    {
        ULONG n = 0;

        while (line[n] != '\0')
            n++;
        tool_copy_string(line + n, sizeof(line) - n, plan->name);
    }

    tool_printf("\n%s\n", (LONG)line);

    /*
     * Run the real command rather than starting the stack here, so only one
     * place knows how to start a network and the user sees whatever
     * AddNetInterface says about a failure.
     */
    rc = SystemTagList((CONST_STRPTR)line, NULL);

    if (rc == -1)
    {
        /*
         * Not on the command path: the commands need not have been copied to
         * C: yet, as just after an installer unpacked them elsewhere. Look
         * next to this binary before giving up.
         */
        char alt[PATH_LEN + 40];

        tool_copy_string(alt, sizeof(alt), "SYS:");
        {
            ULONG n = 0;

            while (alt[n] != '\0')
                n++;
            tool_copy_string(alt + n, sizeof(alt) - n, line);
        }

        rc = SystemTagList((CONST_STRPTR)alt, NULL);
    }

    if (rc == -1)
    {
        tool_error("AddNetInterface did not run");
        tool_printf("      AddNetInterface %s\n", (LONG)plan->name);
        return;
    }

    if (rc == 0)
    {
        tool_printf("\nThe network is up.\n");
        tool_printf("Add this line to S:User-Startup to start the network at\n");
        tool_printf("every boot:\n");
        tool_printf("      C:AddNetInterface %s QUIET\n", (LONG)plan->name);
    }
    else
    {
        tool_printf("\nThe configuration was written, but the network did not\n");
        tool_printf("come up. AddNetInterface said why, above.\n");
    }
}

/* ------------------------------------------------------------------- main, */

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    Plan           plan;
    Blob          *blob;
    char           ifpath[PATH_LEN];
    BOOL           quiet;
    BOOL           interactive;
    BOOL           kept_if    = FALSE;
    BOOL           kept_route = FALSE;
    BOOL           kept_res   = FALSE;
    int            i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[name] [DEVICE=<driver>] [UNIT=<n>] [DHCP] "
                   "[ADDRESS=..] [NETMASK=..]",
                   "Set up a network interface. Run it with no arguments to "
                   "be asked.");
        return RETURN_ERROR;
    }

    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    /* ---- what the arguments gave, before asking for the rest ------------ */

    plan.device[0]   = '\0';
    plan.name[0]     = '\0';
    plan.unit        = 0;
    plan.dhcp        = (args[ARG_DHCP] != 0) ? TRUE : FALSE;
    plan.address     = 0;
    plan.netmask     = 0;
    plan.gateway     = 0;
    plan.dns         = 0;
    plan.have_gateway = FALSE;
    plan.have_dns     = FALSE;

    if (args[ARG_DEVICE] != 0)
        tool_copy_string(plan.device, sizeof(plan.device),
                         (const char *)args[ARG_DEVICE]);
    if (args[ARG_NAME] != 0)
        tool_copy_string(plan.name, sizeof(plan.name),
                         tool_basename((const char *)args[ARG_NAME]));
    if (args[ARG_UNIT] != 0)
        plan.unit = *(ULONG *)args[ARG_UNIT];

    if (args[ARG_ADDRESS] != 0 &&
        !ami_config_parse_ip((const char *)args[ARG_ADDRESS], &plan.address))
    {
        tool_error("ADDRESS=%s is not an address",
                   (LONG)args[ARG_ADDRESS]);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (args[ARG_NETMASK] != 0 &&
        !ami_config_parse_ip((const char *)args[ARG_NETMASK], &plan.netmask))
    {
        tool_error("NETMASK=%s is not an address",
                   (LONG)args[ARG_NETMASK]);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (args[ARG_GATEWAY] != 0)
    {
        if (!ami_config_parse_ip((const char *)args[ARG_GATEWAY], &plan.gateway))
        {
            tool_error("GATEWAY=%s is not an address",
                       (LONG)args[ARG_GATEWAY]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        plan.have_gateway = TRUE;
    }
    if (args[ARG_DNS] != 0)
    {
        if (!ami_config_parse_ip((const char *)args[ARG_DNS], &plan.dns))
        {
            tool_error("DNS=%s is not an address", (LONG)args[ARG_DNS]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        plan.have_dns = TRUE;
    }
    if (args[ARG_IPV6] != 0)
    {
        static const char *const modes[] =
        {
            "auto", "dhcp", "static", "linklocal", "off", NULL
        };
        UWORD m;

        for (m = 0; modes[m] != NULL; m++)
        {
            if (tool_stricmp((const char *)args[ARG_IPV6], modes[m]) == 0)
                break;
        }

        if (modes[m] == NULL)
        {
            tool_error("IPV6=%s is not a mode: AUTO, DHCP, STATIC, LINKLOCAL "
                       "or OFF", (LONG)args[ARG_IPV6]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        tool_copy_string(plan.configure6, sizeof(plan.configure6),
                         (const char *)args[ARG_IPV6]);

        /*
         * IPV6 alone, with no DHCP and no ADDRESS, is the IPv6-only machine.
         * OFF is not: an operator switching IPv6 off has said nothing about
         * how IPv4 is configured, and an interface with neither is the file
         * that has no address at all.
         */
        plan.ipv6_only = (BOOL)(!plan.dhcp && plan.address == 0 &&
                                tool_stricmp((const char *)args[ARG_IPV6],
                                             "off") != 0);
    }

    if (plan.address != 0 && plan.netmask == 0)
        plan.netmask = default_netmask(plan.address);

    /*
     * A driver plus a way of getting an address is a full specification, so
     * nothing needs to be asked. An installer script drives NetSetup this way.
     */
    interactive = (BOOL)!(plan.device[0] != '\0' &&
                          (plan.dhcp || plan.address != 0 || plan.ipv6_only));

    if (!quiet)
    {
        tool_printf("\nNetSetup: set up a network interface\n");
        if (interactive)
        {
            tool_printf("\nPress Return to accept the [suggested] answer.\n");
            tool_printf("Type Q at any question to stop. Nothing is written\n");
            tool_printf("until every question is answered.\n");
        }
    }

    /* ---- the questions -------------------------------------------------- */

    if (plan.device[0] == '\0' && !ask_device(&plan))
    {
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (interactive && args[ARG_UNIT] == 0 && !ask_unit(&plan))
    {
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (interactive && !check_device(&plan, quiet))
    {
        if (!setup_aborted)
            tool_printf("\nStopped. Nothing was changed.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (plan.name[0] == '\0')
    {
        if (interactive)
        {
            if (!ask_name(&plan))
            {
                FreeArgs(rda);
                return RETURN_WARN;
            }
        }
        else
        {
            tool_copy_string(plan.name, sizeof(plan.name), "eth0");
        }
    }
    else if (!name_is_sane(plan.name))
    {
        tool_error("\"%s\" is not a usable interface name", (LONG)plan.name);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (interactive && !plan.dhcp && plan.address == 0 && !plan.ipv6_only)
    {
        if (!ask_addressing(&plan))
        {
            FreeArgs(rda);
            return RETURN_WARN;
        }

        if (!plan.dhcp && !ask_static_details(&plan))
        {
            FreeArgs(rda);
            return RETURN_WARN;
        }
    }

    if (!plan.dhcp && plan.address == 0 && !plan.ipv6_only)
    {
        tool_error("no address: give ADDRESS=<address>, DHCP, or "
                   "IPV6=<mode> for an IPv6-only interface");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!plan.dhcp && !plan.ipv6_only && plan.netmask == 0)
        plan.netmask = default_netmask(plan.address);

    /* ---- confirm --------------------------------------------------------- */

    tool_join_path(ifpath, sizeof(ifpath), DIR_INTERFACES, plan.name);

    if (interactive)
    {
        show_plan(&plan, ifpath);

        if (tool_exists(ifpath) && args[ARG_FORCE] == 0)
        {
            tool_printf("\n  %s already exists and will be replaced.\n",
                        (LONG)ifpath);
        }

        if (!ask_yes("\nWrite it", TRUE))
        {
            if (!setup_aborted)
                tool_printf("\nNothing was changed.\n");
            FreeArgs(rda);
            return RETURN_WARN;
        }
    }
    else if (tool_exists(ifpath) && args[ARG_FORCE] == 0)
    {
        /*
         * Nobody is being asked anything, so an existing interface file is not
         * replaced on a guess: an installer re-run must not quietly discard a
         * working configuration.
         */
        tool_error("%s already exists", (LONG)ifpath);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* ---- write ----------------------------------------------------------- */

    blob = (Blob *)ami_alloc((ULONG)sizeof(Blob));
    if (blob == NULL)
    {
        tool_error("out of memory");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!ensure_dir(DIR_INTERFACES))
    {
        ami_free(blob);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    build_interface_file(&plan, blob);
    if (!write_file(ifpath, blob, &kept_if))
    {
        ami_free(blob);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    tool_printf("Wrote %s\n", (LONG)ifpath);

    if (plan.have_gateway || plan.have_dns)
    {
        char path[PATH_LEN];

        if (!ensure_dir(DIR_INTERNET))
        {
            restore_file(ifpath, kept_if);
            ami_free(blob);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        if (plan.have_gateway)
        {
            tool_join_path(path, sizeof(path), DIR_INTERNET, "routes");
            build_routes_file(&plan, blob);
            if (!write_file(path, blob, &kept_route))
            {
                restore_file(ifpath, kept_if);
                ami_free(blob);
                FreeArgs(rda);
                return RETURN_FAIL;
            }
            tool_printf("Wrote %s\n", (LONG)path);
        }

        if (plan.have_dns)
        {
            char rpath[PATH_LEN];

            tool_join_path(rpath, sizeof(rpath), DIR_INTERNET,
                           "name_resolution");
            build_resolver_file(&plan, blob);
            if (!write_file(rpath, blob, &kept_res))
            {
                tool_join_path(path, sizeof(path), DIR_INTERNET, "routes");
                if (plan.have_gateway)
                    restore_file(path, kept_route);
                restore_file(ifpath, kept_if);
                ami_free(blob);
                FreeArgs(rda);
                return RETURN_FAIL;
            }
            tool_printf("Wrote %s\n", (LONG)rpath);
        }
    }

    /* All the files are in place, so the interface's .old rollback backup is
       no longer needed and is deleted.  Left in DEVS:NetInterfaces it would be
       loaded as a second interface, because the drawer is read whole
       (src/config/config_file.c). */
    if (kept_if)
    {
        char  keep[PATH_LEN + 8];
        ULONG n = 0;

        tool_copy_string(keep, sizeof(keep), ifpath);
        while (keep[n] != '\0')
            n++;
        tool_copy_string(keep + n, sizeof(keep) - n, ".old");
        (VOID)DeleteFile((CONST_STRPTR)keep);
    }

    ami_free(blob);

    /* ---- and try it ------------------------------------------------------ */

    if (args[ARG_NOONLINE] == 0)
    {
        BOOL now = (args[ARG_ONLINE] != 0) ? TRUE : FALSE;

        if (!now && interactive)
        {
            tool_printf("\nThe network can be started now, which is the quickest\n");
            tool_printf("way to find out whether this worked.\n");
            now = ask_yes("Start it now", TRUE);
        }

        if (now)
            bring_up(&plan);
        else if (!quiet)
            tool_printf("\nStart the network with:   AddNetInterface %s\n",
                        (LONG)plan.name);
    }

    FreeArgs(rda);
    return RETURN_OK;
}
