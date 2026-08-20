/*
 * AmiNetXDuo, the AMITCP public port, and the ARexx host behind it.
 *
 * The port exists so that `WaitForPort AMITCP` in a startup script returns once
 * the stack is up. It is also the ARexx host port of AmiTCP, which is what this
 * file is for. 31 of the 2,149 comm/ archives surveyed in docs/RESEARCH.md 75
 * send commands to it: the AmiTCP bin/stopnet and bin/netstat, the Genesis
 * copies of both, SLIPCall, Netdial, TCPFront, netbeginner, interinstall,
 * TCP_Start_Stop, rx.fingerd.
 *
 * A port with PA_IGNORE and no signal task answers none of them. RexxSysLib
 * finds the port, PutMsg()es a RexxMsg and waits for a reply that never comes,
 * so the script hangs. With no port at all it failed at once with "host
 * environment not found". So the port has to be serviced and every message
 * replied to, including the commands this file does not implement. An error
 * reply restores the behaviour the absence of the port used to give.
 *
 * The command set is the AmiTCP 3.0b2 set, from src/amitcp/kern/amiga_config.h:
 *
 *     #define REXXKEYWORDS "Q=QUERY,S=SET,READ,ROUTE,ADD,RESET,KILL"
 *
 * parsed the way AmiTCP parsed it, ReadItem() then FindArg(), so abbreviation
 * and quoting behave identically without a reimplementation here. What each
 * keyword does, and why, is at ami_rx_execute(). netstack_rexx_vars.c has the
 * variable space for QUERY and SET.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * A private rexxsyslib base, the way src/tls/tls_amiga.c takes a private
 * TimerBase: the NDK inlines are parameterised for it, and with -fno-common a
 * conventional `RexxSysBase` here collides with anything else that defines one.
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

/* ----------------------------------------------------------------- names, */

static char ami_rx_port_name[]  = "AMITCP";

/*
 * The AmiTCP keyword list and its order. The enum has to match.
 * "ROUTE is currently not implemented" is the AmiTCP comment on its line 95.
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
   variable space shares. The AmiTCP command line was one CONFIGLINELEN at most.
   This is the length of the longest QUERY netstat sends, rounded up. */
#define RX_CMDLEN       1024

/* One command line (RX_CMDLEN) and one reply (RX_REPLYBUFLEN) are both on this
   stack while a QUERY runs, and the netstat TCP query fills most of both. */
#define RX_STACK        8192
#define RX_PRIORITY     0

/* ----------------------------------------------------------------- state, */

typedef struct AmiRexxBoot
{
    struct Task *rb_Parent;
    BOOL         rb_Ok;
} AmiRexxBoot;

static struct MsgPort  *ami_rx_port;        /* the AMITCP port itself       */
static struct Process  *ami_rx_proc;
static AmiRexxBoot     *ami_rx_boot;

/*
 * The stop handshake. It is not a MsgPort.
 *
 * The host starts from netstack_startup() and stops from netstack_shutdown(),
 * and those two run on different Tasks: bsdsocket brings the stack up on a
 * throwaway Process (library.c, bsd_netstack_bringup) and tears it down on
 * whichever task drops the last reference. A MsgPort cannot carry a handshake
 * across that. CreateMsgPort() takes mp_SigBit from the signal allocation of
 * the creating task and points mp_SigTask at it, so the reply woke the bring-up
 * Process, gone by then, while the stopper sat in WaitPort() on a bit that
 * nothing ever set. That was CloseLibrary() never returning, and
 * DeleteMsgPort() also frees the signal out of the wrong task.
 *
 * So the stopper supplies both. It allocates the signal in its own task and
 * registers itself, and the host only sets a flag and pokes whatever is
 * registered. The same shape as _tx_amiga_stop_notify() in the port.
 */
static volatile ULONG   ami_rx_gone;
static struct Task     *ami_rx_stopper;
static ULONG            ami_rx_stop_sig;

/* ------------------------------------------------------- rexxsyslib calls,
 *
 * AmiTCP also set an ARexx variable, AMITCP.LASTERROR, alongside the return
 * code. That is not done here. SetRexxVarFromMsg() is the only way to reach it
 * from a shared library. The SetRexxVar() in amiga.lib needs a base this file
 * does not have, and only some NDKs declare it, so building against it makes
 * the result depend on which NDK is installed.
 *
 * Nothing in the 31-archive corpus reads LASTERROR (docs/RESEARCH.md 75.7). A
 * script reads RC as rm_Result1, which is set, and an error string comes back
 * in rm_Result2 when the caller asked for a result.
 */

/* --------------------------------------------------------------- commands, */

static ULONG ami_rx_strlen(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/*
 * KILL. In AmiTCP this was Signal(AmiTCP_Task, SIGBREAKF_CTRL_C): the stack
 * task exited and the library went with it. This does the same thing by the
 * route a singleton library allows. ami_shutdown_notify() sends every opener
 * that same SIGBREAKF_CTRL_C and gives back the reference that keeps the stack
 * standing, so the last CloseLibrary() takes the stack with it.
 *
 * Then the interfaces go down, which is all this used to do. Doing only that
 * left `httpd` and everything else running against a network that had gone,
 * the same defect NetShutdown was fixed for, because the two shared a
 * description and not an implementation. They still do not share one.
 * NetShutdown waits out a grace period and reports who did not let go, and
 * this cannot, because an ARexx handler that blocks for five seconds blocks
 * the port. What they share is the pair of operations.
 *
 * The port stays, and so does the library until its last opener leaves. The
 * optional FLUSH branch in `stopnet` tests Show(ports, AMITCP) before it
 * flushes memory. While an opener remains it declines, because the stack is
 * still resident.
 */
static LONG ami_rx_kill(void)
{
    AmiNetStack *ns = ami_netstack_raw();
    UWORD        i;
    LONG         worst = RETURN_OK;

    if (ns == NULL)
        return RETURN_ERROR;

    AMI_INFO("AMITCP: KILL, telling the programs using the network and "
             "taking every interface down");

    /*
     * Before the interfaces, so a program that wants to close a connection
     * tidily still has a network to do it over, and because this is the half
     * that cannot be undone by an Online afterwards.
     */
    ami_shutdown_notify();

    for (i = 0; i < ns->ns_IfaceCount; i++)
    {
        /* Runtime removal leaves a stable-index hole below ns_IfaceCount.
           It is already down; only a live slot can fail this operation. */
        if (ns->ns_Iface[i] != NULL &&
            netstack_interface_down(i) != AMI_NET_OK)
            worst = RETURN_WARN;
    }

    return worst;
}

/*
 * One command line. Mirrors parseline() in AmiTCP: an empty line is not an
 * error, an unreadable one is a syntax error, an unrecognised keyword is
 * "Unknown command", and anything else dispatches on the keyword index.
 */
static LONG ami_rx_execute(const char *line, ULONG line_size,
                           const char **errstr, AmiRxReply *reply)
{
    char           cmd[RX_CMDLEN + 2];
    struct CSource cs;
    char           buf[RX_KEYWORDLEN];
    LONG           item;
    LONG           key;
    ULONG          len = 0;

    *errstr = NULL;

    if (line_size > RX_CMDLEN)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_WARN;
    }

    while (len < line_size && line[len] != '\0')
    {
        cmd[len] = line[len];
        len++;
    }

    /* ReadItem() wants a sentinel it can stop on. AmiTCP uses '\n' as well, but
       writes it into the argstring of the caller. This is a private copy. */
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
         * READ, ROUTE, ADD and RESET. AmiTCP never implemented two of them
         * either: readfile() returned "readfile() is currently unimplemented"
         * and parseroute() "ROUTE not implemented.", with the working body
         * #if 0'ed out around an INCOMPLETE marker. ADD and RESET edit the net
         * database AmiTCP kept in memory (kern/amiga_netdb.c), which this
         * stack does not have: src/config/netdb.c is immutable after
         * ami_netdb_load(), which is why it needs no lock.
         *
         * Recognised so that the caller is told "not implemented" rather than
         * "unknown command".
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

/* -------------------------------------------------------- message handling, */

/*
 * One RexxMsg, answered. rm_Result1 is the return code the script sees as RC.
 * rm_Result2 is a result string, but only when the script asked for one with
 * OPTIONS RESULTS. Creating one otherwise leaks an argstring the interpreter
 * does not free.
 */
static VOID ami_rx_service(struct RexxMsg *rmsg)
{
    char        buffer[RX_REPLYBUFLEN + 1];
    AmiRxReply  reply;
    const char *errstr = NULL;
    const char *line;
    ULONG       line_size;
    LONG        rc;

    line = (const char *)ARG0(rmsg);
    if (line == NULL)
    {
        line = "";
        line_size = 0;
    }
    else
    {
        /* An ARexx argstring carries its length in the allocation in front of
           the returned pointer.  Use that bound instead of searching beyond
           RX_CMDLEN for a terminator: a 1024-byte command has no byte 1024 we
           are entitled to read before deciding whether it fits. */
        line_size = LengthArgstring((UBYTE *)line);
    }

    rmsg->rm_Result1 = 0;
    rmsg->rm_Result2 = 0;

    ami_rx_reply_init(&reply, (STRPTR)buffer, RX_REPLYBUFLEN);

    rc = ami_rx_execute(line, line_size, &errstr, &reply);

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
         * Even when the reply is empty, which is the AmiTCP behaviour.
         * `QUERY CONNECTIONS` on a machine with no sockets succeeds with
         * nothing to say, and a script that reads RESULT afterwards must see
         * "" rather than the name of the uninitialised variable.
         */
        /* STRPTR, not CONST_STRPTR: the pinned NDK types this parameter
           `const STRPTR`, which is UBYTE *const, a const pointer, not a
           pointer to const, so CONST_STRPTR discards a qualifier there and
           builds only against an NDK that spells it CONST_STRPTR. The reply is
           a local buffer, so handing it over non-const costs nothing. */
        rmsg->rm_Result2 =
            (LONG)CreateArgstring(reply.rr_Buffer, (LONG)reply.rr_Used);
    }

    /* After CreateArgstring(), which copies: an error string can point into the
       reply buffer, so nothing must free it before both are read. */
    ami_rx_reply_done(&reply);

    ReplyMsg((struct Message *)rmsg);
}

/*
 * Everything queued, replied to. A message that is not a RexxMsg is replied
 * unchanged: rm_Result1 lies past the end of a plain struct Message, so writing
 * it corrupts the sender.
 */
/*
 * The port is a parameter, and is not read from ami_rx_port. The closing drain
 * runs after RemPort() has already cleared the global, so reading it there was
 * GetMsg(NULL): two Enforcer hits, because mp_MsgList of a NULL MsgPort is at
 * offset 0x14 and then through the head at 0. The drain that is supposed to
 * answer whatever arrived between RemPort() and here answered nothing. A
 * sender that got its message in during that window waited for a reply that
 * never came, which is the hang the header comment says this code prevents.
 * Found by tests/stack under Enforcer.
 */
static VOID ami_rx_drain(struct MsgPort *port, BOOL closing)
{
    struct Message *msg;

    if (port == NULL)
        return;

    while ((msg = GetMsg(port)) != NULL)
    {
        if (ami_rx_rexxbase == NULL || !IsRexxMsg((struct RexxMsg *)msg))
        {
            ReplyMsg(msg);
            continue;
        }

        if (closing)
        {
            struct RexxMsg *rmsg = (struct RexxMsg *)msg;

            /* The rexx_deinit() values of AmiTCP, exactly. It also set
               LASTERROR to "99: Port Closed!" here. That string is the one
               thing lost by not setting the variable at all. */
            rmsg->rm_Result2 = 0;
            rmsg->rm_Result1 = 100;
            ReplyMsg(msg);
            continue;
        }

        ami_rx_service((struct RexxMsg *)msg);
    }
}

/* ----------------------------------------------------------- the process, */

static VOID ami_rx_main(VOID)
{
    AmiRexxBoot    *boot = ami_rx_boot;
    struct MsgPort *port;
    ULONG           portmask;
    BOOL            running = TRUE;

    port = CreateMsgPort();
    if (port != NULL)
    {
        port->mp_Node.ln_Name = ami_rx_port_name;
        port->mp_Node.ln_Pri  = 0;

        /*
         * Another stack owns the name: leave it alone and start nothing. The
         * check and the AddPort() are one atomic step, because two stacks
         * coming up together can otherwise both pass it.
         */
        Forbid();
        if (FindPort((CONST_STRPTR)ami_rx_port_name) != NULL)
        {
            Permit();
            DeleteMsgPort(port);
            port = NULL;
            AMI_WARN("AMITCP: a port of that name already exists. "
                     "Ours is not added");
        }
        else
        {
            AddPort(port);
            /* Published and recorded in one step: ami_netstack_rexx_suspend()
               reads ami_rx_port under this same Forbid, and it silently
               declines to RemPort() a port that is on the Exec list while the
               global is still NULL. */
            ami_rx_port = port;
            Permit();
        }
    }

    if (port == NULL)
        ami_rx_port = NULL;

    if (boot != NULL)
    {
        boot->rb_Ok = (port != NULL) ? TRUE : FALSE;
        Signal(boot->rb_Parent, SIGF_SINGLE);
    }

    if (port == NULL)
        return;

    /*
     * rexxsyslib is what makes a reply meaningful. Without it the host still
     * replies, which is what stops the hang, but cannot set rm_Result1 (the
     * message cannot be checked to be a RexxMsg) or hand back a result string.
     * Nothing can send a RexxMsg without it either, so this is theoretical.
     */
    ami_rx_rexxbase = OpenLibrary((CONST_STRPTR)"rexxsyslib.library", 0);
    if (ami_rx_rexxbase == NULL)
        AMI_WARN("AMITCP: no rexxsyslib.library. Messages will be replied to "
                 "but not interpreted");

    AMI_INFO("AMITCP: ARexx host started");

    portmask = 1UL << port->mp_SigBit;

    while (running)
    {
        ULONG sigs = Wait(portmask | SIGBREAKF_CTRL_C);

        ami_rx_drain(port, FALSE);

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

    ami_rx_drain(port, TRUE);

    DeleteMsgPort(port);

    if (ami_rx_rexxbase != NULL)
    {
        CloseLibrary(ami_rx_rexxbase);
        ami_rx_rexxbase = NULL;
    }

    AMI_INFO("AMITCP: ARexx host stopped");

    /*
     * The stopper waits on this to know the library segment is free of this
     * process. Forbid() and no Permit(): the epilogue after this call is still
     * code in that segment, and the stopper must not run until this process has
     * left it. The Forbid nesting of the task dies with the task.
     */
    Forbid();
    ami_rx_gone = 1UL;
    if (ami_rx_stopper != NULL)
        Signal(ami_rx_stopper, ami_rx_stop_sig);
}

/* ---------------------------------------------------------------- the API, */

VOID ami_netstack_rexx_start(VOID)
{
    AmiRexxBoot     boot;
    struct TagItem  tags[6];
    struct Task    *me = FindTask(NULL);

    if (ami_rx_proc != NULL)
        return;

    /* CreateNewProc() needs a Process to inherit from, the same requirement
       bsd_tcp_handler_start() has. A bare Task gets no port rather than a
       crash, and no port is the safe failure mode. */
    if (me == NULL || me->tc_Node.ln_Type != NT_PROCESS)
    {
        AMI_WARN("AMITCP: opener is not a Process. There is no ARexx host");
        return;
    }

    ami_rx_gone    = 0UL;
    ami_rx_stopper = NULL;

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
    }
}

VOID ami_netstack_rexx_stop(VOID)
{
    BYTE  sig;
    ULONG mask;

    if (ami_rx_proc == NULL)
        return;

    /* Allocated here so the bit belongs to the task that waits on it. Not
       SIGF_SINGLE: this runs from netstack_shutdown(), whose caller can still
       be an adopted ThreadX thread, and the port uses SIGF_SINGLE as the thread
       run-signal. */
    sig  = (BYTE)AllocSignal(-1);
    mask = (sig >= 0) ? (1UL << (ULONG)sig) : 0UL;

    /* Register before the break, so the host cannot finish and find nobody. */
    Forbid();
    ami_rx_stopper  = FindTask(NULL);
    ami_rx_stop_sig = mask;
    Signal((struct Task *)ami_rx_proc, SIGBREAKF_CTRL_C);
    Permit();

    while (ami_rx_gone == 0UL)
    {
        if (mask != 0UL)
            (VOID)Wait(mask);
        else
            Delay(1UL);         /* no signal to spare: poll instead */
    }

    Forbid();
    ami_rx_stopper  = NULL;
    ami_rx_stop_sig = 0UL;
    Permit();

    if (sig >= 0)
        FreeSignal(sig);

    ami_rx_proc = NULL;
}

/*
 * The iComp SANA-II drivers read the AMITCP port as "this stack hands AmiTCP
 * mbufs to the copy callbacks". On finding it they stop calling the callbacks
 * and walk ios2_Data as an IOIPReq instead, into a slot that is not one
 * (docs/RESEARCH.md 71). Their flag is sampled once per OpenDevice and defaults
 * to on, so the port is taken down across the open and put back after.
 *
 * The host process needs no part in this. RemPort() only unlinks the port from
 * the public list of Exec: the port structure, its message list and its signal
 * are untouched, so a message already queued is still serviced, and a PutMsg()
 * through a pointer found earlier still arrives and still signals. The window
 * hides the name from FindPort() and changes nothing about delivery, so there
 * is no state to share and no lock between the suspending task and the host.
 *
 * The port must not be freed under the host. Suspend and resume are bracketed
 * inside one OpenDevice() call, and teardown clears these hooks before
 * ami_netstack_rexx_stop() is called.
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
