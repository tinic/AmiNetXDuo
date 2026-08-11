/*
 * hostname, what this machine calls itself, and where that came from.
 *
 *     C:hostname [<name>] [QUIET]
 *
 * WHY THE SOURCE IS PRINTED AND NOT JUST THE NAME
 *
 * Four places can name this machine, and they are RANKED
 * (AmiHostnameSource, aminetxduo/config.h), weakest first:
 *
 *     1  an interface file's ID=       DEVS:NetInterfaces/<name>
 *     2  ENV:HOSTNAME
 *     3  DHCP option 12                whatever the server said
 *     4  HOSTNAME= in DEVS:Internet/name_resolution
 *
 * A stronger source replaces a weaker one and a weaker one is refused, which is
 * why a machine can be renamed in the obvious place and go on answering to its
 * old name with nothing saying so. A leftover ENV:HOSTNAME did exactly that
 * here once. So a name on its own is not an answer to "what is this machine
 * called": the source is half of it, and this command prints both.
 *
 * SETTING WRITES ENV:HOSTNAME, AND SAYS SO
 *
 * `hostname beast` writes ENV:HOSTNAME and ENVARC:HOSTNAME -- ENV: so it is in
 * force now, ENVARC: so it survives the next boot -- and offers the name to the
 * running stack at the rank of ENV:HOSTNAME, which is the file it just wrote.
 *
 * It does NOT write the name_resolution file and it does not set the name at a
 * rank higher than the file it wrote. A command that quietly named the machine
 * at rank 4 with nothing in DEVS:Internet saying so would produce a machine
 * that renames itself back at the next boot with no record of why.
 *
 * So a machine whose name comes from DHCP or from name_resolution is NOT
 * renamed by this, and is told: the file is still written, because it decides
 * the name if the stronger source stops supplying one, and the running name is
 * reported as what it still is.
 *
 * WHAT IT CHANGES IN THE RUNNING STACK, AND WHAT IT DOES NOT
 *
 * gethostname() answers the new name at once, and so does the DHCP client's
 * option 12 on its next request: NX_DHCP was handed the configuration's own
 * buffer rather than a copy.
 *
 * The mDNS responder is NOT renamed. It claimed <name>.local at start-up, and
 * claiming a new one means withdrawing the old record and probing for the new
 * one on every interface. Nothing here does that, so a machine that answers
 * .local goes on answering to the label it started with until the stack is
 * restarted, and this command says so rather than leaving it to be discovered.
 *
 * WITH THE NETWORK DOWN it still works, and is the normal way to name a machine
 * before its first boot with an interface: the files are written, nothing is
 * running to be told, and the next start-up reads them.
 *
 * THE NAME IS CHECKED, by the same rule the configuration uses
 * (ami_config_hostname_valid): RFC 952 as RFC 1123 2.1 relaxes it, so letters,
 * digits and hyphens, no leading or trailing hyphen, dot-separated labels of at
 * most 63 characters. A name that would be refused by the config parser is
 * refused here rather than written to a file that then fails to load.
 *
 * ROADSHOW HAS NO COMMAND OF THIS NAME. Its 26 C: commands were read out of the
 * shipped 1.13 archive and there is none, and RoadshowControl's settable
 * variables are all protocol tuning; the local name there comes from
 * DEVS:Internet/hosts and the only command-line way to assert one is
 * ConfigureNetInterface's ID= keyword, which tells the DHCP server what to call
 * you and does not name the machine. So the surface here is the Unix one -- the
 * name as the argument, no argument to report -- because that is what somebody
 * typing `hostname` expects, and there is nothing to be compatible with.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <dos/var.h>

const char *const tool_name = "hostname";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("hostname");

#define TEMPLATE    "NAME,QUIET/S"

enum
{
    ARG_NAME = 0,
    ARG_QUIET,
    ARG_COUNT
};

/*
 * The errno numbers this command names, as bsdsocket.library reports them
 * (src/bsdsocket/bsdsocket_internal.h). Spelled out rather than taken from
 * <errno.h> for the reason addnetroute.c gives: the library uses the BSD
 * numbering and newlib does not agree with it everywhere.
 */
#define HN_EPERM    1

static BOOL hn_quiet;

/* The name in force, and where it came from. TRUE when a stack answered. */
static BOOL running_name(char *out, ULONG outlen, UWORD *source)
{
    NetStatusSystem sys;

    *source = (UWORD)AMI_HOSTNAME_NONE;

    if (!tool_stack_query(NULL, out, outlen) || out[0] == '\0')
        return FALSE;

    /* nss_HostSource is zero on a library too old to have it, which is
       AMI_HOSTNAME_NONE and reads as "not stated" below, not as a guess. */
    if (tool_netstatus_system(&sys))
        *source = (UWORD)sys.nss_HostSource;

    return TRUE;
}

/*
 * The name the FILES say, for a machine with no stack running. The same chain
 * ami_config_load() runs at start-up, minus DHCP, which has no lease to have
 * come from.
 */
static BOOL configured_name(char *out, ULONG outlen, UWORD *source)
{
    static AmiConfig cfg;               /* far too big for a 4 KB stack */

    *source = (UWORD)AMI_HOSTNAME_NONE;
    out[0]  = '\0';

    if (ami_config_load(&cfg) != AMI_CFG_OK || cfg.hostname[0] == '\0')
        return FALSE;

    tool_copy_string(out, outlen, cfg.hostname);
    *source = cfg.hostname_source;

    return TRUE;
}

/* "ENV:HOSTNAME", or the phrase for a machine nothing named. */
static VOID say_source(UWORD source)
{
    const char *from = ami_config_hostname_source_text(source);

    if (hn_quiet)
        return;

    /*
     * No source means nothing configured one, so the stack named the machine
     * after its card's hardware address (ami_ns_name_after_card()). A card
     * that reports no address leaves the name to gethostname()'s own chain,
     * the interface address reverse-resolved or "localhost" (bsdsocket.doc
     * NOTES), and then this line is only approximately right; no SANA-II
     * Ethernet driver refuses S2_GETSTATIONADDRESS, so it is not worth a
     * second sentence. ShowNetStatus prints "(derived)" for the same case and
     * this says more, because here it is the answer rather than a footnote.
     */
    if (from != NULL)
        tool_printf("  named by %s\n", (LONG)from);
    else
        tool_printf("  not named by anything; derived from the card's "
                    "hardware address\n");
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    const char     *wanted;
    char            name[AMI_CFG_NAME_LEN];
    UWORD           source;
    BOOL            live;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_NAME]  = 0;
    args[ARG_QUIET] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[<name>] [QUIET]",
                   "Report this machine's name, or set it.");
        return RETURN_ERROR;
    }

    hn_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    wanted   = (const char *)args[ARG_NAME];

    /* ------------------------------------------------------- report ---- */

    if (wanted == NULL)
    {
        live = running_name(name, sizeof(name), &source);

        if (!live && !configured_name(name, sizeof(name), &source))
        {
            if (!hn_quiet)
                tool_printf("This machine has no name: nothing in "
                            "DEVS:NetInterfaces, ENV:HOSTNAME or "
                            "DEVS:Internet/name_resolution sets one.\n");
            FreeArgs(rda);
            return RETURN_WARN;
        }

        tool_printf("%s\n", (LONG)name);
        say_source(source);

        /* The files say one thing and the stack another: a DHCP lease or a
           name set since start-up. Worth a line, because the file is what the
           next boot reads. */
        if (live && !hn_quiet)
        {
            char  onfile[AMI_CFG_NAME_LEN];
            UWORD filesource;

            if (configured_name(onfile, sizeof(onfile), &filesource) &&
                tool_stricmp(onfile, name) != 0)
            {
                tool_printf("  the files on this drive say %s, from %s\n",
                            (LONG)onfile,
                            (LONG)ami_config_hostname_source_text(filesource));
            }
        }

        FreeArgs(rda);
        return RETURN_OK;
    }

    /* ---------------------------------------------------------- set ---- */

    if (!ami_config_hostname_valid(wanted))
    {
        tool_error("\"%s\" is not a host name: letters, digits and hyphens, "
                   "no hyphen at either end of a label, and under %ld "
                   "characters", (LONG)wanted, (LONG)AMI_CFG_NAME_LEN);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * ENV: so it is in force for everything started from now on, ENVARC: so it
     * survives the next boot. GVF_SAVE_VAR is what writes the second one, and
     * without it a machine renamed here is called something else after a
     * reboot, which is the surprise this command exists to remove.
     */
    if (!SetVar((CONST_STRPTR)"HOSTNAME", (CONST_STRPTR)wanted, -1,
                LV_VAR | GVF_GLOBAL_ONLY | GVF_SAVE_VAR))
    {
        tool_fault(IoErr());
        tool_error("ENV:HOSTNAME could not be written, so the name is "
                   "unchanged");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* ---- and the running stack, if there is one ----------------------- */

    if (tool_stack_library_running())
    {
        struct Library  *base = tool_netstatus_open(hn_quiet);
        NetStatusControl ctl;
        ULONG            w;
        ULONG            i;
        LONG             err = 0;

        if (base == NULL)
        {
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
            ((ULONG *)&ctl)[w] = 0;

        ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
        ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;

        for (i = 0; i + 1 < (ULONG)sizeof(ctl.nsc_HostName) &&
                    wanted[i] != '\0'; i++)
            ctl.nsc_HostName[i] = wanted[i];

        if (tool_netstatus_control(base, NETCTRL_HOSTNAME_SET, &ctl, &err) != 0)
        {
            tool_netstatus_close(base);

            if (err != HN_EPERM)
            {
                tool_error("the network would not take the name");
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            /*
             * Outranked. The file is written and the machine is not renamed,
             * and both halves of that are said, because either on its own is
             * misleading.
             */
            if (running_name(name, sizeof(name), &source))
            {
                tool_printf("%s\n", (LONG)name);
                say_source(source);
            }
            if (!hn_quiet)
                tool_printf("  %s is in ENV:HOSTNAME and ENVARC:HOSTNAME, and "
                            "will name this machine when the stronger source "
                            "stops supplying one\n", (LONG)wanted);

            FreeArgs(rda);
            return RETURN_WARN;
        }

        tool_netstatus_close(base);
    }

    /* ---- what it is now ----------------------------------------------- */

    if (!running_name(name, sizeof(name), &source))
    {
        tool_copy_string(name, sizeof(name), wanted);
        source = (UWORD)AMI_HOSTNAME_ENV;
    }

    tool_printf("%s\n", (LONG)name);
    say_source(source);

    if (!hn_quiet)
    {
        tool_printf("  written to ENV:HOSTNAME and ENVARC:HOSTNAME; DHCP "
                    "option 12 and DEVS:Internet/name_resolution both outrank "
                    "it at the next boot\n");

        /* Said only where there is a responder to be out of step with. */
        {
            NetStatusSystem sys;

            if (tool_netstatus_system(&sys) &&
                (sys.nss_Flags & NETSTATUS_SYS_MDNS) != 0 &&
                sys.nss_MdnsName[0] != '\0')
            {
                tool_printf("  .local is still %s until the network is "
                            "restarted\n", (LONG)sys.nss_MdnsName);
            }
        }
    }

    FreeArgs(rda);

    return RETURN_OK;
}
