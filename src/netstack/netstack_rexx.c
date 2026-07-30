/*
 * AmiNetXDuo -- the AMITCP public port, and the ARexx host behind it.
 *
 * The port exists so `WaitForPort AMITCP` in a startup script returns once the
 * stack is up. That is not all it is, which is what this file is for: AMITCP is
 * AmiTCP's *ARexx host port*, and 31 of the 2,149 Aminet comm/ archives
 * surveyed in docs/RESEARCH.md 75 send commands to it -- AmiTCP's own
 * bin/stopnet and bin/netstat, Genesis's copies of both, SLIPCall, Netdial,
 * TCPFront, netbeginner, interinstall, TCP_Start_Stop, rx.fingerd.
 *
 * A port with PA_IGNORE and no signal task answers none of them. RexxSysLib
 * finds the port, PutMsg()es a RexxMsg and waits for a reply that never comes,
 * so the script hangs -- where with no port at all it would have failed at once
 * with "host environment not found". So the port has to be serviced, and every
 * message replied to, including the ones we do not implement: an error reply is
 * what restores the behaviour the port's absence used to give.
 *
 * The command set is AmiTCP 3.0b2's, from src/amitcp/kern/amiga_config.h:
 *
 *     #define REXXKEYWORDS "Q=QUERY,S=SET,READ,ROUTE,ADD,RESET,KILL"
 *
 * parsed the way AmiTCP parsed it -- ReadItem() then FindArg() -- so
 * abbreviation and quoting behave identically without us reimplementing them.
 * What each keyword does here, and why, is at ami_rx_execute();
 * netstack_rexx_vars.c has QUERY and SET's variable space.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * A private rexxsyslib base, the way src/tls/tls_amiga.c takes a private
 * TimerBase: the NDK inlines are parameterised for it, and with -fno-common a
 * conventional `RexxSysBase` here would collide with anything else defining one.
 */
#define REXXSYSLIB_BASE_NAME ami_rx_rexxbase

#include "netstack_internal.h"
#include "netstack_rexx.h"

#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <rexx/storage.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/rexxsyslib.h>

struct Library *ami_rx_rexxbase;

/* ----------------------------------------------------------------- names -- */

static char ami_rx_port_name[]  = "AMITCP";

/*
 * AmiTCP's own keyword list and its order; the enum has to match.
 * "ROUTE is currently not implemented" is AmiTCP's comment on its own line 95.
 */
static const char ami_rx_keywords[] = "Q=QUERY,S=SET,READ,ROUTE,ADD,RESET,KILL";

enum
{
    RX_KEY_QUERY = 0,
    RX_KEY_SET,
    RX_KEY_READ,
    RX_KEY_ROUTE,
    RX_KEY_ADD,
    RX_KEY_RESET,
    RX_KEY_KILL
};

/* The error strings and the two buffer sizes are in netstack_rexx.h, which the
   variable space shares. AmiTCP's command line was one CONFIGLINELEN at most;
   this is the length of the longest QUERY netstat sends, rounded up. */
#define RX_CMDLEN       1024

/* One command line (RX_CMDLEN) and one reply (RX_REPLYBUFLEN) are both on this
   stack while a QUERY runs, and netstat's TCP query fills most of both. */
#define RX_STACK        8192
#define RX_PRIORITY     0

/* ----------------------------------------------------------------- state -- */

typedef struct AmiRexxBoot
{
    struct Task *rb_Parent;
    BOOL         rb_Ok;
} AmiRexxBoot;

static struct MsgPort  *ami_rx_port;        /* the AMITCP port itself       */
static struct Process  *ami_rx_proc;
static struct MsgPort  *ami_rx_death;       /* parent's; the child posts here */
static struct Message   ami_rx_death_msg;
static AmiRexxBoot     *ami_rx_boot;

/* ------------------------------------------------------- rexxsyslib calls --
 *
 * AmiTCP also set an ARexx variable, AMITCP.LASTERROR, alongside the return
 * code. That is not done here. SetRexxVarFromMsg() is the only way to reach it
 * from a shared library -- amiga.lib's SetRexxVar() needs a base we do not have
 * -- and only some NDKs declare it, so building against it makes the result
 * depend on which NDK is installed. Nothing in the 31-archive corpus reads
 * LASTERROR (docs/RESEARCH.md 75.7); what a script reads as RC is rm_Result1,
 * which is set, and an error string comes back in rm_Result2 when the caller
 * asked for a result.
 */

/* --------------------------------------------------------------- commands -- */

static ULONG ami_rx_strlen(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/*
 * KILL. AmiTCP's was Signal(AmiTCP_Task, SIGBREAKF_CTRL_C) -- the stack task
 * exited and the library went with it. Ours cannot: bsdsocket.library is a
 * singleton that lives until reboot once anything has opened it, which
 * src/tools/netshutdown.c documents at length. What is stoppable is the
 * traffic, so KILL does what NetShutdown does and takes every interface down
 * through the same netstack_interface_down() that Offline calls.
 *
 * The port stays. `stopnet`'s optional FLUSH branch tests Show(ports, AMITCP)
 * before flushing memory, so it correctly declines to flush -- the stack really
 * is still resident.
 */
static LONG ami_rx_kill(void)
{
    AmiNetStack *ns = ami_netstack_raw();
    UWORD        i;
    LONG         worst = RETURN_OK;

    if (ns == NULL)
        return RETURN_ERROR;

    AMI_INFO("AMITCP: KILL -- taking every interface down");

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        if (netstack_interface_down(i) != AMI_NET_OK)
            worst = RETURN_WARN;
    }

    return worst;
}

/*
 * One command line. Mirrors AmiTCP's parseline(): an empty line is not an
 * error, an unreadable one is a syntax error, an unrecognised keyword is
 * "Unknown command", and anything else dispatches on the keyword index.
 */
static LONG ami_rx_execute(const char *line, const char **errstr,
                           AmiRxReply *reply)
{
    char           cmd[RX_CMDLEN + 2];
    struct CSource cs;
    char           buf[RX_KEYWORDLEN];
    LONG           item;
    LONG           key;
    ULONG          len = 0;

    *errstr = NULL;

    while (line[len] != '\0' && len < RX_CMDLEN)
    {
        cmd[len] = line[len];
        len++;
    }
    if (line[len] != '\0')
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_WARN;
    }

    /* ReadItem() wants a sentinel it can stop on; AmiTCP uses '\n' too, but
       writes it into the caller's argstring. This is our copy. */
    cmd[len]     = '\n';
    cmd[len + 1] = '\0';

    cs.CS_Buffer = (STRPTR)cmd;
    cs.CS_Length = (LONG)len + 1;
    cs.CS_CurChr = 0;

    item = ReadItem((STRPTR)buf, (LONG)sizeof(buf), &cs);
    if (item == 0)
        return RETURN_OK;           /* empty line: AmiTCP returns OK as well */
    if (item < 0)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_WARN;
    }

    key = FindArg((CONST_STRPTR)ami_rx_keywords, (CONST_STRPTR)buf);
    if (key < 0)
    {
        *errstr = ami_rx_err_unknown;
        return RETURN_WARN;
    }

    if (ami_netstack_raw() == NULL)
    {
        *errstr = ami_rx_err_state;
        return RETURN_ERROR;
    }

    switch (key)
    {
        case RX_KEY_QUERY:
            return ami_rx_getvalue(&cs, errstr, reply);

        case RX_KEY_SET:
            return ami_rx_setvalue(&cs, errstr, reply);

        case RX_KEY_KILL:
            return ami_rx_kill();

        /*
         * READ, ROUTE, ADD and RESET. Two of them AmiTCP never implemented
         * either: readfile() returned "readfile() is currently unimplemented"
         * and parseroute() "ROUTE not implemented." with the working body
         * #if 0'ed out around an INCOMPLETE marker. ADD and RESET edit the net
         * database AmiTCP kept in memory (its kern/amiga_netdb.c), which this
         * stack does not have -- docs/DEVELOPMENT.md sizes what adding one
         * would mean.
         *
         * Recognised so the caller is told "not implemented" rather than
         * "unknown command", which is the difference between a stack that has
         * no such feature and a stack that has never heard of it.
         */
        case RX_KEY_READ:
        case RX_KEY_ROUTE:
        case RX_KEY_ADD:
        case RX_KEY_RESET:
        default:
            *errstr = ami_rx_err_unimpl;
            return RETURN_ERROR;
    }
}

/* -------------------------------------------------------- message handling -- */

/*
 * One RexxMsg, answered. rm_Result1 is the return code the script sees as RC;
 * rm_Result2 is a result string, but only when the script asked for one with
 * OPTIONS RESULTS -- creating one otherwise leaks an argstring the interpreter
 * will not free.
 */
static VOID ami_rx_service(struct RexxMsg *rmsg)
{
    char        buffer[RX_REPLYBUFLEN + 1];
    AmiRxReply  reply;
    const char *errstr = NULL;
    const char *line;
    LONG        rc;

    line = (const char *)ARG0(rmsg);
    if (line == NULL)
        line = "";

    rmsg->rm_Result1 = 0;
    rmsg->rm_Result2 = 0;

    ami_rx_reply_init(&reply, (STRPTR)buffer, RX_REPLYBUFLEN);

    rc = ami_rx_execute(line, &errstr, &reply);

    if (rc != RETURN_OK)
    {
        rmsg->rm_Result1 = rc;
        if (errstr != NULL && (rmsg->rm_Action & RXFF_RESULT) != 0)
            rmsg->rm_Result2 =
                (LONG)CreateArgstring((STRPTR)errstr, ami_rx_strlen(errstr));
    }
    else if ((rmsg->rm_Action & RXFF_RESULT) != 0)
    {
        /*
         * Even when the reply is empty, which is AmiTCP's behaviour and matters:
         * `QUERY CONNECTIONS` on a machine with no sockets succeeds with nothing
         * to say, and a script that reads RESULT afterwards must see "" rather
         * than the uninitialised variable's own name.
         */
        /* STRPTR, not CONST_STRPTR: the pinned NDK types this parameter
           `const STRPTR`, which is UBYTE *const -- a const pointer, not a
           pointer to const -- so CONST_STRPTR discards a qualifier there and
           builds only against an NDK that spells it CONST_STRPTR. The reply is
           our own buffer, so handing it over non-const costs nothing. */
        rmsg->rm_Result2 =
            (LONG)CreateArgstring(reply.rr_Buffer, (LONG)reply.rr_Used);
    }

    /* After CreateArgstring(), which copies: an error string may point into the
       reply buffer, so nothing may free it before both are read. */
    ami_rx_reply_done(&reply);

    ReplyMsg((struct Message *)rmsg);
}

/*
 * Everything queued, replied to. A message that is not a RexxMsg is replied
 * unchanged: rm_Result1 lies past the end of a plain struct Message, so writing
 * it would scribble on the sender.
 */
static VOID ami_rx_drain(BOOL closing)
{
    struct Message *msg;

    while ((msg = GetMsg(ami_rx_port)) != NULL)
    {
        if (ami_rx_rexxbase == NULL || !IsRexxMsg((struct RexxMsg *)msg))
        {
            ReplyMsg(msg);
            continue;
        }

        if (closing)
        {
            struct RexxMsg *rmsg = (struct RexxMsg *)msg;

            /* AmiTCP's rexx_deinit() values, exactly. It also set LASTERROR
               to "99: Port Closed!" here; that string is the one thing lost by
               not setting the variable at all. */
            rmsg->rm_Result2 = 0;
            rmsg->rm_Result1 = 100;
            ReplyMsg(msg);
            continue;
        }

        ami_rx_service((struct RexxMsg *)msg);
    }
}

/* ----------------------------------------------------------- the process -- */

static VOID ami_rx_main(VOID)
{
    AmiRexxBoot    *boot = ami_rx_boot;
    struct MsgPort *port;
    struct MsgPort *death;
    ULONG           portmask;
    BOOL            running = TRUE;

    port = CreateMsgPort();
    if (port != NULL)
    {
        port->mp_Node.ln_Name = ami_rx_port_name;
        port->mp_Node.ln_Pri  = 0;

        /*
         * Another stack owns the name: leave it alone and start nothing. The
         * check and the AddPort() are one atomic step, or two stacks coming up
         * together could both pass it.
         */
        Forbid();
        if (FindPort((CONST_STRPTR)ami_rx_port_name) != NULL)
        {
            Permit();
            DeleteMsgPort(port);
            port = NULL;
            AMI_WARN("AMITCP: a port of that name already exists; not adding ours");
        }
        else
        {
            AddPort(port);
            Permit();
        }
    }

    ami_rx_port = port;

    if (boot != NULL)
    {
        boot->rb_Ok = (port != NULL) ? TRUE : FALSE;
        Signal(boot->rb_Parent, SIGF_SINGLE);
    }

    if (port == NULL)
        return;

    /*
     * rexxsyslib is what makes a reply meaningful -- without it we can still
     * reply, which is what stops the hang, but not set rm_Result1 (the message
     * cannot be confirmed to be a RexxMsg) or hand back a result string.
     * Nothing can send us a RexxMsg without it either, so this is theoretical.
     */
    ami_rx_rexxbase = OpenLibrary((CONST_STRPTR)"rexxsyslib.library", 0);
    if (ami_rx_rexxbase == NULL)
        AMI_WARN("AMITCP: no rexxsyslib.library; messages will be replied to "
                 "but not interpreted");

    AMI_INFO("AMITCP: ARexx host started");

    portmask = 1UL << port->mp_SigBit;

    while (running)
    {
        ULONG sigs = Wait(portmask | SIGBREAKF_CTRL_C);

        ami_rx_drain(FALSE);

        if ((sigs & SIGBREAKF_CTRL_C) != 0)
            running = FALSE;
    }

    /*
     * Off the public list first so nothing new can find the name, then answer
     * whatever arrived in the meantime.
     */
    Forbid();
    RemPort(port);
    ami_rx_port = NULL;
    Permit();

    ami_rx_drain(TRUE);

    DeleteMsgPort(port);

    if (ami_rx_rexxbase != NULL)
    {
        CloseLibrary(ami_rx_rexxbase);
        ami_rx_rexxbase = NULL;
    }

    AMI_INFO("AMITCP: ARexx host stopped");

    death = ami_rx_death;

    /*
     * The parent is waiting on this message to know the library segment is free
     * of us. Forbid() and no Permit(): the epilogue after this call is still
     * our code, and the parent must not run until this process has left it.
     * The task's Forbid nesting dies with the task.
     */
    Forbid();
    if (death != NULL)
        PutMsg(death, &ami_rx_death_msg);
}

/* ---------------------------------------------------------------- the API -- */

VOID ami_netstack_rexx_start(VOID)
{
    AmiRexxBoot     boot;
    struct TagItem  tags[6];
    struct Task    *me = FindTask(NULL);

    if (ami_rx_proc != NULL)
        return;

    /* CreateNewProc() needs a Process to inherit from, the same requirement
       bsd_tcp_handler_start() has. A bare Task gets no port rather than a
       crash -- and no port is the failure mode that was always safe. */
    if (me == NULL || me->tc_Node.ln_Type != NT_PROCESS)
    {
        AMI_WARN("AMITCP: opener is not a Process; no ARexx host");
        return;
    }

    ami_rx_death = CreateMsgPort();
    if (ami_rx_death == NULL)
        return;

    ami_rx_death_msg.mn_Node.ln_Type = NT_MESSAGE;
    ami_rx_death_msg.mn_ReplyPort    = NULL;
    ami_rx_death_msg.mn_Length       = (UWORD)sizeof(ami_rx_death_msg);

    boot.rb_Parent = me;
    boot.rb_Ok     = FALSE;
    ami_rx_boot    = &boot;

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)ami_rx_main;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"AmiNetXDuo ARexx";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = RX_STACK;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = (ULONG)RX_PRIORITY;
    tags[4].ti_Tag  = NP_Cli;
    tags[4].ti_Data = FALSE;
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    SetSignal(0, SIGF_SINGLE);

    ami_rx_proc = CreateNewProc(tags);
    if (ami_rx_proc == NULL)
    {
        ami_rx_boot = NULL;
        DeleteMsgPort(ami_rx_death);
        ami_rx_death = NULL;
        AMI_ERROR("AMITCP: cannot start the ARexx host process");
        return;
    }

    /* Bounded: the host signals before it does anything that can block, and
       `boot` is on this stack. */
    Wait(SIGF_SINGLE);
    ami_rx_boot = NULL;

    if (!boot.rb_Ok)
    {
        /* It added no port and has already returned. Nothing to stop. */
        ami_rx_proc = NULL;
        DeleteMsgPort(ami_rx_death);
        ami_rx_death = NULL;
    }
}

VOID ami_netstack_rexx_stop(VOID)
{
    if (ami_rx_proc == NULL)
        return;

    Signal((struct Task *)ami_rx_proc, SIGBREAKF_CTRL_C);

    WaitPort(ami_rx_death);
    (VOID)GetMsg(ami_rx_death);

    DeleteMsgPort(ami_rx_death);
    ami_rx_death = NULL;
    ami_rx_proc  = NULL;
}

/*
 * iComp's SANA-II drivers read the AMITCP port as "this stack hands AmiTCP
 * mbufs to the copy callbacks", and on finding it they stop calling the
 * callbacks and walk ios2_Data as an IOIPReq instead -- into our slot, which is
 * not one (docs/RESEARCH.md 71). Their flag is sampled once per OpenDevice and
 * defaults to on, so the port is taken down across the open and put back after.
 *
 * The host process needs no part in this. RemPort() only unlinks the port from
 * Exec's public list: the port structure, its message list and its signal are
 * untouched, so a message already queued is still serviced and a PutMsg()
 * through a pointer someone found earlier still arrives and still signals. The
 * window hides the name from FindPort(), which is the whole point, and changes
 * nothing about delivery -- so there is no state to share and no lock between
 * the suspending task and the host.
 *
 * What does matter is that the port is not *freed* under the host: suspend and
 * resume are bracketed inside one OpenDevice() call, and teardown clears these
 * hooks before ami_netstack_rexx_stop() is called.
 */
VOID ami_netstack_rexx_suspend(VOID)
{
    Forbid();
    if (ami_rx_port != NULL)
        RemPort(ami_rx_port);
    Permit();
}

VOID ami_netstack_rexx_resume(VOID)
{
    Forbid();
    if (ami_rx_port != NULL)
        AddPort(ami_rx_port);
    Permit();
}
