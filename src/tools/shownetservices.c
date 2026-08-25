/*
 * ShowNetServices, what else on this network is offering something.
 *
 *     ShowNetServices TYPE,ALL/S,SECONDS/K/N,TXT/S,QUIET/S
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "ShowNetServices";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ShowNetServices");

#define TEMPLATE    "TYPE,ALL/S,SECONDS/K/N,TXT/S,QUIET/S"

enum
{
    ARG_TYPE = 0,
    ARG_ALL,
    ARG_SECONDS,
    ARG_TXT,
    ARG_QUIET,
    ARG_COUNT
};

/* The window, and the range SECONDS can set it to. One second is enough on a
   quiet wire. Past a minute a Shell command is the wrong shape for the job. */
#define SVC_SECONDS         3
#define SVC_SECONDS_MIN     1
#define SVC_SECONDS_MAX     60

#define SVC_MAX             48

#define SVC_TYPES_MAX       32

static struct
{
    NetStatusHeader  hdr;
    NetStatusService entry[SVC_MAX];
} svc_answer;

/* Off the stack for the same reason svc_answer is. */
static char svc_types[SVC_TYPES_MAX][NETSTATUS_SVC_TYPE_LEN];

/*
 * A service type, as RFC 6763 4.1.2 spells one: "_" then one to fifteen
 * characters, then "._tcp" or "._udp". Checked here rather than left to the
 * library because the failure is a user typing "http" or "_http", and the
 * answer to both is the same sentence.
 */
static BOOL type_is_wellformed(const char *type)
{
    ULONG len = 0;
    ULONG dot = 0;
    ULONG i;

    if (type == NULL || type[0] != '_')
        return FALSE;

    while (type[len] != '\0')
        len++;

    for (i = 0; i < len; i++)
    {
        if (type[i] == '.')
            dot = i;
    }

    if (dot == 0 || dot + 5 != len)
        return FALSE;

    if (type[dot + 1] != '_')
        return FALSE;

    if (!((type[dot + 2] == 't' && type[dot + 3] == 'c' && type[dot + 4] == 'p')
          || (type[dot + 2] == 'u' && type[dot + 3] == 'd'
              && type[dot + 4] == 'p')))
    {
        return FALSE;
    }

    /* "_x._tcp" at the shortest, "_fifteenchars12._tcp" at the longest. */
    return (BOOL)(dot >= 2 && dot <= 16);
}

static VOID explain_type(const char *type)
{
    tool_error("\"%s\" is not a service type", (LONG)type);
}

static const char *plural(ULONG n)
{
    return (n == 1) ? "" : "s";
}

/*
 * A field a responder chose, printed. A TXT record is arbitrary bytes and an
 * instance name is free UTF-8 text (RFC 6763 4.1.1), and either can carry
 * something that would move the cursor or clear the screen. Anything below a
 * space becomes '.' so a hostile or broken responder cannot rearrange the
 * terminal this list is going to be pasted out of.
 */
static VOID put_safe(const char *text)
{
    char  buf[NETSTATUS_SVC_TXT_LEN];
    ULONG i = 0;

    while (text[i] != '\0' && i + 1 < (ULONG)sizeof(buf))
    {
        char c = text[i];

        buf[i] = (c < ' ' || c == 0x7F) ? '.' : c;
        i++;
    }
    buf[i] = '\0';

    tool_printf("%s", (LONG)buf);
}

static VOID put_address(ULONG addr)
{
    tool_printf("%lu.%lu.%lu.%lu",
                (LONG)((addr >> 24) & 0xFFUL),
                (LONG)((addr >> 16) & 0xFFUL),
                (LONG)((addr >> 8) & 0xFFUL),
                (LONG)(addr & 0xFFUL));
}

static VOID print_instance(const NetStatusService *e, BOOL want_txt)
{
    tool_printf("  ");
    put_safe(e->nsv_Name);

    if (e->nsv_Flags & NETSTATUS_SVC_LOCAL)
        tool_printf("  (this machine)");

    tool_printf("\n      ");

    if (e->nsv_Host[0] != '\0')
    {
        put_safe(e->nsv_Host);
        tool_printf("  ");
    }

    if (e->nsv_Flags & NETSTATUS_SVC_ADDRESS)
        put_address(e->nsv_Address);
    else
        tool_printf("no address");

    if (e->nsv_Port != 0)
        tool_printf("  port %lu", (LONG)e->nsv_Port);

    tool_printf("\n");

    if (want_txt && (e->nsv_Flags & NETSTATUS_SVC_TXT))
    {
        tool_printf("      ");
        put_safe(e->nsv_Text);
        if (e->nsv_Flags & NETSTATUS_SVC_TXTCUT)
            tool_printf(" ...");
        tool_printf("\n");
    }
}

/* DNS names compare without regard to case (RFC 4343), and a type read off the
   wire need not be spelled the way it was typed. */
static BOOL same_name(const char *a, const char *b)
{
    ULONG i = 0;

    for (;;)
    {
        char x = a[i];
        char y = b[i];

        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');

        if (x != y)
            return FALSE;
        if (x == '\0')
            return TRUE;

        i++;
    }
}

static BOOL type_already_shown(UWORD upto, const char *type)
{
    UWORD i;

    for (i = 0; i < upto; i++)
    {
        if (same_name(svc_answer.entry[i].nsv_Type, type))
            return TRUE;
    }

    return FALSE;
}

static UWORD collect_types(UWORD count)
{
    UWORD taken = 0;
    UWORD i;
    UWORD j;

    for (i = 0; i < count; i++)
    {
        const char *type = svc_answer.entry[i].nsv_Type;
        BOOL        seen = FALSE;

        if (type[0] == '\0')
            continue;

        for (j = 0; j < taken; j++)
        {
            if (same_name(svc_types[j], type))
            {
                seen = TRUE;
                break;
            }
        }

        if (seen)
            continue;

        if (taken == (UWORD)SVC_TYPES_MAX)
            break;

        for (j = 0; j + 1 < (UWORD)NETSTATUS_SVC_TYPE_LEN && type[j] != '\0';
             j++)
        {
            svc_types[taken][j] = type[j];
        }
        svc_types[taken][j] = '\0';
        taken++;
    }

    return taken;
}

/* The instances of one type, under its own heading. Returns how many. */
static UWORD print_type(UWORD count, const char *type, BOOL want_txt)
{
    UWORD shown = 0;
    UWORD i;

    for (i = 0; i < count; i++)
    {
        const NetStatusService *e = &svc_answer.entry[i];

        if (!(e->nsv_Flags & NETSTATUS_SVC_INSTANCE))
            continue;
        if (!same_name(e->nsv_Type, type))
            continue;

        if (shown == 0)
        {
            put_safe(type);
            tool_printf("\n\n");
        }

        print_instance(e, want_txt);
        shown++;
    }

    if (shown != 0)
        tool_printf("\n");

    return shown;
}

static BOOL mdns_enabled_somewhere(struct Library *base)
{
    struct
    {
        NetStatusHeader    hdr;
        NetStatusInterface e[NX_MAX_PHYSICAL_INTERFACES];
    } answer;
    LONG count;
    LONG i;

    count = tool_netstatus_query(base, NETSTATUS_INTERFACES, &answer,
                                 sizeof(answer), sizeof(NetStatusInterface));
    if (count <= 0)
        return FALSE;

    for (i = 0; i < count && i < (LONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (answer.e[i].nsi_Flags & NETSTATUS_IF_MDNS)
            return TRUE;
    }

    return FALSE;
}

/*
 * Whether the library on this machine has a responder in it at all. A build
 * without AMINETXDUO_MDNS answers ENOSYS to the browse, which on its own would
 * read as an empty network.
 */
static BOOL stack_has_mdns(struct Library *base)
{
    struct
    {
        NetStatusHeader hdr;
        NetStatusSystem sys;
    } answer;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &answer, sizeof(answer),
                             sizeof(NetStatusSystem)) <= 0)
    {
        return FALSE;
    }

    return (answer.sys.nss_Flags & NETSTATUS_SYS_MDNS) ? TRUE : FALSE;
}

static VOID fill_control(NetStatusControl *ctl, const char *type)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(*ctl) / sizeof(ULONG)); i++)
        ((ULONG *)ctl)[i] = 0;

    if (type == NULL)
        return;

    for (i = 0; i + 1 < (ULONG)sizeof(ctl->nsc_Name) && type[i] != '\0'; i++)
        ctl->nsc_Name[i] = type[i];
}

/* NULL is the meta-query, here as everywhere below. */
static LONG browse_start(struct Library *base, const char *type, LONG *err)
{
    NetStatusControl ctl;

    fill_control(&ctl, type);

    return tool_netstatus_control(base, NETCTRL_MDNS_BROWSE, &ctl, err);
}

/*
 * Always, and before anything is printed. A query left registered is re-sent
 * for as long as the stack is up, and it occupies the cache the next browse's
 * answers have to land in.
 */
static VOID browse_stop(struct Library *base, const char *type)
{
    NetStatusControl ctl;

    fill_control(&ctl, type);
    (VOID)tool_netstatus_control(base, NETCTRL_MDNS_BROWSE_STOP, &ctl, NULL);
}

static LONG read_services(struct Library *base)
{
    return tool_netstatus_query(base, NETSTATUS_SERVICES, &svc_answer,
                                sizeof(svc_answer), sizeof(NetStatusService));
}

int main(int argc, char **argv)
{
    LONG              args[ARG_COUNT];
    struct RDArgs    *rda;
    struct Library   *base;
    UBYTE             had[SVC_TYPES_MAX] = { 0 };
    const char       *type    = NULL;
    ULONG             seconds = SVC_SECONDS;
    BOOL              want_txt;
    BOOL              quiet;
    BOOL              all;
    BOOL              broke   = FALSE;
    LONG              err     = 0;
    LONG              count;
    LONG              rc      = RETURN_OK;
    UWORD             ntypes  = 0;
    UWORD             shown   = 0;
    UWORD             i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (UWORD)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    quiet    = (BOOL)(args[ARG_QUIET] != 0);
    want_txt = (BOOL)(args[ARG_TXT] != 0);
    all      = (BOOL)(args[ARG_ALL] != 0);
    type     = (const char *)args[ARG_TYPE];

    if (type != NULL && !type_is_wellformed(type))
    {
        explain_type(type);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (all && type != NULL)
    {
        tool_error("ALL browses every type, so it takes no type");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_SECONDS] != 0)
    {
        seconds = (ULONG)(*(LONG *)args[ARG_SECONDS]);

        if (seconds < SVC_SECONDS_MIN || seconds > SVC_SECONDS_MAX)
        {
            tool_error("SECONDS must be between %lu and %lu",
                       (LONG)SVC_SECONDS_MIN, (LONG)SVC_SECONDS_MAX);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    base = tool_netstatus_open(FALSE);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!stack_has_mdns(base))
    {
        tool_error("the running stack has no mDNS");
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!mdns_enabled_somewhere(base))
    {
        tool_error("mDNS is not enabled on any interface. Add MDNS=YES to "
                   "the interface file in DEVS:NetInterfaces, then restart "
                   "the stack");
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (browse_start(base, type, &err) < 0)
    {
        tool_error("cannot ask the network: %s", (LONG)tool_net_error(err));
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!quiet)
    {
        if (type != NULL)
            tool_printf("Asking for %s, listening %lu second%s...\n",
                        (LONG)type, (LONG)seconds, (LONG)plural(seconds));
        else
            tool_printf("Asking what this network offers, listening %lu "
                        "second%s...\n", (LONG)seconds, (LONG)plural(seconds));
    }

    /* In slices, so Ctrl-C stops the wait rather than the window having to
       run out first. The query is retired either way, below. */
    broke = tool_delay_ticks(seconds * 50UL);

    count = read_services(base);

    if (all && broke)
        all = FALSE;

    if (all && count > 0)
    {
        browse_stop(base, NULL);

        ntypes = collect_types((UWORD)count);

        for (i = 0; i < ntypes; i++)
            (VOID)browse_start(base, svc_types[i], NULL);

        if (!quiet)
            tool_printf("Asking after %lu kind%s of service, listening %lu "
                        "second%s...\n", (LONG)ntypes, (LONG)plural(ntypes),
                        (LONG)seconds, (LONG)plural(seconds));

        if (!broke)
            broke = tool_delay_ticks(seconds * 50UL);

        count = read_services(base);

        for (i = 0; i < ntypes; i++)
            browse_stop(base, svc_types[i]);
    }
    else
    {
        browse_stop(base, type);
    }

    tool_netstatus_close(base);

    if (count < 0)
    {
        tool_error("the stack did not say what answered");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!quiet)
        tool_printf("\n");

    /* ALL prints by type, in the order the meta-query turned them up. */
    for (i = 0; all && i < ntypes; i++)
    {
        UWORD n = print_type((UWORD)count, svc_types[i], want_txt);

        had[i]  = (UBYTE)((n != 0) ? 1 : 0);
        shown  += n;
    }

    for (i = 0; !all && i < (UWORD)count; i++)
    {
        const NetStatusService *e = &svc_answer.entry[i];

        if (type != NULL)
        {
            if (!(e->nsv_Flags & NETSTATUS_SVC_INSTANCE))
                continue;
            if (!same_name(e->nsv_Type, type))
                continue;

            if (shown == 0 && !quiet)
                tool_printf("%s here\n\n", (LONG)type);

            print_instance(e, want_txt);
        }
        else
        {
            if (e->nsv_Type[0] == '\0' || type_already_shown(i, e->nsv_Type))
                continue;

            if (shown == 0 && !quiet)
                tool_printf("Service types answering here\n\n");

            tool_printf("  ");
            put_safe(e->nsv_Type);
            tool_printf("\n");
        }

        shown++;
    }

    if (shown == 0)
    {
        if (type != NULL)
            tool_printf("Nothing answered for %s in %lu second%s.\n",
                        (LONG)type, (LONG)seconds, (LONG)plural(seconds));
        else
            tool_printf("Nothing answered in %lu second%s.\n",
                        (LONG)seconds, (LONG)plural(seconds));

        rc = RETURN_WARN;
    }
    else if (!quiet)
    {
        if (all)
        {
            BOOL first = TRUE;

            for (i = 0; i < ntypes; i++)
            {
                if (had[i])
                    continue;

                if (first)
                {
                    tool_printf("No instance answered for:");
                    first = FALSE;
                }

                tool_printf(" ");
                put_safe(svc_types[i]);
            }

            if (!first)
                tool_printf("\n");
        }

        /* SVC_MAX is what the library was asked for, nsh_Available is what it
           had. Saying nothing here would present a cut list as the whole one. */
        if (svc_answer.hdr.nsh_Available > svc_answer.hdr.nsh_Count)
            tool_printf("More answered than are shown here.\n");
    }

    if (broke)
        rc = RETURN_WARN;

    FreeArgs(rda);
    return rc;
}
