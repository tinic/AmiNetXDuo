/*
 * ShowNetServices, what else on this network is offering something.
 *
 *     ShowNetServices TYPE,ALL/S,SECONDS/K/N,TXT/S,QUIET/S
 *
 * The other half of mDNS. netstack_mdns.c makes this machine findable by name
 * and advertises whatever DEVS:Internet/service_discovery declares; this asks
 * the same wire what everything else has to offer. No server, no configuration
 * and no address typed by hand, a Mac, a Linux box running Avahi, a network
 * printer and any Windows since 10 all answer.
 *
 * WHY IT TAKES A FEW SECONDS AND WHY THE LIST IS NOT "EVERYTHING"
 *
 *   A browse is a subscription, not a lookup. The query goes out, answers
 *   arrive over the following seconds from whatever is awake and listening,
 *   and there is no point at which mDNS says "that is all of them", a
 *   machine that boots in a minute will answer then. So this command listens
 *   for a fixed window and then prints the peer cache, which is a different
 *   thing from what is on the network in both directions: shorter, because
 *   only what was awake has answered, and longer, because a cache entry
 *   outlives the machine that put it there. The output says so. A user who
 *   reads a short list as "there is nothing else here", or a listed machine as
 *   one that is still up, will file a bug about the wrong thing.
 *
 *   The window is three seconds. A responder on the same wire answers in well
 *   under one, RFC 6762 6 gives a shared record a 20-120 ms delay before it
 *   replies, so three catches the ordinary case with room to spare, and a
 *   command that sat there for ten would read as hung. SECONDS raises it for a
 *   busy or a slow network.
 *
 * WITH NO TYPE
 *
 *   RFC 6763 9's meta-query: _services._dns-sd._udp.local enumerates the
 *   service TYPES present rather than instances of any one of them. That is
 *   how a browser finds out what there is to browse for, and it is what this
 *   command does when asked for nothing in particular, a user does not know
 *   that a network printer is _ipp._tcp until something tells them.
 *
 * WITH ALL
 *
 *   The types, and then every instance of every one of them. It costs one more
 *   window and not one per type: a continuous query per type is registered and
 *   they all run at once, so a machine answers whichever of them apply to it
 *   in a single response.
 *
 *   What it does cost is peer cache. Four records land per instance and the
 *   queries themselves take a record each, and when the cache fills the module
 *   evicts the least recently used record rather than refusing the new one,
 *   which used to mean an SRV outliving the A record it points at, and a row
 *   printing "no address". That is now chased: netstack_mdns_browse_collect()
 *   asks for an address the browse did not carry. SVC_TYPES_MAX bounds the
 *   rest.
 *
 * WHAT IT DOES NOT DO
 *
 *   It does not connect to anything it finds. An SRV record is a claim by the
 *   machine that published it, and this command reports the claim.
 *
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

/* The window, and the range SECONDS may set it to. One second is enough on a
   quiet wire; past a minute the user wants a browser, not a command. */
#define SVC_SECONDS         3
#define SVC_SECONDS_MIN     1
#define SVC_SECONDS_MAX     60

/*
 * How many rows to ask the library for. The stack's peer cache holds about
 * twenty services, so this is not the limit that binds, but nsh_Available
 * comes back regardless, so a network larger than either is reported as
 * truncated rather than silently cut.
 */
#define SVC_MAX             48

/*
 * How many types ALL will browse at once. Each is a query record in the peer
 * cache, and each brings back four more records for every instance behind it,
 * so this is what stops a network offering a hundred types from evicting the
 * answers to the first ninety. A house LAN offers around twenty.
 */
#define SVC_TYPES_MAX       32

static struct
{
    NetStatusHeader  hdr;
    NetStatusService entry[SVC_MAX];
} svc_answer;

/* Off the stack for the same reason svc_answer is. */
static char svc_types[SVC_TYPES_MAX][NETSTATUS_SVC_TYPE_LEN];

/* ------------------------------------------------------------------ types, */

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

/* --------------------------------------------------------------- the list, */

/*
 * A field a responder chose, printed. A TXT record is arbitrary bytes and an
 * instance name is free UTF-8 text (RFC 6763 4.1.1); either can carry
 * something that would move the cursor or clear the screen. Anything below a
 * space becomes '.' so a hostile or merely broken responder cannot rearrange
 * the terminal this list is going to be pasted out of.
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

/*
 * One service, as two or three lines rather than a row of columns.
 *
 * A table was the first shape and it does not survive contact with a real
 * network: RFC 6763 4.1.1 instance names are free text and the ones on this
 * LAN run to "HIKVISION DS-2CD2085FWD-I - 167921371" with a host name of
 * "DS-2CD2085FWD-I20180113AAWR167921371.local" beside it. Every column past
 * the first was pushed out of line by the first entry, which is worse than no
 * columns at all in something meant to be pasted into a post.
 */
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

    /*
     * A service with no SRV record has no port either, and printing "port 0"
     * would look like a fact. That happens when a PTR arrived and the record
     * it points at has not: the responder answered the browse and nothing has
     * asked it for the details yet.
     */
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

/*
 * Whether this type has already been listed.
 *
 * NETSTATUS_SERVICES answers with the whole cache, so the type list is every
 * distinct nsv_Type in it, taken from the instances as well as from the
 * meta-query's bare rows, because the two overlap. The module drops a bare
 * type row once it holds an instance of that type, it being the less
 * informative of the two, so a list built from the bare rows alone loses
 * exactly the types that answered best.
 */
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

/* --------------------------------------------------------------- every type, */

/*
 * The types the meta-query turned up, so that one continuous query can be
 * started per type. They are collected from the instances as well as from the
 * bare rows, for the reason type_already_shown() gives.
 *
 * Returns how many were taken; a network offering more than SVC_TYPES_MAX
 * leaves the rest alone.
 */
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

/* ------------------------------------------------------------------ browse, */

/*
 * Whether the library on this machine has a responder in it at all. A build
 * without AMINETXDUO_MDNS answers ENOSYS to the browse, which on its own would
 * read as "nothing found".
 */
/*
 * Is any interface actually answering .local?  Distinct from stack_has_mdns():
 * that asks whether this build has a responder at all, this asks whether any
 * interface asked it to run.  MDNS= is per interface and defaults to off, so
 * "built in and idle" is the ordinary case, not a fault.
 */
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

    /* Never starts the stack: a machine with no network has nothing to
       discover, and starting one to say so would be a surprise. */
    /* FALSE: QUIET drops the progress lines, never the reason nothing can be
       discovered. */
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

    /*
     * ALL is two windows, not one per type. The first is the meta-query above,
     * which says which types are here; the second asks after all of them at
     * once, since RFC 6762 5.2 queries run concurrently and a machine answers
     * whichever of them apply to it in one response.
     *
     * The meta-query is retired first: it has said what it had to say, and its
     * record and its answers are peer cache the instances now need.
     *
     * Ctrl-C in the first window drops back to listing the types, which is what
     * that window collected. Starting a second round of queries to stop them
     * again is not what the key was pressed for.
     */
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
            /*
             * The selector answers with the whole cache, so the type is
             * matched here. Instances only: a bare type row is the
             * meta-query's answer and says that something offers the type,
             * not which machine does.
             */
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
        /*
         * A type the meta-query turned up and no instance answered for. The
         * machine behind it did not reply inside the window, so naming the
         * types keeps that separate from their not being there.
         */
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

        /* SVC_MAX is what the library was asked for; nsh_Available is what it
           had. Saying nothing here would present a cut list as the whole one. */
        if (svc_answer.hdr.nsh_Available > svc_answer.hdr.nsh_Count)
            tool_printf("More answered than are shown here.\n");
    }

    if (broke)
        rc = RETURN_WARN;

    FreeArgs(rda);
    return rc;
}
