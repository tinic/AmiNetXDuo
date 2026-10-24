/*
 * forbidrec: every Forbid() and Disable() hold, timed exactly.
 *
 *   forbidrec [SECS=n] [THRESHOLD=us] [RATE=hz] [SELFTEST] [NOSAMPLER]
 *             [CLOCK=ECLOCK]
 *
 * Exec's Forbid, Permit, Disable, Enable and Wait are patched.  The outermost
 * Forbid (TDNestCnt -1 -> 0) and the outermost Disable (IDNestCnt -1 -> 0)
 * take a timestamp; the matching Permit or Enable takes another.  Every hold
 * goes into a histogram per kind, and a hold at or over THRESHOLD (2000 us)
 * goes into a ring with the task, the caller of the entry and the caller of
 * the exit.  A Wait() inside the hold closes it and opens a new one when the
 * task runs again, because the time asleep holds nothing.
 *
 * The clock is the Pi's 1 MHz system timer on an Emu68 (one load, no bus
 * cycle), else the E-clock through ReadEClock().
 *
 * What the patches cannot see -- an inline Forbid (addq.b #1,TDNestCnt) or an
 * interrupt server that runs long -- the CIA-B timer A sampler sees: a run of
 * samples inside Forbid() by one task, and a gap between samples.  It also
 * times how long the XHCI and AHI tasks stay ready and not running.
 *
 * Output is key=value lines.  CTRL-E prints the counts so far and starts a
 * new phase; CTRL-C or SECS ends the run.  Return code: 0 nothing at or over
 * the threshold, 5 something was, 10 the self-test failed, 20 setup failed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/resident.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <hardware/cia.h>
#include <hardware/intbits.h>
#include <resources/cia.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>

/* TO=file: under `Run >file` from httpd's shell, stdout reaches nothing. */
static FILE *out;
#define printf(...) fprintf(out ? out : stdout, __VA_ARGS__)
#include <stddef.h>

#include "netdev_dtree.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: forbidrec 1.0 (24.10.2026) AmiNetXDuo";

extern struct ExecBase *SysBase;
struct Device *TimerBase;

/* ------------------------------------------------------------ the stubs -- */

extern UBYTE fr_blob_start[], fr_blob_end[], fr_data[];
extern UBYTE fr_forbid[], fr_permit[], fr_disable[], fr_enable[], fr_wait[];

/* forbidrec_stub.S has the same offsets. */
#define D_OLDF   0
#define D_OLDP   1
#define D_OLDD   2
#define D_OLDE   3
#define D_OLDW   4
#define D_CF     5
#define D_CP     6
#define D_CD     7
#define D_CE     8
#define D_CWPRE  9
#define D_CWPOST 10
#define D_INFL   11
#define D_ARMED  12
#define D_SYSB   13

_Static_assert(offsetof(struct ExecBase, IDNestCnt) == 294, "IDNestCnt");
_Static_assert(offsetof(struct ExecBase, TDNestCnt) == 295, "TDNestCnt");

#define LVO_DISABLE (-120)
#define LVO_ENABLE  (-126)
#define LVO_FORBID  (-132)
#define LVO_PERMIT  (-138)
#define LVO_WAIT    (-318)

static UBYTE          *blob;
static volatile ULONG *bdata;           /* the copy's data words */

/* ------------------------------------------------------------ the clock -- */

static volatile ULONG *stc;             /* BCM system timer CLO, or NULL */
static ULONG           hz;              /* ticks per second of now() */
static volatile LONG   inclock;         /* inside ReadEClock(): ignore */

static ULONG eclock_lo(ULONG *freq)
{
    struct EClockVal ev;
    ULONG            f;

    inclock++;
    f = ReadEClock(&ev);
    inclock--;
    if (freq)
        *freq = f;
    return ev.ev_lo;
}

/* The peripheral is little-endian behind Emu68's mapping (as genet.c's
   clock was, 5c0dd23c). */
static inline ULONG now(void)
{
    if (stc)
        return __builtin_bswap32(*stc);
    return eclock_lo(NULL);
}

static ULONG ticks_us(ULONG t)
{
    ULONG q = t / hz, r = t % hz;

    if (hz == 1000000UL)
        return t;
    return q * 1000000UL + (r * 1000UL) / (hz / 1000UL);
}

static ULONG us_ticks(ULONG us)
{
    if (hz == 1000000UL)
        return us;
    return (us / 1000UL) * (hz / 1000UL) + ((us % 1000UL) * (hz / 1000UL)) / 1000UL;
}

/* ------------------------------------------------------------ the record -- */

#define KF 0
#define KD 1

#define FL_INT     0x01     /* Disable() taken in interrupt context        */
#define FL_WAITEND 0x02     /* ended by Wait(), not by Permit()/Enable()   */
#define FL_RESUMED 0x04     /* began when Wait() returned                  */
#define FL_BROKEN  0x08     /* DispCount moved: the task slept unseen      */

struct Slot
{
    struct Task *task;
    ULONG        t0, pc0, disp;
    UBYTE        open, flags;
};

struct Hold
{
    ULONG        dur, t0, pc0, pc1;
    struct Task *task;
    UBYTE        kind, flags;
    char         name[32];
};

#define RING 8192
#define NB   6
#define NUM  32

static ULONG          thr, bkt[NB - 1];
static struct Task   *self;
static volatile UBYTE skipself = 1;

static struct Slot    fs, ds;

struct Counts
{
    ULONG hist[2][NB], cnt[2], sum[2][2], broken[2], unmatched[2], stale[2];
    ULONG intforbid, waitsplit[2], clockskip, ringlost;
    ULONG umpc[2][NUM], umn[2][NUM], umover[2];
    struct Hold max[2];
};

static struct Counts  c;
static struct Hold   *ring;
static volatile ULONG rhead;
static ULONG          t_phase;

static void name_copy(char *d, struct Task *t)
{
    const char *s = (t && t->tc_Node.ln_Name) ? t->tc_Node.ln_Name : "(none)";
    int         i;

    for (i = 0; i < 31 && s[i]; i++)
        d[i] = (s[i] == '"') ? '\'' : s[i];
    d[i] = 0;
}

static void fill(struct Hold *h, UBYTE kind, const struct Slot *s, ULONG dur,
                 ULONG pc1, UBYTE f)
{
    h->dur   = dur;
    h->t0    = s->t0;
    h->pc0   = s->pc0;
    h->pc1   = pc1;
    h->task  = s->task;
    h->kind  = kind;
    h->flags = f;
    name_copy(h->name, s->task);
}

static void put(UBYTE kind, const struct Slot *s, ULONG dur, ULONG pc1, UBYTE f)
{
    ULONG i;
    BOOL  on = SysBase->IDNestCnt < 0;

    /* Disable() itself would come back here. */
    if (on)
        *(volatile UWORD *)0xdff09aUL = INTF_INTEN;
    i = rhead++;
    if (on)
        *(volatile UWORD *)0xdff09aUL = INTF_SETCLR | INTF_INTEN;
    if (i < RING)
        fill(&ring[i], kind, s, dur, pc1, f);
    else
        c.ringlost++;
}

static void record(UBYTE kind, struct Slot *s, ULONG t1, ULONG pc1, UBYTE fl)
{
    ULONG dur = t1 - s->t0;
    UBYTE f   = s->flags | fl;
    int   b;

    s->open = 0;
    if (SysBase->DispCount != s->disp)
    {
        c.broken[kind]++;
        if (dur >= thr)
            put(kind, s, dur, pc1, f | FL_BROKEN);
        return;
    }
    for (b = 0; b < NB - 1 && dur >= bkt[b]; b++)
        ;
    c.hist[kind][b]++;
    c.cnt[kind]++;
    c.sum[kind][0] += dur;
    if (c.sum[kind][0] < dur)
        c.sum[kind][1]++;
    if (dur > c.max[kind].dur)
        fill(&c.max[kind], kind, s, dur, pc1, f);
    if (dur >= thr)
        put(kind, s, dur, pc1, f);
}

static void unmatched(UBYTE kind, ULONG pc)
{
    int i;

    c.unmatched[kind]++;
    for (i = 0; i < NUM; i++)
    {
        if (c.umpc[kind][i] == pc)
        {
            c.umn[kind][i]++;
            return;
        }
        if (c.umpc[kind][i] == 0)
        {
            c.umpc[kind][i] = pc;
            c.umn[kind][i]  = 1;
            return;
        }
    }
    c.umover[kind]++;
}

/* Interrupt context: the stack in use is not the task's. */
static inline BOOL intctx(struct Task *t)
{
    volatile UBYTE probe;
    ULONG          sp = (ULONG)&probe;

    return !(t && sp >= (ULONG)t->tc_SPLower && sp < (ULONG)t->tc_SPUpper);
}

static void open_slot(struct Slot *s, struct Task *t, ULONG pc, UBYTE f)
{
    s->task  = t;
    s->pc0   = pc;
    s->disp  = SysBase->DispCount;
    s->flags = f;
    s->t0    = now();
    s->open  = 1;
}

/* The bodies.  Each runs with Forbid() or Disable() held by the caller. */

static void fr_cf(ULONG pc)
{
    struct Task *t = SysBase->ThisTask;

    if (inclock) { c.clockskip++; return; }
    if (intctx(t)) { c.intforbid++; return; }
    if (t == self && skipself)
        return;
    if (fs.open)
        c.stale[KF]++;
    open_slot(&fs, t, pc, 0);
}

static void fr_cp(ULONG pc)
{
    struct Task *t = SysBase->ThisTask;

    if (inclock || intctx(t))
        return;
    if (!fs.open || fs.task != t)
    {
        if (fs.open)
            c.stale[KF]++;
        fs.open = 0;
        if (!(t == self && skipself))
            unmatched(KF, pc);
        return;
    }
    record(KF, &fs, now(), pc, 0);
}

static void fr_cd(ULONG pc)
{
    struct Task *t = SysBase->ThisTask;
    UBYTE        f;

    if (inclock) { c.clockskip++; return; }
    f = intctx(t) ? FL_INT : 0;
    if (!f && t == self && skipself)
        return;
    if (ds.open)
        c.stale[KD]++;
    open_slot(&ds, t, pc, f);
}

static void fr_ce(ULONG pc)
{
    struct Task *t = SysBase->ThisTask;

    if (inclock)
        return;
    if (!ds.open || ds.task != t)
    {
        if (ds.open)
            c.stale[KD]++;
        ds.open = 0;
        if (!(t == self && skipself))
            unmatched(KD, pc);
        return;
    }
    record(KD, &ds, now(), pc, 0);
}

static void fr_wpre(ULONG pc)
{
    struct Task *t = SysBase->ThisTask;

    if (inclock)
        return;
    if (SysBase->TDNestCnt >= 0 && fs.open && fs.task == t)
    {
        c.waitsplit[KF]++;
        record(KF, &fs, now(), pc, FL_WAITEND);
    }
    if (SysBase->IDNestCnt >= 0 && ds.open && ds.task == t)
    {
        c.waitsplit[KD]++;
        record(KD, &ds, now(), pc, FL_WAITEND);
    }
}

static void fr_wpost(ULONG pc)
{
    struct Task *t = SysBase->ThisTask;

    if (inclock || intctx(t) || (t == self && skipself))
        return;
    if (SysBase->TDNestCnt >= 0)
    {
        if (fs.open)
            c.stale[KF]++;
        open_slot(&fs, t, pc, FL_RESUMED);
    }
    if (SysBase->IDNestCnt >= 0)
    {
        if (ds.open)
            c.stale[KD]++;
        open_slot(&ds, t, pc, FL_RESUMED);
    }
}

/* ---------------------------------------------------------- the sampler -- */

#define SRING 4096
#define SK_FORBID 0
#define SK_GAP    1
#define SK_XHCI   2
#define SK_AHI    3
static const char *const skname[] = { "forbid_run", "gap", "xhci_ready_waiting", "ahi_ready_waiting" };

struct Sev
{
    ULONG dur, t0;
    UBYTE kind, seen;
    char  name[32];
};

struct SCounts
{
    ULONG ticks, max[4], ge[4], lost;
    ULONG first, last;
};

struct Library            *CiabBase;
extern volatile struct CIA ciab;
static struct Interrupt    s_int;
static volatile UBYTE      s_armed;
static struct SCounts      sc;
static struct Sev         *sring;
static volatile ULONG      shead;
static ULONG               s_period, s_gapthr;
static struct Task        *s_xhci, *s_ahi;

static ULONG       s_last;
static struct Task *s_rtask;
static ULONG       s_rstart;
static UBYTE       s_rin, s_rseen;
static ULONG       s_ws[2];
static UBYTE       s_win[2];
static struct Task *s_wtask[2];

static void sev(UBYTE kind, ULONG dur, ULONG t0, struct Task *t, UBYTE seen)
{
    ULONG i = shead;

    if (dur > sc.max[kind])
        sc.max[kind] = dur;
    if (dur < thr)
        return;
    sc.ge[kind]++;
    if (i >= SRING) { sc.lost++; return; }
    shead = i + 1;
    sring[i].dur  = dur;
    sring[i].t0   = t0;
    sring[i].kind = kind;
    sring[i].seen = seen;
    name_copy(sring[i].name, t);
}

static UBYTE onready(struct Task *t)
{
    struct Node *n;

    if (!t)
        return 0;
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t)
            return 1;
    return 0;
}

/* cia.resource calls this with jsr: a plain function, it returns with rts. */
static void s_tick(void)
{
    struct Task *t;
    ULONG        ts;
    UBYTE        f;
    int          k;

    if (!s_armed)
        return;
    ts = now();
    t  = SysBase->ThisTask;
    sc.ticks++;
    if (s_last && ts - s_last > s_gapthr)
        sev(SK_GAP, ts - s_last - s_period, s_last, t, 0);
    if (!sc.first)
        sc.first = ts;
    sc.last = ts;
    s_last  = ts;

    f = SysBase->TDNestCnt >= 0;
    if (!(f && s_rin && t == s_rtask))
    {
        if (s_rin)
            sev(SK_FORBID, ts - s_rstart, s_rstart, s_rtask, s_rseen);
        s_rin = f;
        if (f)
        {
            s_rtask  = t;
            s_rstart = ts;
            s_rseen  = fs.open && fs.task == t;
        }
    }

    for (k = 0; k < 2; k++)
    {
        UBYTE r = onready(k ? s_ahi : s_xhci);

        if (r && !s_win[k])
        {
            s_win[k]   = 1;
            s_ws[k]    = ts;
            s_wtask[k] = t;
        }
        else if (!r && s_win[k])
        {
            s_win[k] = 0;
            sev(k ? SK_AHI : SK_XHCI, ts - s_ws[k], s_ws[k], s_wtask[k], 0);
        }
    }
}

static struct Interrupt *AddICR(struct Library *r, WORD bit, struct Interrupt *i)
{
    register struct Interrupt *d0 __asm("d0");
    register struct Library   *a6 __asm("a6") = r;
    register LONG              din __asm("d0") = bit;
    register struct Interrupt *a1 __asm("a1") = i;

    __asm volatile ("jsr a6@(-6:W)" : "=r"(d0) : "r"(a6), "0"(din), "r"(a1)
                    : "d1", "a0", "cc", "memory");
    return d0;
}

static void RemICR(struct Library *r, WORD bit, struct Interrupt *i)
{
    register struct Library   *a6 __asm("a6") = r;
    register LONG              d0 __asm("d0") = bit;
    register struct Interrupt *a1 __asm("a1") = i;

    __asm volatile ("jsr a6@(-12:W)" : "+r"(d0), "+r"(a1) : "r"(a6)
                    : "d1", "a0", "cc", "memory");
}

static const char *s_start(ULONG rate, ULONG ehz)
{
    ULONG per = ehz / rate;

    CiabBase = OpenResource((CONST_STRPTR)CIABNAME);
    if (!CiabBase)
        return "no_ciab";
    s_period = us_ticks(1000000UL / rate);
    s_gapthr = s_period + s_period / 2;
    Forbid();
    s_xhci = FindTask((CONST_STRPTR)"XHCI USB driver");
    s_ahi  = FindTask((CONST_STRPTR)"ahi.device Unit Process");
    Permit();
    s_int.is_Node.ln_Type = NT_INTERRUPT;
    s_int.is_Node.ln_Name = (char *)"forbidrec";
    s_int.is_Code         = (void (*)())s_tick;
    ciab.ciacra &= ~(CIACRAF_START | CIACRAF_RUNMODE | CIACRAF_INMODE | CIACRAF_PBON);
    ciab.ciatalo = (per - 1) & 0xff;
    ciab.ciatahi = (per - 1) >> 8;
    if (AddICR(CiabBase, CIAICRB_TA, &s_int))
        return "cia_busy";
    s_armed = 1;
    ciab.ciacra |= CIACRAF_LOAD | CIACRAF_START;
    return NULL;
}

static void s_stop(void)
{
    if (!s_armed)
        return;
    ciab.ciacra &= ~CIACRAF_START;
    s_armed = 0;
    RemICR(CiabBase, CIAICRB_TA, &s_int);
}

/* ------------------------------------------------------ where is that PC -- */

#define MAXMOD 256
#define MAXRNG 1024
#define MAXLVO 8192
#define LVO_WINDOW (12UL * 1024UL)

#define RK_LIBSEG 1
#define RK_PROC   2
#define RK_STUB   3
#define RK_ROMTAG 4

struct Rng { ULONG lo, hi; UWORD mod; UBYTE hunk, kind; };
struct Lvo { ULONG target; UWORD mod; WORD lvo; };

static char        (*modname)[32];
static UWORD        nmod;
static struct Rng  *rng;
static UWORD        nrng, nlibrng;
static struct Lvo  *lvo;
static UWORD        nlvo;

static UWORD mod_id(const char *s)
{
    UWORD i;
    int   k;

    if (!s)
        s = "(null)";
    for (i = 0; i < nmod; i++)
    {
        for (k = 0; k < 31 && s[k] && s[k] == modname[i][k]; k++)
            ;
        if (modname[i][k] == 0 && (k == 31 || s[k] == 0))
            return i;
    }
    if (nmod >= MAXMOD)
        return MAXMOD - 1;
    for (k = 0; k < 31 && s[k]; k++)
        modname[nmod][k] = (s[k] == '"') ? '\'' : s[k];
    modname[nmod][k] = 0;
    return nmod++;
}

static void add_rng(ULONG lo, ULONG hi, UWORD mod, UBYTE hunk, UBYTE kind)
{
    UWORD i;

    for (i = nlibrng; i < nrng; i++)
        if (rng[i].lo == lo && rng[i].hi == hi)
        {
            rng[i].mod = mod; rng[i].hunk = hunk; rng[i].kind = kind;
            return;
        }
    if (nrng >= MAXRNG)
        return;
    rng[nrng].lo = lo; rng[nrng].hi = hi; rng[nrng].mod = mod;
    rng[nrng].hunk = hunk; rng[nrng].kind = kind;
    nrng++;
}

static void add_seglist(BPTR seg, UWORD mod, UBYTE kind)
{
    UBYTE n;

    for (n = 0; seg && n < 64; n++)
    {
        ULONG *h = (ULONG *)BADDR(seg);

        if (((ULONG)h & 3) || h[-1] < 8 || h[-1] > 16UL * 1024 * 1024)
            return;
        add_rng((ULONG)&h[1], (ULONG)&h[1] + h[-1] - 8, mod, n, kind);
        seg = (BPTR)h[0];
    }
}

#define SEGTAG_MAGIC 0x50534731UL           /* 'PSG1', tools/profiler/prof.h */

static BPTR segtag(struct Library *lib)
{
    const UWORD *p = (const UWORD *)lib, *end;

    if (lib->lib_PosSize < 20)
        return 0;
    end = (const UWORD *)((const UBYTE *)lib + lib->lib_PosSize - 20);
    for (; p <= end; p++)
    {
        const ULONG *t = (const ULONG *)p;

        if (t[0] == SEGTAG_MAGIC && t[1] == 20 && t[2] == (ULONG)lib && t[3] &&
            t[0] + t[1] + t[2] + t[3] + t[4] == 0)
            return (BPTR)t[3];
    }
    return 0;
}

static void scan_list(struct List *l)
{
    struct Node *n;

    for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ)
    {
        struct Library *lib = (struct Library *)n;
        UWORD           m   = mod_id(n->ln_Name), i, cnt = lib->lib_NegSize / 6;
        BPTR            sl;

        for (i = 1; i <= cnt && nlvo < MAXLVO; i++)
        {
            UWORD *e = (UWORD *)((UBYTE *)lib - 6 * i);
            ULONG  tg;

            if (e[0] == 0x4ef9)
                tg = *(ULONG *)(e + 1);
            else if (e[0] == 0x4efa)
                tg = (ULONG)(e + 1) + (LONG)(WORD)e[1];
            else
                continue;
            if (tg >= (ULONG)blob && tg < (ULONG)blob + ((ULONG)fr_blob_end - (ULONG)fr_blob_start))
                continue;
            lvo[nlvo].target = tg;
            lvo[nlvo].mod    = m;
            lvo[nlvo].lvo    = -6 * (WORD)i;
            nlvo++;
        }
        sl = segtag(lib);
        if (sl)
            add_seglist(sl, m, RK_LIBSEG);
    }
}

static void scan_proc(struct Task *t)
{
    struct Process *p = (struct Process *)t;
    char            nm[32];

    if (!t || t->tc_Node.ln_Type != NT_PROCESS)
        return;
    if (p->pr_CLI)
    {
        struct CommandLineInterface *cli = BADDR(p->pr_CLI);
        UBYTE                       *b   = BADDR(cli->cli_CommandName);

        if (cli->cli_Module)
        {
            int i, n = b ? b[0] : 0, s = 0;

            for (i = 1; i <= n; i++)
                if (b[i] == ':' || b[i] == '/')
                    s = i;
            for (i = 0; s + 1 + i <= n && i < 31; i++)
                nm[i] = b[s + 1 + i];
            nm[i] = 0;
            add_seglist(cli->cli_Module, mod_id(i ? nm : t->tc_Node.ln_Name), RK_PROC);
        }
    }
    else if (p->pr_SegList)
    {
        ULONG *a = BADDR(p->pr_SegList);

        if (a[0] >= 3 && a[3])
            add_seglist((BPTR)a[3], mod_id(t->tc_Node.ln_Name), RK_PROC);
    }
}

/* Processes come and go during a run, so they are scanned every couple of
   seconds and kept; libraries are scanned fresh before each dump. */
static void scan_procs(void)
{
    struct Node *n;

    Forbid();
    scan_proc(SysBase->ThisTask);
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        scan_proc((struct Task *)n);
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
        scan_proc((struct Task *)n);
    Permit();
}

static void scan_libs(void)
{
    UWORD i, keep;

    Forbid();
    /* Drop the old library ranges, keep the processes. */
    for (i = 0, keep = 0; i < nrng; i++)
        if (rng[i].kind == RK_PROC)
            rng[keep++] = rng[i];
    nrng = keep;
    nlibrng = 0;
    nlvo = 0;
    add_rng((ULONG)blob, (ULONG)blob + ((ULONG)fr_blob_end - (ULONG)fr_blob_start),
            mod_id("forbidrec.stub"), 0, RK_STUB);
    /* Kickstart modules by their romtags: an LVO near a ROM PC can belong
       to a module whose jump table points somewhere else entirely. */
    {
        ULONG *rm = (ULONG *)SysBase->ResModules;

        while (rm && *rm)
        {
            struct Resident *r;

            if (*rm & 0x80000000UL)
            {
                rm = (ULONG *)(*rm & 0x7fffffffUL);
                continue;
            }
            r = (struct Resident *)*rm++;
            if (r->rt_MatchTag == r && (ULONG)r->rt_EndSkip > (ULONG)r)
                add_rng((ULONG)r, (ULONG)r->rt_EndSkip, mod_id(r->rt_Name), 0, RK_ROMTAG);
        }
    }
    scan_list(&SysBase->LibList);
    scan_list(&SysBase->DeviceList);
    scan_list(&SysBase->ResourceList);
    Permit();
}

static void where(const char *key, ULONG pc)
{
    UWORD i, best = 0xffff;
    ULONG bd = LVO_WINDOW;

    printf(" %s=0x%08lx", key, pc);
    if (!pc)
        return;
    for (i = 0; i < nrng; i++)
        if (pc >= rng[i].lo && pc < rng[i].hi && rng[i].kind == RK_ROMTAG)
        {
            printf(" %s_in=\"%s\" %s_romtag_off=0x%lx", key, modname[rng[i].mod],
                   key, pc - rng[i].lo);
            return;
        }
    for (i = 0; i < nrng; i++)
        if (pc >= rng[i].lo && pc < rng[i].hi)
        {
            printf(" %s_in=\"%s\" %s_hunk=%u %s_off=0x%lx", key, modname[rng[i].mod],
                   key, rng[i].hunk, key, pc - rng[i].lo);
            return;
        }
    for (i = 0; i < nlvo; i++)
        if (lvo[i].target <= pc && pc - lvo[i].target < bd)
        {
            bd   = pc - lvo[i].target;
            best = i;
        }
    if (best != 0xffff)
    {
        printf(" %s_in=\"%s\" %s_lvo=%d %s_lvo_off=0x%lx", key, modname[lvo[best].mod],
               key, lvo[best].lvo, key, bd);
        return;
    }
    if (pc >= 0xf80000UL && pc < 0x1000000UL)
        printf(" %s_in=\"kickstart\" %s_off=0x%lx", key, key, pc - 0xf80000UL);
    else
        printf(" %s_in=\"?\"", key);
}

/* ------------------------------------------------------------- the dump -- */

static const char *const kname[] = { "forbid", "disable" };

static void flags_out(UBYTE f)
{
    printf(" start=%s end=%s ctx=%s broken=%d",
           (f & FL_RESUMED) ? "wait" : "call", (f & FL_WAITEND) ? "wait" : "call",
           (f & FL_INT) ? "int" : "task", (f & FL_BROKEN) ? 1 : 0);
}

static void hold_out(const char *tag, ULONG n, const struct Hold *h)
{
    printf("%s n=%lu kind=%s dur_us=%lu t_ms=%lu task=\"%s\"", tag, n, kname[h->kind],
           ticks_us(h->dur), ticks_us(h->t0 - t_phase) / 1000UL, h->name);
    flags_out(h->flags);
    where("pc0", h->pc0);
    where("pc1", h->pc1);
    printf("\n");
}

static ULONG dump(ULONG phase, ULONG secs, const struct Counts *k, const struct Hold *r,
                  ULONG nr, const struct SCounts *s, const struct Sev *sr, ULONG ns)
{
    static const char *const bn[NB] = { "lt_500us", "500us_1ms", "1_2ms", "2_5ms", "5_10ms", "ge_10ms" };
    ULONG i, ge = 0;
    int   kk, b;

    scan_libs();
    printf("phase=%lu secs=%lu clock=%s clock_hz=%lu threshold_us=%lu\n", phase, secs,
           stc ? "bcm2711_stc" : "eclock", hz, ticks_us(thr));
    for (kk = 0; kk < 2; kk++)
    {
        ULONG avg = k->cnt[kk] ? ticks_us(k->sum[kk][1] ? 0xffffffffUL : k->sum[kk][0]) / k->cnt[kk] : 0;

        printf("hist phase=%lu kind=%s holds=%lu avg_us=%lu max_us=%lu", phase, kname[kk],
               k->cnt[kk], avg, ticks_us(k->max[kk].dur));
        for (b = 0; b < NB; b++)
            printf(" %s=%lu", bn[b], k->hist[kk][b]);
        printf(" ge_threshold=%lu waitsplit=%lu broken=%lu unmatched=%lu stale=%lu\n",
               k->hist[kk][3] + k->hist[kk][4] + k->hist[kk][5], k->waitsplit[kk],
               k->broken[kk], k->unmatched[kk], k->stale[kk]);
        ge += k->hist[kk][3] + k->hist[kk][4] + k->hist[kk][5];
    }
    printf("counters phase=%lu forbid_in_interrupt=%lu clock_skips=%lu ring_lost=%lu\n",
           phase, k->intforbid, k->clockskip, k->ringlost);
    for (kk = 0; kk < 2; kk++)
        if (k->max[kk].dur)
            hold_out("max", (ULONG)kk, &k->max[kk]);
    for (i = 0; i < nr && i < RING; i++)
        hold_out("hold", i + 1, &r[i]);
    for (kk = 0; kk < 2; kk++)
        for (i = 0; i < NUM && k->umpc[kk][i]; i++)
        {
            printf("unmatched kind=%s count=%lu", kname[kk], k->umn[kk][i]);
            where("pc", k->umpc[kk][i]);
            printf("\n");
        }
    if (s)
    {
        ULONG span = s->last - s->first;

        printf("sampler phase=%lu ticks=%lu span_ms=%lu xhci_found=%d ahi_found=%d lost=%lu",
               phase, s->ticks, ticks_us(span) / 1000UL, s_xhci != 0, s_ahi != 0, s->lost);
        for (kk = 0; kk < 4; kk++)
            printf(" %s_max_us=%lu %s_ge_threshold=%lu", skname[kk], ticks_us(s->max[kk]),
                   skname[kk], s->ge[kk]);
        printf("\n");
        for (i = 0; i < ns && i < SRING; i++)
            printf("sample n=%lu kind=%s dur_us=%lu t_ms=%lu task=\"%s\" recorder_saw_it=%d\n",
                   i + 1, skname[sr[i].kind], ticks_us(sr[i].dur),
                   ticks_us(sr[i].t0 - t_phase) / 1000UL, sr[i].name, sr[i].seen);
    }
    fflush(out ? out : stdout);
    return ge;
}

/* Called with Disable() held. */
static void zero_counts(void)
{
    UBYTE *p;
    ULONG  i;

    for (p = (UBYTE *)&c, i = 0; i < sizeof(c); i++)
        p[i] = 0;
    for (p = (UBYTE *)&sc, i = 0; i < sizeof(sc); i++)
        p[i] = 0;
    rhead = 0;
    shead = 0;
}

/* Swap the record out under Disable(): the dump reads a still copy. */
static struct Counts   kc;
static struct Hold    *ring2;
static struct SCounts  ksc;
static struct Sev     *sring2;

static ULONG phase_end(ULONG phase, ULONG secs, BOOL sampler)
{
    struct Hold *r;
    struct Sev  *sr;
    ULONG        nr, ns, tn;

    Disable();
    tn     = now();
    kc     = c;
    r      = ring;   ring = ring2;   ring2 = r;   nr = rhead;
    ksc    = sc;
    sr     = sring;  sring = sring2; sring2 = sr; ns = shead;
    zero_counts();
    Enable();
    {
        ULONG ge = dump(phase, secs, &kc, r, nr, sampler ? &ksc : NULL, sr, ns);
        t_phase = tn;
        return ge;
    }
}

/* ------------------------------------------------------------ self-test -- */

extern UBYTE fr_st_f1[], fr_st_p1[], fr_st_d1[], fr_st_e1[];

/* Raster lines, from VPOSR:VHPOSR: a clock the recorder does not use.  The
   distance moved is added up, not the changes counted, so an interrupt that
   runs longer than a line does not stretch the spin. */
static void spin_lines(ULONG n)
{
    volatile ULONG *vp = (volatile ULONG *)0xdff004UL;
    ULONG           fl = (SysBase->VBlankFrequency == 60) ? 263 : 313;
    ULONG           last = (*vp >> 8) & 0x1ff, acc = 0, t0 = now(), lim = us_ticks(200000UL);

    while (acc < n && now() - t0 < lim)
    {
        ULONG v = (*vp >> 8) & 0x1ff;

        if (v != last)
        {
            acc += (v + fl - last) % fl;
            last = v;
        }
    }
}

static void __attribute__((noinline)) st_forbid(ULONG lines)
{
    register struct ExecBase *a6 __asm("a6") = SysBase;

    __asm volatile ("jsr a6@(-132:W)\n\t.globl _fr_st_f1\n_fr_st_f1:" : : "r"(a6) : "cc", "memory");
    spin_lines(lines);
    __asm volatile ("jsr a6@(-138:W)\n\t.globl _fr_st_p1\n_fr_st_p1:" : : "r"(a6) : "cc", "memory");
}

static void __attribute__((noinline)) st_disable(ULONG lines)
{
    register struct ExecBase *a6 __asm("a6") = SysBase;

    __asm volatile ("jsr a6@(-120:W)\n\t.globl _fr_st_d1\n_fr_st_d1:" : : "r"(a6) : "cc", "memory");
    spin_lines(lines);
    __asm volatile ("jsr a6@(-126:W)\n\t.globl _fr_st_e1\n_fr_st_e1:" : : "r"(a6) : "cc", "memory");
}

static BOOL near(ULONG us, ULONG want)
{
    ULONG tol = want / 10 + 150;

    return us + tol >= want && us <= want + tol;
}

static BOOL wait_half(ULONG us, ULONG want)
{
    return us + want / 10 >= want && us <= want + 5000;
}

static BOOL selftest(void)
{
    ULONG  line_ns = (SysBase->VBlankFrequency == 60) ? 63555UL : 64000UL;
    ULONG  i, nr, f3 = 0, d2 = 0, wsplit = 0, wres = 0, wbroken = 0, bad = 0, nest = 0;
    struct Hold *r;
    BOOL   ok;

    Disable();
    rhead = 0;
    Enable();
    skipself = 0;

    st_forbid(47);                      /* 3 ms */
    st_disable(39);                     /* 2.5 ms */

    Forbid();                           /* broken by Wait() inside Delay() */
    spin_lines(39);
    Delay(2);
    spin_lines(39);
    Permit();

    Forbid();                           /* nested: one hold, the outer pair */
    Forbid();
    spin_lines(39);
    Permit();
    Permit();

    Forbid();
    Permit();
    skipself = 1;

    Disable();
    nr = rhead;
    r  = ring;
    Enable();

    for (i = 0; i < nr && i < RING; i++)
    {
        const struct Hold *h  = &r[i];
        ULONG              us = ticks_us(h->dur);

        if (h->task != self)
            continue;
        hold_out("selftest_hold", i + 1, h);
        /* Interrupts run inside a Forbid(), and the hold includes them:
           only a Disable() hold is the spin and nothing else. */
        if (h->kind == KF && h->pc0 == (ULONG)fr_st_f1 && h->pc1 == (ULONG)fr_st_p1 &&
            wait_half(us, 47 * line_ns / 1000))
            f3++;
        else if (h->kind == KD && h->pc0 == (ULONG)fr_st_d1 && h->pc1 == (ULONG)fr_st_e1 &&
                 near(us, 39 * line_ns / 1000) && !(h->flags & FL_INT))
            d2++;
        else if (h->kind == KF && (h->flags & FL_BROKEN))
            wbroken++;
        /* Delay() is Forbid()den up to its Wait() and again after it: the
           halves are the spins plus that, never the 40 ms asleep. */
        else if (h->kind == KF && (h->flags & FL_WAITEND) && wait_half(us, 39 * line_ns / 1000))
            wsplit++;
        else if (h->kind == KF && (h->flags & FL_RESUMED) && wait_half(us, 39 * line_ns / 1000))
            wres++;
        else if (h->kind == KF && wait_half(us, 39 * line_ns / 1000) && !h->flags)
            nest++;
        else
            bad++;
    }
    ok = f3 == 1 && d2 == 1 && nest == 1 && bad == 0 &&
         ((wsplit == 1 && wres == 1 && wbroken == 0) || (wsplit == 0 && wres == 0 && wbroken == 1));
    printf("selftest forbid_3ms=%lu disable_2500us=%lu nested=%lu wait=%s other=%lu"
           " pc_forbid=0x%08lx pc_permit=0x%08lx selftest=%s\n",
           f3, d2, nest, wsplit ? "split" : (wbroken ? "broken" : "missing"), bad,
           (ULONG)fr_st_f1, (ULONG)fr_st_p1, ok ? "pass" : "fail");
    Disable();
    zero_counts();
    Enable();
    t_phase = now();
    return ok;
}

/* ----------------------------------------------------------------- main -- */

static ULONG jmp_target(LONG off)
{
    UWORD *e = (UWORD *)((UBYTE *)SysBase + off);

    return (e[0] == 0x4ef9) ? *(ULONG *)(e + 1) : 0;
}

struct Patch { LONG lvo; UBYTE *stub; int old; };
static const struct Patch patches[] = {
    { LVO_FORBID,  fr_forbid,  D_OLDF },
    { LVO_PERMIT,  fr_permit,  D_OLDP },
    { LVO_DISABLE, fr_disable, D_OLDD },
    { LVO_ENABLE,  fr_enable,  D_OLDE },
    { LVO_WAIT,    fr_wait,    D_OLDW },
};
#define NPATCH 5

static ULONG entry(const UBYTE *stub)
{
    return (ULONG)blob + ((ULONG)stub - (ULONG)fr_blob_start);
}

int main(void)
{
    static struct timerequest tr;
    static const char template[] = "SECS/N,THRESHOLD/N,RATE/N,SELFTEST/S,NOSAMPLER/S,CLOCK/K,TO/K";
    LONG          args[7] = { 0 };
    struct RDArgs *rd;
    ULONG         secs = 0, thr_us = 2000, rate = 4700, ehz, phase = 1, ge = 0, t_start, i;
    BOOL          sampler, st_ok = TRUE, chained = FALSE;
    const char   *serr = NULL;
    ULONG         blobsize = (ULONG)fr_blob_end - (ULONG)fr_blob_start;
    int           rc = 0;

    rd = ReadArgs((CONST_STRPTR)template, args, NULL);
    if (!rd)
    {
        printf("result=bad_args template=\"%s\"\n", template);
        return 20;
    }
    if (args[6] && !(out = fopen((const char *)args[6], "w")))
    {
        FreeArgs(rd);
        return 20;
    }
    if (args[0]) secs   = *(ULONG *)args[0];
    if (args[1]) thr_us = *(ULONG *)args[1];
    if (args[2]) rate   = *(ULONG *)args[2];
    sampler = !args[4];

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0))
    {
        printf("result=no_timer\n");
        FreeArgs(rd);
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    eclock_lo(&ehz);
    hz = ehz;

    /* The Pi's system timer, if this is an Emu68 and the window is there. */
    if (!(args[5] && ((char *)args[5])[0] == 'E'))
    {
        ULONG base;

        if (netdev_dtree_bus_addr("/soc", 0x7e003000UL, &base))
        {
            volatile ULONG *clo = (volatile ULONG *)(base + 4);
            ULONG c0 = __builtin_bswap32(*clo), e0 = eclock_lo(NULL), c1, e1, dus;

            Delay(5);
            c1 = __builtin_bswap32(*clo);
            e1 = eclock_lo(NULL);
            hz = ehz;
            dus = ticks_us(e1 - e0);
            printf("clock_probe stc_base=0x%08lx stc_us=%lu eclock_us=%lu\n", base, c1 - c0, dus);
            if (c1 - c0 > dus - dus / 20 && c1 - c0 < dus + dus / 20)
            {
                stc = clo;
                hz  = 1000000UL;
            }
        }
    }

    thr = us_ticks(thr_us);
    bkt[0] = us_ticks(500);  bkt[1] = us_ticks(1000); bkt[2] = us_ticks(2000);
    bkt[3] = us_ticks(5000); bkt[4] = us_ticks(10000);
    if (thr_us != 2000)
        bkt[2] = thr;

    ring    = AllocMem(RING * sizeof(struct Hold), MEMF_PUBLIC | MEMF_CLEAR);
    ring2   = AllocMem(RING * sizeof(struct Hold), MEMF_PUBLIC | MEMF_CLEAR);
    sring   = AllocMem(SRING * sizeof(struct Sev), MEMF_PUBLIC | MEMF_CLEAR);
    sring2  = AllocMem(SRING * sizeof(struct Sev), MEMF_PUBLIC | MEMF_CLEAR);
    modname = AllocMem(MAXMOD * 32, MEMF_PUBLIC | MEMF_CLEAR);
    rng     = AllocMem(MAXRNG * sizeof(struct Rng), MEMF_PUBLIC | MEMF_CLEAR);
    lvo     = AllocMem(MAXLVO * sizeof(struct Lvo), MEMF_PUBLIC | MEMF_CLEAR);
    /* Never freed: see forbidrec_stub.S. */
    blob    = AllocMem(blobsize, MEMF_PUBLIC);
    if (!ring || !ring2 || !sring || !sring2 || !modname || !rng || !lvo || !blob)
    {
        printf("result=no_memory\n");
        return 20;
    }
    CopyMem(fr_blob_start, blob, blobsize);
    bdata = (volatile ULONG *)(blob + ((ULONG)fr_data - (ULONG)fr_blob_start));
    bdata[D_CF] = (ULONG)fr_cf;    bdata[D_CP] = (ULONG)fr_cp;
    bdata[D_CD] = (ULONG)fr_cd;    bdata[D_CE] = (ULONG)fr_ce;
    bdata[D_CWPRE] = (ULONG)fr_wpre; bdata[D_CWPOST] = (ULONG)fr_wpost;
    bdata[D_INFL] = 0; bdata[D_ARMED] = 0; bdata[D_SYSB] = (ULONG)SysBase;
    for (i = 0; i < NPATCH; i++)
    {
        bdata[patches[i].old] = jmp_target(patches[i].lvo);
        if (!bdata[patches[i].old])
        {
            printf("result=vector_not_jmp lvo=%ld\n", patches[i].lvo);
            return 20;
        }
    }
    CacheClearU();
    self = FindTask(NULL);

    /* The old vector is in the stub BEFORE the stub goes in: SetFunction()
       calls Forbid() and Permit() itself, and so does every interrupt that
       calls Disable() in between. */
    Disable();
    for (i = 0; i < NPATCH; i++)
    {
        APTR o = SetFunction((struct Library *)SysBase, patches[i].lvo,
                             (APTR)entry(patches[i].stub));
        bdata[patches[i].old] = (ULONG)o;
    }
    Enable();
    printf("patched blob=0x%08lx blob_bytes=%lu forbid_was=0x%08lx disable_was=0x%08lx exec=%u.%u\n",
           (ULONG)blob, blobsize, bdata[D_OLDF], bdata[D_OLDD],
           SysBase->LibNode.lib_Version, SysBase->LibNode.lib_Revision);
    scan_libs();
    scan_procs();
    t_phase = now();
    t_start = t_phase;
    bdata[D_ARMED] = 1;

    if (sampler && (serr = s_start(rate, ehz)) != NULL)
    {
        printf("sampler=%s\n", serr);
        sampler = FALSE;
    }

    if (args[3])
        st_ok = selftest();
    fflush(out ? out : stdout);

    {
        ULONG ticks = 0, lim = secs * 10, last_ps = 0;

        for (;;)
        {
            ULONG sig = SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);

            if (sig & SIGBREAKF_CTRL_C)
                break;
            if (lim && ticks >= lim)
                break;
            if (sig & SIGBREAKF_CTRL_E)
            {
                ge += phase_end(phase, ticks_us(now() - t_phase) / 1000000UL, sampler);
                phase++;
            }
            Delay(5);
            ticks++;
            if (ticks - last_ps >= 20)
            {
                scan_procs();
                last_ps = ticks;
            }
        }
    }

    s_stop();
    bdata[D_ARMED] = 0;
    Disable();
    for (i = 0; i < NPATCH; i++)
    {
        APTR o = SetFunction((struct Library *)SysBase, patches[i].lvo,
                             (APTR)bdata[patches[i].old]);

        if ((ULONG)o != entry(patches[i].stub))
        {
            /* Somebody patched over us: put theirs back.  Ours stays in
               their chain, disarmed, and the block is never freed. */
            SetFunction((struct Library *)SysBase, patches[i].lvo, o);
            chained = TRUE;
        }
    }
    Enable();
    for (i = 0; bdata[D_INFL] && i < 3000; i++)
        Delay(1);

    ge += phase_end(phase, ticks_us(now() - t_phase) / 1000000UL, sampler);
    printf("result=%s phases=%lu total_s=%lu holds_ge_threshold=%lu unpatch=%s inflight=%lu resident_bytes=%lu\n",
           !st_ok ? "selftest_failed" : (ge ? "holds_found" : "clean"), phase,
           ticks_us(now() - t_start) / 1000000UL, ge, chained ? "chained" : "restored",
           bdata[D_INFL], blobsize);
    fflush(out ? out : stdout);
    if (bdata[D_INFL])
    {
        /* A body may still be running: never unload under it. */
        for (;;)
            Wait(0);
    }

    FreeMem(ring, RING * sizeof(struct Hold));
    FreeMem(ring2, RING * sizeof(struct Hold));
    FreeMem(sring, SRING * sizeof(struct Sev));
    FreeMem(sring2, SRING * sizeof(struct Sev));
    FreeMem(modname, MAXMOD * 32);
    FreeMem(rng, MAXRNG * sizeof(struct Rng));
    FreeMem(lvo, MAXLVO * sizeof(struct Lvo));
    CloseDevice((struct IORequest *)&tr);
    FreeArgs(rd);
    if (out)
        fclose(out);
    rc = !st_ok ? 10 : (ge ? 5 : 0);
    return rc;
}
