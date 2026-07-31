/*
 * ShowNetServices -- what else on this network is offering something.
 *
 *     ShowNetServices [TYPE] [SECONDS=<n>] [TXT/S] [QUIET/S]
 *
 * The other half of mDNS. netstack_mdns.c makes this machine findable by name
 * and advertises whatever DEVS:Internet/service_discovery declares; this asks
 * the same wire what everything else has to offer. No server, no configuration
 * and no address typed by hand -- a Mac, a Linux box running Avahi, a network
 * printer and any Windows since 10 all answer.
 *
 * WHY IT TAKES A FEW SECONDS AND WHY THE LIST IS NOT "EVERYTHING"
 *
 *   A browse is a subscription, not a lookup. The query goes out, answers
 *   arrive over the following seconds from whatever is awake and listening,
 *   and there is no point at which mDNS says "that is all of them" -- a
 *   machine that boots in a minute will answer then. So this command listens
 *   for a fixed window and then prints the peer cache, which is a different
 *   thing from what is on the network in both directions: shorter, because
 *   only what was awake has answered, and longer, because a cache entry
 *   outlives the machine that put it there. The output says so. A user who
 *   reads a short list as "there is nothing else here", or a listed machine as
 *   one that is still up, will file a bug about the wrong thing.
 *
 *   The window is three seconds. A responder on the same wire answers in well
 *   under one -- RFC 6762 6 gives a shared record a 20-120 ms delay before it
 *   replies -- so three catches the ordinary case with room to spare, and a
 *   command that sat there for ten would read as hung. SECONDS raises it for a
 *   busy or a slow network.
 *
 * WITH NO TYPE
 *
 *   RFC 6763 9's meta-query: _services._dns-sd._udp.local enumerates the
 *   service TYPES present rather than instances of any one of them. That is
 *   how a browser finds out what there is to browse for, and it is what this
 *   command does when asked for nothing in particular -- a user does not know
 *   that a network printer is _ipp._tcp until something tells them.
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
    "$VER: ShowNetServices 1.0 (31.7.2026)";

#define TEMPLATE    "TYPE,SECONDS/K/N,TXT/S,QUIET/S"

enum
{
    ARG_TYPE = 0,
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
 * twenty services, so this is not the limit that binds -- but nsh_Available
 * comes back regardless, so a network larger than either is reported as
 * truncated rather than silently cut.
 */
#define SVC_MAX             48

static struct
{
    NetStatusHeader  hdr;
    NetStatusService entry[SVC_MAX];
} svc_answer;

/* ------------------------------------------------------------------ types -- */

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
    tool_advise_blank();
    tool_advise("A service type is an underscore, a name, and the protocol:");
    tool_advise("_http._tcp, _ssh._tcp, _ipp._tcp, _sftp-ssh._tcp.");
    tool_advise_blank();
    tool_advise("Run ShowNetServices with no type to see which types this");
    tool_advise("network is offering.");
}

static const char *plural(ULONG n)
{
    return (n == 1) ? "" : "s";
}

/* --------------------------------------------------------------- the list -- */

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
 * distinct nsv_Type in it -- taken from the instances as well as from the
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

/* ------------------------------------------------------------------ browse -- */

/*
 * Whether the library on this machine has a responder in it at all. A build
 * without AMINETXDUO_MDNS answers ENOSYS to the browse, which on its own would
 * read as "nothing found".
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

int main(int argc, char **argv)
{
    LONG              args[ARG_COUNT];
    struct RDArgs    *rda;
    struct Library   *base;
    NetStatusControl  ctl;
    const char       *type    = NULL;
    ULONG             seconds = SVC_SECONDS;
    BOOL              want_txt;
    BOOL              quiet;
    BOOL              broke   = FALSE;
    LONG              err     = 0;
    LONG              count;
    LONG              rc      = RETURN_OK;
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
    type     = (const char *)args[ARG_TYPE];

    if (type != NULL && !type_is_wellformed(type))
    {
        explain_type(type);
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
    base = tool_netstatus_open(quiet);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!stack_has_mdns(base))
    {
        tool_error("the running stack has no mDNS");
        tool_advise_blank();
        tool_advise("Service discovery is a build option, and the library");
        tool_advise("this machine is running was built without it. There is");
        tool_advise("nothing to switch on from here.");
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    fill_control(&ctl, type);
    if (tool_netstatus_control(base, NETCTRL_MDNS_BROWSE, &ctl, &err) < 0)
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

    count = tool_netstatus_query(base, NETSTATUS_SERVICES, &svc_answer,
                                 sizeof(svc_answer), sizeof(NetStatusService));

    /*
     * Always, and before anything is printed. A query left registered is
     * re-sent for as long as the stack is up, and it occupies the cache the
     * next browse's answers have to land in.
     */
    fill_control(&ctl, type);
    (VOID)tool_netstatus_control(base, NETCTRL_MDNS_BROWSE_STOP, &ctl, NULL);

    tool_netstatus_close(base);

    if (count < 0)
    {
        tool_error("the stack would not say what answered");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!quiet)
        tool_printf("\n");

    for (i = 0; i < (UWORD)count; i++)
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

        if (!quiet)
        {
            tool_advise_blank();
            tool_advise("That is not the same as nothing being there. A");
            tool_advise("machine that is asleep, on another network segment,");
            tool_advise("or simply slow does not answer inside the window --");
            tool_advise("try again with SECONDS=10.");

            if (type != NULL)
            {
                tool_advise_blank();
                tool_advise("Run ShowNetServices with no type to see which");
                tool_advise("types this network is offering at all.");
            }
        }

        rc = RETURN_WARN;
    }
    else if (!quiet)
    {
        /*
         * Said every time, not only when the list looks short. mDNS has no
         * complete answer to give and a list that did not say so would be read
         * as one.
         *
         * "heard recently" rather than "answered just now": the collect walks
         * the whole peer cache, so a second browse inside the TTL still lists a
         * machine that did not answer it. Claiming the window would be true
         * only on a cold cache.
         */
        tool_advise_blank();
        tool_advise("This is what this machine has heard recently, not");
        tool_advise("everything on the network, and something listed may");
        tool_advise("since have gone. Ask again for a longer look with");
        tool_advise("SECONDS=10.");

        if (type == NULL)
        {
            tool_advise_blank();
            tool_advise("Browse one of them:  ShowNetServices _http._tcp");
        }

        if (svc_answer.hdr.nsh_Available > svc_answer.hdr.nsh_Count)
        {
            tool_advise_blank();
            tool_advise("More answered than are shown here.");
        }
    }

    if (broke)
        rc = RETURN_WARN;

    FreeArgs(rda);
    return rc;
}
