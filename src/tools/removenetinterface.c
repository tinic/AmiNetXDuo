/*
 * RemoveNetInterface, take one interface out of the running stack.
 *
 *     RemoveNetInterface INTERFACE/M/A,FORCE/S,QUIET/S
 *
 * The counterpart of AddNetInterface, and a narrower thing than NetShutdown:
 * that one stops every interface there is and leaves them all configured,
 * while this one takes a single interface out altogether. Its SANA-II device
 * is closed, so the hardware is free for something else to open, and its
 * configuration slot is released, so the same name can be added again with a
 * different file behind it.
 *
 * Interfaces are not renumbered by a removal. An index is a handle a caller
 * may already be holding, so removing the first of three leaves the other two
 * where they were.
 *
 * An interface still carrying TCP connections is refused rather than removed,
 * because detaching it resets every one of them, which is visible at the far
 * end of the wire. FORCE says to do it anyway.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "RemoveNetInterface";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("RemoveNetInterface");

#define TEMPLATE    "INTERFACE/M/A,FORCE/S,QUIET/S"

enum
{
    ARG_INTERFACE = 0,
    ARG_FORCE,
    ARG_QUIET,
    ARG_COUNT
};

static BOOL rni_quiet;

static VOID say(const char *fmt, ...)
{
    va_list args;

    if (rni_quiet)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);         /* (APTR): see tool_util.c */
    va_end(args);
}

/* Static: this is most of a Shell command's 4 KB stack on its own. */
static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[NX_MAX_PHYSICAL_INTERFACES];
} rni_ifaces;

static BOOL same_name(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return FALSE;
    }

    return (*a == '\0' && *b == '\0') ? TRUE : FALSE;
}

/*
 * The running stack's own idea of which interfaces exist, which is the only
 * one that counts here: tool_find_interface() reads a netstack linked into the
 * calling binary, and this command has none.
 *
 * Re-read for every name rather than once, because each removal changes it.
 */
static LONG find_index(struct Library *base, const char *name)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &rni_ifaces,
                             sizeof(rni_ifaces), sizeof(NetStatusInterface));
    if (n < 0)
        return -2;

    /* nsh_Count is the library's number, not ours: bound it by the table it
       is being used to index, as shownetservices.c and tool_nx.c do. */
    for (i = 0; i < n && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (!(rni_ifaces.e[i].nsi_Flags & NETSTATUS_IF_NAMED))
            continue;

        if (same_name(rni_ifaces.e[i].nsi_Name, name))
            return (LONG)rni_ifaces.e[i].nsi_Index;
    }

    return -1;
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    struct Library  *base;
    STRPTR          *names;
    BOOL             force;
    LONG             removed = 0;
    LONG             failed  = 0;
    LONG             n;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACE] = 0;
    args[ARG_FORCE]     = 0;
    args[ARG_QUIET]     = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<interface> [FORCE] [QUIET]",
                   "Take one interface out of the running network.");
        return RETURN_ERROR;
    }

    rni_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    force     = (args[ARG_FORCE] != 0) ? TRUE : FALSE;
    names     = (STRPTR *)args[ARG_INTERFACE];

    /* Opening the library would start the stack, and starting a network in
       order to take an interface out of it is not what was asked for. */
    if (!tool_stack_library_running())
    {
        say("The network is not running, so there is nothing to remove.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    /* FALSE: QUIET drops what was removed, never why it could not be. */
    base = tool_netstatus_open(FALSE);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (n = 0; names[n] != NULL; n++)
    {
        /* The same spelling AddNetInterface accepts: a name, or the path of
           the file it was configured from. */
        const char      *name = tool_basename((const char *)names[n]);
        LONG             index;
        LONG             err = 0;
        NetStatusControl ctl;
        ULONG            w;

        index = find_index(base, name);
        if (index == -2)
        {
            tool_error("the network did not say which interfaces it has");
            tool_explain_no_netstatus(base);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }
        if (index < 0)
        {
            tool_error("there is no interface called \"%s\"", (LONG)name);
            failed++;
            continue;
        }

        for (w = 0; w < (ULONG)(sizeof(ctl) / sizeof(ULONG)); w++)
            ((ULONG *)&ctl)[w] = 0;

        ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
        ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
        ctl.nsc_Index   = (UWORD)index;
        ctl.nsc_Flags   = force ? NETCTRL_F_FORCE : 0UL;

        if (tool_netstatus_control(base, NETCTRL_INTERFACE_REMOVE, &ctl,
                                   &err) != 0)
        {
            if (err == EBUSY)
            {
                tool_error("%s still has connections open. FORCE removes it "
                           "anyway and resets them", (LONG)name);
            }
            else
            {
                tool_error("%s was not removed", (LONG)name);
            }

            failed++;
            continue;
        }

        say("%s: removed\n", (LONG)name);
        removed++;
    }

    tool_netstatus_close(base);
    FreeArgs(rda);

    if (failed > 0)
        return (removed > 0) ? RETURN_WARN : RETURN_FAIL;

    return RETURN_OK;
}
