/*
 * hostname, what this machine calls itself, and where that came from.
 *
 * Four ranked sources name a machine (AmiHostnameSource, aminetxduo/config.h):
 * an interface file's ID=, ENV:HOSTNAME, DHCP option 12, then HOSTNAME= in
 * DEVS:Internet/name_resolution. A stronger source replaces a weaker one.
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

/* bsdsocket.library reports the BSD errno numbers, which newlib's <errno.h>
   does not agree with everywhere; its macros must not be used here. */
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
       AMI_HOSTNAME_NONE: the name then reads as set by nothing. */
    if (tool_netstatus_system(&sys))
        *source = (UWORD)sys.nss_HostSource;

    return TRUE;
}

/*
 * The name the files say, for a machine with no stack running. The same chain
 * ami_config_load() runs at start-up, minus DHCP.
 */
static BOOL configured_name(char *out, ULONG outlen, UWORD *source)
{
    static AmiConfig cfg;               /* far too big for a 4 KB stack */
    BOOL             found = FALSE;

    *source = (UWORD)AMI_HOSTNAME_NONE;
    out[0]  = '\0';

    /* Everything wanted is copied out before the free, so the whole load has
       exactly one lifetime. ami_config_load() allocates the interface list. */
    if (ami_config_load(&cfg) == AMI_CFG_OK && cfg.hostname[0] != '\0')
    {
        tool_copy_string(out, outlen, cfg.hostname);
        *source = cfg.hostname_source;
        found   = TRUE;
    }

    ami_config_free(&cfg);

    return found;
}

/* "ENV:HOSTNAME", or the phrase for a machine nothing named. */
static VOID say_source(UWORD source, const char *name)
{
    const char *from = ami_config_hostname_source_text(source);

    if (hn_quiet)
        return;

    /*
     * No source means nothing configured one, so the stack named the machine
     * after its card's hardware address (ami_ns_name_after_card()); the bare
     * "amiga" is what that falls back to when no card would give one.
     */
    if (from != NULL)
        tool_printf("  named by %s\n", (LONG)from);
    else if (tool_stricmp(name, "amiga") == 0)
        tool_printf("  not named by anything. No card gave a hardware "
                    "address to name it after, so every other machine in the "
                    "same state has this name\n");
    else
        tool_printf("  not named by anything. The name comes from the card's "
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
        say_source(source, name);

        /* The files say one thing and the stack another: a DHCP lease or a name
           set since start-up. The file is what the next boot reads. */
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
                   "no hyphen at either end of a label, and less than %ld "
                   "characters", (LONG)wanted, (LONG)AMI_CFG_NAME_LEN);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * ENV: so it is in force from now on, ENVARC: so it survives the next boot.
     * GVF_SAVE_VAR is what writes the second one.
     */
    if (!SetVar((CONST_STRPTR)"HOSTNAME", (CONST_STRPTR)wanted, -1,
                LV_VAR | GVF_GLOBAL_ONLY | GVF_SAVE_VAR))
    {
        tool_fault(IoErr());
        tool_error("ENV:HOSTNAME was not written, so the name is "
                   "unchanged");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* ---- and the running stack, if there is one ----------------------- */

    if (tool_stack_library_running())
    {
        /* FALSE: QUIET is the name and nothing else, and a stack that would
           not take the name is not the name. */
        struct Library  *base = tool_netstatus_open(FALSE);
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
                tool_error("the network refused the name");
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            /*
             * Outranked. The file is written and the machine is not renamed;
             * both halves are said, because either alone is misleading.
             */
            if (running_name(name, sizeof(name), &source))
            {
                tool_printf("%s\n", (LONG)name);
                say_source(source, name);
            }
            if (!hn_quiet)
                tool_printf("  %s is in ENV:HOSTNAME and ENVARC:HOSTNAME, and "
                            "will name this machine when the stronger source "
                            "no longer supplies one\n", (LONG)wanted);

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
    say_source(source, name);

    if (!hn_quiet)
    {
        tool_printf("  written to ENV:HOSTNAME and ENVARC:HOSTNAME. DHCP "
                    "option 12 and DEVS:Internet/name_resolution both outrank "
                    "it at the next boot\n");

        /* Said only where there is a responder to be out of step with. */
        {
            NetStatusSystem sys;

            if (tool_netstatus_system(&sys) &&
                (sys.nss_Flags & NETSTATUS_SYS_MDNS) != 0 &&
                sys.nss_MdnsName[0] != '\0')
            {
                tool_printf("  .local is still %s until the network "
                            "restarts\n", (LONG)sys.nss_MdnsName);
            }
        }
    }

    FreeArgs(rda);

    return RETURN_OK;
}
