/*
 * CycleDrill: open/expunge/reopen and interface Online/Offline repeatedly,
 * reading the counters between every cycle to expose drift.
 *
 * Phase E must run FIRST and cold: bsd_lib_expunge() only proceeds at
 * lib_OpenCnt 0, and AddNetInterface deliberately leaks an OpenLibrary()
 * reference, so after that no expunge is ever reachable again.
 *
 * Vectors are called by hand at the LVOs the ABI assigns: the NDK inlines
 * assume a global SocketBase and this program holds several bases at once.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/timer.h>      /* struct timeval, for WaitSelect */
#include <utility/tagitem.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them: these must precede it. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: CycleDrill 1.0 (31.7.2026)";

#define TEMPLATE    "CYCLES/N,EXPUNGE/N,IFACE/K,SOCKETS/N,REPORT/S"

enum
{
    ARG_CYCLES = 0,
    ARG_EXPUNGE,
    ARG_IFACE,
    ARG_SOCKETS,
    ARG_REPORT,
    ARG_COUNT
};

#define DEF_CYCLES      3
#define DEF_EXPUNGE     2
#define DEF_SOCKETS     2
#define DEF_IFACE       "eth0"

/* Nested OpenLibrary()s per cycle.  Hard ceiling: each base costs the calling
   task signal bits out of the 32 a Task has, so more than four nested bases
   doing a timed WaitSelect() runs this Process out and OpenLibrary() refuses. */
#define NEST_OPENS      4

#define MAX_CYCLES      16
#define MAX_IFACES      4
#define MAX_SOCKETS     8

#define LIB_NAME        "bsdsocket.library"

#define P_AF_INET       2
#define P_SOCK_STREAM   1
#define P_SOCK_DGRAM    2

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

/* ------------------------------------------------------------- vectors ---- */

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno -0x0a2 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"      /* socket -0x01e */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"     /* CloseSocket -0x078 */
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_getsockname(struct Library *base, LONG s, ProbeAddr *name,
                          LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)name;
    register APTR            a1  __asm("a1") = (APTR)len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"     /* getsockname -0x066 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

/* WaitSelect(nfds, read, write, except, timeout, signals). */
static LONG p_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                         struct timeval *tv)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register APTR            d1  __asm("d1") = NULL;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");
    register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"     /* WaitSelect -0x07e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

static LONG p_query(struct Library *base, ULONG what, APTR buffer, ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = what;
    register APTR            a0  __asm("a0") = buffer;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-870:W)"     /* AMI_NETSTATUS_QUERY_LVO   */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_control(struct Library *base, ULONG op, NetStatusControl *ctl)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = (APTR)ctl;
    register ULONG           d2  __asm("d2") = (ULONG)sizeof(*ctl);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"     /* AMI_NETSTATUS_CONTROL_LVO */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_add_interface(struct Library *base, const char *name,
                            const char *device, LONG unit,
                            struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)device;
    register LONG            d0  __asm("d0") = unit;
    register APTR            a2  __asm("a2") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-444:W)"     /* AddInterfaceTagList -0x1bc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_configure_interface(struct Library *base, const char *name,
                                  struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            a1  __asm("a1") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-450:W)"     /* ConfigureInterfaceTagList -0x1c2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG p_remove_interface(struct Library *base, const char *name,
                               LONG force)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register LONG            d0  __asm("d0") = force;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-732:W)"     /* RemoveInterface -0x2dc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "a1", "cc", "memory");
    return res;
}

/* The LVOs above are literals because `jsr a6@(-870:W)` needs one; check them
   against the header so a moved slot cannot land on whatever is next. */
_Static_assert(AMI_NETSTATUS_QUERY_LVO   == -870, "NetStackQuery LVO moved");
_Static_assert(AMI_NETSTATUS_CONTROL_LVO == -876, "NetStackControl LVO moved");

/* ------------------------------------------------------------- reporting -- */

static LONG checks;
static LONG failures;

static VOID say(const char *fmt, LONG a, LONG b, LONG c, LONG d)
{
    LONG args[4];

    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;

    VPrintf((CONST_STRPTR)fmt, (APTR)args);     /* (APTR): see tool_util.c */
}

static BOOL check(BOOL ok, const char *what, LONG a, LONG b)
{
    checks++;
    if (!ok)
        failures++;

    say(ok ? "  ok: %s (%ld, %ld)\n" : "FAIL: %s (%ld, %ld)\n",
        (LONG)what, a, b, 0);

    return ok;
}

/* -------------------------------------------------------------- sampling -- */

typedef struct Sample
{
    BOOL    ok;
    ULONG   alloc_live;
    ULONG   alloc_peak;
    ULONG   alloc_refused;
    ULONG   sockets;
    ULONG   sockets_peak;
    ULONG   opens;
    ULONG   pool_total;
    ULONG   pool_free;
    ULONG   pool_low;
    ULONG   pool_empty;
    ULONG   free_mem;       /* from Exec, not the library */
    ULONG   sigs;           /* signal bits held by this Process */
    ULONG   tasks;          /* Tasks and Processes on the system lists */
} Sample;

static struct
{
    NetStatusHeader hdr;
    NetStatusHealth e;
} q_health;

static struct
{
    NetStatusHeader     hdr;
    NetStatusInterface  e[MAX_IFACES];
} q_ifaces;

static VOID zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- != 0)
        *b++ = 0;
}

static ULONG own_signals(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        mask;
    ULONG        n = 0;

    if (me == NULL)
        return 0;

    for (mask = me->tc_SigAlloc; mask != 0; mask >>= 1)
    {
        if (mask & 1UL)
            n++;
    }

    return n;
}

/*
 * Disable(), not Forbid(): Forbid() does not stop interrupts, and Signal()
 * from one moves a node between TaskWait and TaskReady mid-walk, so the count
 * would be wrong.  The running task is on neither list, so this is one short.
 */
static ULONG count_tasks(VOID)
{
    struct Node *n;
    ULONG        total = 0;

    Disable();
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        total++;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        total++;
    Enable();

    return total;
}

/* Copied under Disable() and printed after: VPrintf() cannot run with
   interrupts off. */
#define TD_MAX      48
#define TD_NAME     28

static char  td_name[TD_MAX][TD_NAME];
static UWORD td_held;

static VOID names_of(struct Node *head)
{
    struct Node *n;

    for (n = head; n->ln_Succ != NULL && td_held < TD_MAX; n = n->ln_Succ)
    {
        const char *s = (const char *)n->ln_Name;
        UWORD       i = 0;

        if (s != NULL)
        {
            while (s[i] != '\0' && i < (TD_NAME - 1))
            {
                td_name[td_held][i] = s[i];
                i++;
            }
        }
        td_name[td_held][i] = '\0';
        td_held++;
    }
}

static VOID dump_tasks(const char *label)
{
    UWORD i;

    td_held = 0;

    Disable();
    names_of(SysBase->TaskReady.lh_Head);
    names_of(SysBase->TaskWait.lh_Head);
    Enable();

    for (i = 0; i < td_held; i++)
    {
        say("  task %s: %s\n", (LONG)label,
            (LONG)(td_name[i][0] != '\0' ? td_name[i] : "(unnamed)"), 0, 0);
    }
}

static VOID settle(VOID)
{
    Delay(50);
}

/*
 * `base` NULL means the library is not open right now: only the Exec numbers
 * are read.  Struct assignment rather than zero(): -fanalyzer does not follow
 * a byte loop and reports every field the caller reads as uninitialised.
 */
static VOID sample(struct Library *base, Sample *out)
{
    static const Sample empty;

    *out = empty;

    out->free_mem = AvailMem(MEMF_ANY);
    out->sigs     = own_signals();
    out->tasks    = count_tasks();

    if (base == NULL)
        return;

    zero(&q_health, sizeof(q_health));
    q_health.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    q_health.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    if (p_query(base, NETSTATUS_HEALTH, &q_health, sizeof(q_health)) <= 0)
        return;

    out->ok            = TRUE;
    out->alloc_live    = q_health.e.nsl_AllocLive;
    out->alloc_peak    = q_health.e.nsl_AllocPeak;
    out->alloc_refused = q_health.e.nsl_AllocRefused;
    out->sockets       = q_health.e.nsl_Sockets;
    out->sockets_peak  = q_health.e.nsl_SocketsPeak;
    out->opens         = q_health.e.nsl_Opens;
    out->pool_total    = q_health.e.nsl_PoolTotal;
    out->pool_free     = q_health.e.nsl_PoolFree;
    out->pool_low      = q_health.e.nsl_PoolLow;
    out->pool_empty    = q_health.e.nsl_PoolEmpty;
}

static VOID phase_query_counts(struct Library *base)
{
    NetStatusHeader interfaces;
    NetStatusHeader dhcp;

    zero(&interfaces, sizeof(interfaces));
    interfaces.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    interfaces.nsh_Version = AMI_NETSTATUS_VERSION;

    zero(&dhcp, sizeof(dhcp));
    dhcp.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    dhcp.nsh_Version = AMI_NETSTATUS_VERSION;

    (VOID)p_query(base, NETSTATUS_INTERFACES, &interfaces,
                  (ULONG)sizeof(interfaces));
    (VOID)p_query(base, NETSTATUS_DHCP, &dhcp, (ULONG)sizeof(dhcp));

    check(interfaces.nsh_Available != 0 &&
          dhcp.nsh_Available == interfaces.nsh_Available,
          "a header-only DHCP query counts every interface",
          (LONG)dhcp.nsh_Available, (LONG)interfaces.nsh_Available);
}

static VOID show(const char *label, LONG n, const Sample *s)
{
    say("%s %ld: alloc %lu peak %lu\n",
        (LONG)label, n, (LONG)s->alloc_live, (LONG)s->alloc_peak);
    say("           refused %lu sockets %lu peak %lu opens %lu\n",
        (LONG)s->alloc_refused, (LONG)s->sockets, (LONG)s->sockets_peak,
        (LONG)s->opens);
    say("           pool %lu of %lu low %lu empty %lu\n",
        (LONG)s->pool_free, (LONG)s->pool_total, (LONG)s->pool_low,
        (LONG)s->pool_empty);
    say("           free %lu sigs %lu tasks %lu\n",
        (LONG)s->free_mem, (LONG)s->sigs, (LONG)s->tasks, 0);
}

/* ------------------------------------------------------------ the library -- */

static struct Library *lib_find(VOID)
{
    struct Library *lib;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, (STRPTR)LIB_NAME);
    Permit();

    return lib;
}

static BOOL tcp_present(VOID)
{
    struct DosList *dl;
    BOOL            found;

    dl    = LockDosList(LDF_DEVICES | LDF_READ);
    found = (FindDosEntry(dl, (STRPTR)"TCP", LDF_DEVICES) != NULL);
    UnLockDosList(LDF_DEVICES | LDF_READ);

    return found;
}

static LONG tcp_die(VOID)
{
    struct DosList *dl;
    struct MsgPort *port = NULL;

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    dl = FindDosEntry(dl, (STRPTR)"TCP", LDF_DEVICES);
    if (dl != NULL)
        port = dl->dol_Task;
    UnLockDosList(LDF_DEVICES | LDF_READ);

    if (port == NULL)
        return -1;

    return DoPkt(port, ACTION_DIE, 0, 0, 0, 0, 0) ? 1 : 0;
}

/* No Forbid() around RemLibrary(): the expunge vector calls FreeMem() and
   CloseLibrary(), which break a Forbid inside it anyway. */
static BOOL lib_expunge(ULONG *held)
{
    struct Library *lib = lib_find();

    *held = 0;
    if (lib == NULL)
        return TRUE;            /* already gone */

    *held = lib->lib_OpenCnt;
    RemLibrary(lib);

    return (lib_find() == NULL) ? TRUE : FALSE;
}

/* --------------------------------------------------------- the interface -- */

typedef struct IfInfo
{
    BOOL    found;
    UWORD   index;
    BOOL    linkup;
    ULONG   address;
    ULONG   netmask;
    char    name[NETSTATUS_NAME_LEN];
    char    device[NETSTATUS_DEVICE_LEN];
    ULONG   unit;
} IfInfo;

/* Interface names are AmigaDOS file names in DEVS:NetInterfaces, so the
   library compares them case-insensitively; match that. */
static BOOL same_name(const char *a, const char *b)
{
    ULONG i;

    for (i = 0; ; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)   return FALSE;
        if (ca == '\0') return TRUE;
    }
}

static VOID copy_str(char *dst, const char *src, ULONG len)
{
    ULONG i;

    for (i = 0; i + 1 < len && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Re-read every time rather than cached: the index changes across a remove and
   re-add. */
static VOID if_look(struct Library *base, const char *want, IfInfo *out)
{
    static const IfInfo empty;      /* see the note in sample() */
    LONG n;
    LONG i;

    *out = empty;

    zero(&q_ifaces, sizeof(q_ifaces));
    q_ifaces.hdr.nsh_Magic   = AMI_NETSTATUS_MAGIC;
    q_ifaces.hdr.nsh_Version = AMI_NETSTATUS_VERSION;

    n = p_query(base, NETSTATUS_INTERFACES, &q_ifaces, sizeof(q_ifaces));
    if (n <= 0)
        return;

    for (i = 0; i < n; i++)
    {
        const NetStatusInterface *nsi = &q_ifaces.e[i];

        if (!(nsi->nsi_Flags & NETSTATUS_IF_NAMED))
            continue;
        if (!same_name(nsi->nsi_Name, want))
            continue;

        out->found   = TRUE;
        out->index   = nsi->nsi_Index;
        /* LINKUP, not ONLINE: NETCTRL_INTERFACE_UP/DOWN reach NX_LINK_ENABLE
           and NX_LINK_DISABLE, which is what LINKUP records.  The SANA-II
           shim's own online flag is a layer below and does not follow in step. */
        out->linkup  = (nsi->nsi_Flags & NETSTATUS_IF_LINKUP) ? TRUE : FALSE;
        out->address = nsi->nsi_Address;
        out->netmask = nsi->nsi_NetMask;
        out->unit    = nsi->nsi_Unit;
        copy_str(out->name, nsi->nsi_Name, sizeof(out->name));
        copy_str(out->device, nsi->nsi_Device, sizeof(out->device));
        return;
    }
}

static VOID dotted(ULONG addr, char *out)
{
    static const char digits[] = "0123456789";
    LONG  shift;
    ULONG pos = 0;

    for (shift = 24; shift >= 0; shift -= 8)
    {
        ULONG v = (addr >> shift) & 0xFFUL;

        if (v >= 100) out[pos++] = digits[v / 100];
        if (v >= 10)  out[pos++] = digits[(v / 10) % 10];
        out[pos++] = digits[v % 10];

        if (shift != 0)
            out[pos++] = '.';
    }

    out[pos] = '\0';
}

/* ----------------------------------------------------------- the phases --- */

static LONG did_opens;
static LONG did_bounces;
static LONG did_roundtrips;
static LONG did_expunges;

static struct Library *nest[NEST_OPENS];
static LONG            socks[MAX_SOCKETS];

/* Side effect, not the wait: a base opens timer.device and takes a signal bit
   out of this Process the first time a caller passes a timeout, and
   CloseLibrary() has to give that bit back. */
static VOID timed_wait(struct Library *base)
{
    struct timeval tv;

    tv.tv_secs  = 0;
    tv.tv_micro = 1000;

    (VOID)p_waitselect(base, 0, NULL, &tv);
}

static VOID phase_opens(struct Library *anchor, ULONG opens_before)
{
    Sample   before;
    Sample   peak;
    Sample   after;
    LONG     i;
    LONG     got = 0;

    sample(anchor, &before);

    for (i = 0; i < NEST_OPENS; i++)
    {
        nest[i] = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
        if (nest[i] != NULL)
        {
            got++;
            did_opens++;
            timed_wait(nest[i]);
        }
    }

    sample(anchor, &peak);

    for (i = 0; i < NEST_OPENS; i++)
    {
        if (nest[i] != NULL)
        {
            CloseLibrary(nest[i]);
            nest[i] = NULL;
        }
    }

    sample(anchor, &after);

    check(got == NEST_OPENS, "every nested OpenLibrary succeeded",
          got, NEST_OPENS);

    check(peak.opens == opens_before + (ULONG)got,
          "nsl_Opens counted the nested opens",
          (LONG)peak.opens, (LONG)(opens_before + (ULONG)got));

    check(after.opens == opens_before,
          "nsl_Opens came back to where it started",
          (LONG)after.opens, (LONG)opens_before);

    check(after.sigs == before.sigs,
          "the nested opens gave every signal bit back",
          (LONG)before.sigs, (LONG)after.sigs);
}

static BOOL phase_bounce(struct Library *base, const char *iface)
{
    NetStatusControl ctl;
    IfInfo           info;
    LONG             rc;
    BOOL             ok = TRUE;

    if_look(base, iface, &info);
    if (!check(info.found, "the interface is there before the bounce", 0, 0))
        return FALSE;

    zero(&ctl, sizeof(ctl));
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index   = info.index;

    rc = p_control(base, NETCTRL_INTERFACE_DOWN, &ctl);
    if_look(base, iface, &info);
    if (!check(rc == 0 && !info.linkup, "Offline took the link down",
               rc, (LONG)info.linkup))
        ok = FALSE;

    zero(&ctl, sizeof(ctl));
    ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Index   = info.index;

    rc = p_control(base, NETCTRL_INTERFACE_UP, &ctl);
    if_look(base, iface, &info);
    if (!check(rc == 0 && info.linkup, "Online brought it back up",
               rc, (LONG)info.linkup))
        ok = FALSE;

    if (ok)
        did_bounces++;

    return ok;
}

static VOID phase_sockets_across_bounce(struct Library *base,
                                        const char *iface, LONG count)
{
    ProbeAddr sa;
    LONG      len;
    Sample    before;
    Sample    after;
    LONG      i;
    LONG      opened = 0;
    LONG      alive  = 0;

    sample(base, &before);

    for (i = 0; i < count; i++)
    {
        socks[i] = p_socket(base, P_AF_INET,
                            (i & 1) ? P_SOCK_STREAM : P_SOCK_DGRAM, 0);
        if (socks[i] >= 0)
            opened++;
    }

    check(opened == count, "the sockets opened", opened, count);

    (VOID)phase_bounce(base, iface);

    for (i = 0; i < count; i++)
    {
        if (socks[i] < 0)
            continue;

        zero(&sa, sizeof(sa));
        len = (LONG)sizeof(sa);
        if (p_getsockname(base, socks[i], &sa, &len) == 0)
            alive++;
    }

    check(alive == opened, "every socket survived the bounce", alive, opened);

    for (i = 0; i < count; i++)
    {
        if (socks[i] >= 0)
            (VOID)p_close(base, socks[i]);
        socks[i] = -1;
    }

    sample(base, &after);

    check(after.sockets == before.sockets,
          "the socket count came back to where it started",
          (LONG)after.sockets, (LONG)before.sockets);
}

/* AddInterfaceTagList() has no address tag, so what comes back is BARE and the
   ConfigureInterfaceTagList() below is required to address it. */
static VOID phase_addremove(struct Library *base, const char *iface,
                            LONG count)
{
    struct TagItem tags[3];
    IfInfo         before;
    IfInfo         after;
    char           addr_text[16];
    char           mask_text[16];
    LONG           rc;
    LONG           i;
    LONG           opened = 0;

    if_look(base, iface, &before);
    if (!check(before.found, "the interface is there before the round trip",
               0, 0))
        return;

    for (i = 0; i < count; i++)
    {
        socks[i] = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        if (socks[i] >= 0)
            opened++;
    }
    check(opened == count, "sockets open over the round trip", opened, count);

    rc = p_remove_interface(base, iface, 0);
    if_look(base, iface, &after);
    check(rc != 0 && !after.found, "RemoveInterface took it away",
          rc, (LONG)after.found);

    tags[0].ti_Tag  = IFA_NumReadRequests;
    tags[0].ti_Data = 16;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_add_interface(base, iface, before.device, (LONG)before.unit, tags);
    check(rc == 0, "AddInterfaceTagList put it back", rc, p_errno(base));

    if_look(base, iface, &after);
    check(after.found && after.address == 0,
          "and it came back bare, as the published API says",
          (LONG)after.found, (LONG)after.address);

    dotted(before.address, addr_text);
    dotted(before.netmask, mask_text);

    tags[0].ti_Tag  = IFC_Address;
    tags[0].ti_Data = (ULONG)addr_text;
    tags[1].ti_Tag  = IFC_NetMask;
    tags[1].ti_Data = (ULONG)mask_text;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_configure_interface(base, iface, tags);
    if_look(base, iface, &after);
    check(rc == 0 && after.address == before.address &&
          after.netmask == before.netmask,
          "the configure restored the address and the mask",
          (LONG)after.address, (LONG)after.netmask);

    for (i = 0; i < count; i++)
    {
        if (socks[i] >= 0)
            (VOID)p_close(base, socks[i]);
        socks[i] = -1;
    }

    if (after.found)
        did_roundtrips++;
}

static VOID phase_guarded_expunge(struct Library *anchor)
{
    struct Library *lib;
    struct Library *tmp;
    Sample          s;
    ULONG           held = 0;
    ULONG           count_before;
    BOOL            gone;

    lib = lib_find();
    if (!check(lib != NULL, "the library is in the system list", 0, 0))
        return;

    count_before = lib->lib_OpenCnt;
    check(count_before > 0, "and this program is holding it open",
          (LONG)count_before, 0);

    gone = lib_expunge(&held);
    lib  = lib_find();

    check(!gone && lib != NULL,
          "RemLibrary declined to expunge a library with an opener",
          (LONG)gone, (LONG)held);

    check(lib != NULL && (lib->lib_Flags & LIBF_DELEXP) != 0,
          "and set LIBF_DELEXP so the last close retries",
          (LONG)(lib ? lib->lib_Flags : 0), 0);

    check(lib != NULL && lib->lib_OpenCnt == count_before,
          "the open count is untouched",
          (LONG)(lib ? lib->lib_OpenCnt : 0), (LONG)count_before);

    (VOID)p_errno(anchor);
    sample(anchor, &s);
    check(s.ok, "and the base we hold still answers NetStackQuery", 0, 0);

    /* Must clear LIBF_DELEXP again by opening once, or the flag survives into
       whatever runs next and its last close attempts an unwanted expunge. */
    tmp = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (tmp != NULL)
        CloseLibrary(tmp);

    lib = lib_find();
    check(lib != NULL && (lib->lib_Flags & LIBF_DELEXP) == 0,
          "an open cleared LIBF_DELEXP again",
          (LONG)(lib ? lib->lib_Flags : 0), 0);
}

/* Nothing else may hold the library: this must run before AddNetInterface has
   ever been called, the only state in which lib_OpenCnt can reach zero. */
static VOID phase_expunge_cycle(LONG n, const char *iface, Sample *at_open,
                                Sample *at_gone)
{
    struct Library *base;
    IfInfo          info;
    ULONG           held = 0;
    LONG            die;
    BOOL            gone;

    base = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (!check(base != NULL, "the library opened", n, 0))
        return;

    did_opens++;

    if_look(base, iface, &info);
    check(info.found, "the stack came up with the interface", n,
          (LONG)info.linkup);

    timed_wait(base);

    sample(base, at_open);

    CloseLibrary(base);

    die  = tcp_die();
    gone = lib_expunge(&held);

    /* A build without AMINETXDUO_TCPDEVICE has no TCP: to take down and
       tcp_die() answers -1; the expunge must still succeed. */
    if (die < 0)
        check(!tcp_present(), "no TCP: on this build, so none to take down",
              die, 0);
    else
        check(die >= 0, "TCP: answered ACTION_DIE", die, 0);
    check(gone, "the library expunged after its last close", (LONG)held, die);

    if (gone)
        did_expunges++;

    sample(NULL, at_gone);
}

/* TCP: must come back for the NEXT opener after an ACTION_DIE that did not
   expunge.  A base is held across the ACTION_DIE on purpose: without one the
   segment reload would hide the defect. */
static VOID phase_tcp_restart(VOID)
{
    struct Library *holder;
    struct Library *next;
    LONG            died;

    holder = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (!check(holder != NULL, "the library opened for the TCP: restart", 0, 0))
        return;

    if (!tcp_present())
    {
        say("  --: no TCP: device; the restart phase does not apply\n",
            0, 0, 0, 0);
        CloseLibrary(holder);
        return;
    }

    check(tcp_present(), "TCP: is there before ACTION_DIE", 0, 0);

    died = tcp_die();
    check(died > 0, "TCP: answered the restart ACTION_DIE", died, 0);
    check(!tcp_present(), "TCP: went away when told to", 0, 0);

    next = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (check(next != NULL, "the library opened again with TCP: down", 0, 0))
    {
        check(tcp_present(),
              "TCP: is back for an opener that arrived after ACTION_DIE", 0, 0);
        CloseLibrary(next);
    }

    CloseLibrary(holder);
}

/* ------------------------------------------------------------------ main -- */

static Sample baseline;
static Sample cyc[MAX_CYCLES];
static Sample exp_open[MAX_CYCLES];
static Sample exp_gone[MAX_CYCLES];
static Sample cold_open[MAX_CYCLES];
static Sample cold_shut[MAX_CYCLES];

int main(VOID)
{
    struct RDArgs  *rda;
    LONG            args[ARG_COUNT];
    struct Library *anchor;
    struct Library *lib;
    const char     *iface   = DEF_IFACE;
    LONG            cycles  = DEF_CYCLES;
    LONG            expunge = DEF_EXPUNGE;
    LONG            nsocks  = DEF_SOCKETS;
    LONG            i;

    zero(args, sizeof(args));
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"CycleDrill");
        return RETURN_FAIL;
    }

    /* Must open nothing: opening bsdsocket.library starts a stack, and what
       this reports is what a NetShutdown LEFT. */
    if (args[ARG_REPORT] != 0)
    {
        say("tcp: %s\n", (LONG)(tcp_present() ? "present" : "absent"),
            0, 0, 0);
        say("library: %s\n",
            (LONG)((lib_find() != NULL) ? "resident" : "gone"), 0, 0, 0);
        say("tasks: %ld\n", (LONG)count_tasks(), 0, 0, 0);
        say("free: %ld\n", (LONG)AvailMem(MEMF_ANY), 0, 0, 0);

        FreeArgs(rda);
        return RETURN_OK;
    }

    if (args[ARG_CYCLES])  cycles  = *(LONG *)args[ARG_CYCLES];
    if (args[ARG_EXPUNGE]) expunge = *(LONG *)args[ARG_EXPUNGE];
    if (args[ARG_SOCKETS]) nsocks  = *(LONG *)args[ARG_SOCKETS];
    if (args[ARG_IFACE])   iface   = (const char *)args[ARG_IFACE];

    if (cycles  < 1)           cycles  = 1;
    if (expunge < 0)           expunge = 0;
    if (nsocks  < 1)           nsocks  = 1;
    if (cycles  > MAX_CYCLES)  cycles  = MAX_CYCLES;
    if (expunge > MAX_CYCLES)  expunge = MAX_CYCLES;
    if (nsocks  > MAX_SOCKETS) nsocks  = MAX_SOCKETS;

    for (i = 0; i < MAX_SOCKETS; i++)
        socks[i] = -1;

    say("CycleDrill: %ld cycles, %ld expunges, %ld sockets, iface %s\n",
        cycles, expunge, nsocks, (LONG)iface);

    /* ---- phase E: expunge and reopen, cold --------------------------------
       Must be first: the only point at which lib_OpenCnt can reach zero. */
    say("\n-- expunge/reopen --\n", 0, 0, 0, 0);

    lib = lib_find();
    check(lib == NULL || lib->lib_OpenCnt == 0,
          "nothing else holds the library yet",
          (LONG)(lib ? lib->lib_OpenCnt : 0), 0);

    for (i = 0; i < expunge; i++)
    {
        phase_expunge_cycle(i + 1, iface, &exp_open[i], &exp_gone[i]);
        show("open ", i + 1, &exp_open[i]);
        show("gone ", i + 1, &exp_gone[i]);
    }

    /* After the expunge phase, so it starts from a fully unloaded library. */
    phase_tcp_restart();

    /* ---- phase L: cold open/close, no expunge ---------------------------- */
    say("\n-- cold open/close --\n", 0, 0, 0, 0);

    for (i = 0; i < expunge; i++)
    {
        struct Library *cold = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);

        if (!check(cold != NULL, "the library opened cold", i + 1, 0))
            break;

        did_opens++;
        timed_wait(cold);
        sample(cold, &cold_open[i]);
        CloseLibrary(cold);
        sample(NULL, &cold_shut[i]);

        show("cold ", i + 1, &cold_open[i]);
        show("shut ", i + 1, &cold_shut[i]);
    }

    /* ---- phase C: cycling under one held reference ------------------------
       The anchor keeps the stack up so the counters accumulate; after an
       expunge they are all zero again and drift cannot be seen. */
    say("\n-- cycling --\n", 0, 0, 0, 0);

    anchor = OpenLibrary((CONST_STRPTR)LIB_NAME, 4UL);
    if (!check(anchor != NULL, "the library opened for the cycling phase",
               0, 0))
    {
        FreeArgs(rda);
        say("\nCycleDrill: %ld check(s), %ld failed\n", checks, failures, 0, 0);
        return RETURN_FAIL;
    }
    did_opens++;

    {
        char   addr_text[16];
        IfInfo info;

        sample(anchor, &baseline);
        phase_query_counts(anchor);
        if_look(anchor, iface, &info);
        dotted(info.address, addr_text);

        check(info.found, "the interface is up before cycling starts", 0, 0);
        say("interface %s: index %ld, %s, address %s\n",
            (LONG)iface, (LONG)info.index,
            (LONG)(info.linkup ? "up" : "DOWN"), (LONG)addr_text);

        show("cycle", 0, &baseline);
    }

    for (i = 0; i < cycles; i++)
    {
        phase_opens(anchor, baseline.opens);
        (VOID)phase_bounce(anchor, iface);
        phase_sockets_across_bounce(anchor, iface, nsocks);
        phase_addremove(anchor, iface, nsocks);

        settle();

        sample(anchor, &cyc[i]);
        show("cycle", i + 1, &cyc[i]);

        if (i == 0 || i == (cycles - 1))
            dump_tasks((i == 0) ? "first" : "last ");
    }

    /* ---- phase G: expunge refused while we hold it ------------------------ */
    say("\n-- expunge with an opener --\n", 0, 0, 0, 0);
    phase_guarded_expunge(anchor);

    /* ---- the verdict ------------------------------------------------------ */
    say("\n-- drift --\n", 0, 0, 0, 0);

    if (cycles >= 2)
    {
        const Sample *a = &cyc[0];
        const Sample *b = &cyc[cycles - 1];

        say("alloc_live %lu to %lu, sockets %lu to %lu\n",
            (LONG)a->alloc_live, (LONG)b->alloc_live,
            (LONG)a->sockets, (LONG)b->sockets);
        say("pool_free %lu to %lu, free %lu to %lu\n",
            (LONG)a->pool_free, (LONG)b->pool_free,
            (LONG)a->free_mem, (LONG)b->free_mem);
        say("sigs %lu to %lu, tasks %lu to %lu\n",
            (LONG)a->sigs, (LONG)b->sigs,
            (LONG)a->tasks, (LONG)b->tasks);

        check(b->alloc_live <= a->alloc_live,
              "allocations outstanding did not grow over the cycles",
              (LONG)a->alloc_live, (LONG)b->alloc_live);
        check(b->sockets <= a->sockets,
              "sockets alive did not grow over the cycles",
              (LONG)a->sockets, (LONG)b->sockets);
        check(b->pool_free >= a->pool_free,
              "packet buffers free did not shrink over the cycles",
              (LONG)a->pool_free, (LONG)b->pool_free);
        check(b->sigs <= a->sigs,
              "no signal bits were leaked onto this Process",
              (LONG)a->sigs, (LONG)b->sigs);
        check(b->tasks <= a->tasks,
              "no Task or Process was left behind over the cycles",
              (LONG)a->tasks, (LONG)b->tasks);

        {
            BOOL steady = TRUE;
            LONG odd    = -1;

            for (i = 1; i < cycles; i++)
            {
                if (cyc[i].tasks != cyc[0].tasks)
                {
                    steady = FALSE;
                    if (odd < 0)
                        odd = i + 1;
                }
            }

            check(steady, "the Task count was the same at every cycle",
                  (LONG)cyc[0].tasks, odd);
        }
    }
    else
    {
        say("(one cycle: nothing to compare against)\n", 0, 0, 0, 0);
    }

    if (expunge >= 2)
    {
        /* First against last, and from the sample taken while the library is
           OPEN: the post-expunge instant is a few ms after ACTION_DIE and a
           Process may still be on its way out. */
        LONG last  = expunge - 1;
        LONG total = (LONG)exp_open[0].free_mem -
                     (LONG)exp_open[last].free_mem;
        LONG per   = total / last;

        say("free at expunge 1: %lu, at %ld: %lu over %ld cycles\n",
            (LONG)exp_open[0].free_mem, last + 1,
            (LONG)exp_open[last].free_mem, last);
        /* run-cycledrill.sh parses this line; keep its shape. */
        say("expunge leak: %ld bytes per cycle\n", per, 0, 0, 0);

        check(exp_open[last].tasks <= exp_open[0].tasks,
              "no Process was left behind by an expunge",
              (LONG)exp_open[0].tasks, (LONG)exp_open[last].tasks);

        say("free at cold 1: %lu, at %ld: %lu\n",
            (LONG)cold_shut[0].free_mem, last + 1,
            (LONG)cold_shut[last].free_mem, 0);
        say("cold leak: %ld bytes per cycle\n",
            ((LONG)cold_shut[0].free_mem -
             (LONG)cold_shut[last].free_mem) / last, 0, 0, 0);

        check(cold_shut[last].sigs <= cold_shut[0].sigs,
              "no signal bit was leaked by a cold open/close",
              (LONG)cold_shut[0].sigs, (LONG)cold_shut[last].sigs);
    }

    say("\ndid: %ld opens, %ld bounces, %ld round trips, %ld expunges\n",
        did_opens, did_bounces, did_roundtrips, did_expunges);

    /* The anchor is deliberately left open so the commands that follow in the
       list report on this stack rather than a fresh one. */
    say("the stack is left running on a held reference\n", 0, 0, 0, 0);

    FreeArgs(rda);

    say("\nCycleDrill: %ld check(s), %ld failed\n", checks, failures, 0, 0);

    return (failures == 0) ? RETURN_OK : RETURN_FAIL;
}
