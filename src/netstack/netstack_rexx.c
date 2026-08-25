/*
 * AmiNetXDuo, the AMITCP public port and the ARexx host behind it.  Every
 * message must be replied to, including the commands not implemented here: a
 * port that is present but silent hangs the sender inside RexxSysLib.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * A private rexxsyslib base: the NDK inlines are parameterised for it, and
 * with -fno-common a conventional `RexxSysBase` here collides with anything
 * else that defines one.
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

/* The AmiTCP keyword list and its order.  The enum below has to match. */
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

/* The length of the longest QUERY netstat sends, rounded up. */
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
 * The stop handshake.  It is not a MsgPort: start and stop run on different
 * Tasks, so the stopper allocates the signal in its own task and registers
 * itself, and the host only sets a flag and pokes whatever is registered.
 */
static volatile ULONG   ami_rx_gone;
static struct Task     *ami_rx_stopper;
static ULONG            ami_rx_stop_sig;

/* --------------------------------------------------------------- commands, */

static ULONG ami_rx_strlen(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/*
 * KILL.  ami_shutdown_notify() sends every opener SIGBREAKF_CTRL_C and gives
 * back the reference that keeps the stack standing, so the last CloseLibrary()
 * takes the stack with it.  Then the interfaces go down.
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
     * tidily still has a network to do it over.
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

/* One command line, parsed the way parseline() in AmiTCP parsed it. */
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
         * Recognised so that the caller is told "not implemented" rather than
         * "unknown command".  AmiTCP never implemented READ or ROUTE either,
         * and this stack has no writable net database for ADD and RESET.
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
 * One RexxMsg, answered.  rm_Result2 may be created only when the script asked
 * for a result with OPTIONS RESULTS; creating one otherwise leaks an argstring
 * the interpreter does not free.
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
           the returned pointer.  A 1024-byte command has no byte 1024 we are
           entitled to read before deciding whether it fits. */
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
        /* Even when the reply is empty: a script that reads RESULT afterwards
           must see "" rather than the name of the uninitialised variable. */
        /* STRPTR, not CONST_STRPTR: the pinned NDK types this parameter
           `const STRPTR`, which is a const pointer and not a pointer to const,
           so CONST_STRPTR discards a qualifier there. */
        rmsg->rm_Result2 =
            (LONG)CreateArgstring(reply.rr_Buffer, (LONG)reply.rr_Used);
    }

    /* After CreateArgstring(), which copies: an error string can point into the
       reply buffer, so nothing must free it before both are read. */
    ami_rx_reply_done(&reply);

    ReplyMsg((struct Message *)rmsg);
}

/*
 * Everything queued, replied to.  A message that is not a RexxMsg is replied
 * unchanged: rm_Result1 lies past the end of a plain struct Message.  The port
 * is a parameter: the closing drain runs after RemPort() cleared the global.
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

            /* The rexx_deinit() values of AmiTCP, exactly. */
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
     * Without rexxsyslib the host still replies, which is what stops the hang,
     * but cannot set rm_Result1 or hand back a result string.
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
     * Forbid() and no Permit(): the epilogue after this call is still code in
     * the library segment, and the stopper must not run until this process has
     * left it.  The Forbid nesting of the task dies with the task.
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

    /* Allocated here so the bit belongs to the task that waits on it.  Not
       SIGF_SINGLE: the caller can be an adopted ThreadX thread, and the port
       uses SIGF_SINGLE as the thread run-signal. */
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
 * The iComp SANA-II drivers change behaviour when they find the AMITCP port,
 * so it is hidden across every SANA-II OpenDevice.  RemPort() only unlinks the
 * name: a queued message is still serviced and a PutMsg() still arrives.
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
