/*
 * AmiNetXDuo, crash guard implementation.
 *
 * Exec pushes the trap number as a longword on the supervisor stack; above it
 * sits the exception frame: SR.w, PC.l, and on 68010+ a format/vector word.
 * If SR has the supervisor bit set, no recovery is attempted.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/crashguard.h"
#include "aminetxduo/asm_abi.h"
#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>              /* LP1NR, for RawPutChar */

#include <setjmp.h>
#include <stdarg.h>

static AmiCrashInfo ami_crash;
static APTR         ami_crash_old_trap;
static struct Task *ami_crash_task;
static jmp_buf      ami_crash_jmp;
static APTR         ami_crash_ref;
static const char  *ami_crash_ref_label = "code";

/* Filled in by the assembly stub before it returns to user mode. */
ULONG ami_crash_saved_regs[15];     /* d0-d7, a0-a6 */
ULONG ami_crash_saved_number;
ULONG ami_crash_saved_pc;
UWORD ami_crash_saved_sr;
UWORD ami_crash_saved_format;

VOID ami_crash_bailout(VOID);
VOID ami_crash_trap(VOID);

/*
 * The trap stub. Written in assembly because it runs in supervisor mode on the
 * exception frame and must finish with RTE.
 *
 * On entry:  (sp) = trap number (longword, pushed by Exec)
 *            4(sp) = SR.w, 6(sp) = PC.l, 10(sp) = format word (68010+)
 */
__asm__(
"       .text                                   \n"
"       .globl  _ami_crash_trap                 \n"
"_ami_crash_trap:                               \n"
"       movem.l %d0-%d7/%a0-%a6,_ami_crash_saved_regs \n"
"       move.l  (%sp)+,_ami_crash_saved_number  \n"  /* pop Exec's trap number */
"       move.w  (%sp),_ami_crash_saved_sr       \n"  /* frame: SR             */
"       move.l  2(%sp),_ami_crash_saved_pc      \n"  /* frame: PC             */
"       move.w  6(%sp),_ami_crash_saved_format  \n"  /* frame: format/vector  */
"       btst    #5,(%sp)                        \n"  /* SR bit 13 = supervisor */
"       move.w  (%sp),%d0                       \n"
"       andi.w  #0x2000,%d0                     \n"
"       bne     1f                              \n"  /* was supervisor: give up */
"       move.l  #_ami_crash_bailout,2(%sp)      \n"  /* return into user code  */
"       rte                                     \n"
"1:     rte                                     \n"  /* nothing safe to do     */
);

const char *ami_crash_name(ULONG number)
{
    switch (number)
    {
        case 2:  return "bus error (bad address)";
        case 3:  return "address error (misaligned)";
        case 4:  return "illegal instruction";
        case 5:  return "divide by zero";
        case 6:  return "CHK instruction";
        case 7:  return "TRAPV overflow";
        case 8:  return "privilege violation";
        case 9:  return "trace";
        case 10: return "line-A emulator";
        case 11: return "line-F emulator";
        case 24: return "spurious interrupt";
        default: break;
    }
    if (number >= 32 && number <= 47)
        return "TRAP instruction";
    return "unknown exception";
}

VOID ami_crash_set_reference(APTR code_address, const char *label)
{
    ami_crash_ref = code_address;
    if (label != NULL)
        ami_crash_ref_label = label;
}


/* One-line summary, for the DH0:crash.txt record. */
VOID ami_crash_format(char *buf, ULONG len)
{
    static const char hex[] = "0123456789abcdef";
    const char *name = ami_crash_name(ami_crash_saved_number);
    ULONG i = 0;
    int shift;

    while (*name != '\0' && i + 20 < len)
        buf[i++] = *name++;

    if (i + 14 < len)
    {
        buf[i++] = ' '; buf[i++] = 'a'; buf[i++] = 't'; buf[i++] = ' ';
        buf[i++] = 'P'; buf[i++] = 'C'; buf[i++] = '=';
        for (shift = 28; shift >= 0; shift -= 4)
            buf[i++] = hex[(ami_crash_saved_pc >> shift) & 0xF];
    }
    if (i + 1 < len)
        buf[i++] = '\n';
    buf[i] = '\0';
}

/* Runs in user mode, on the stack of the crashed task, directly after the RTE.
   `used' because the only reference is the `move.l #_ami_crash_bailout,2(%sp)'
   in the trap handler's asm() above, which the compiler does not parse. It
   also carries the five ami_crash_saved_* objects: this is their only C
   reader, so if it goes they go, and the asm block loses six names at once. */
VOID ami_crash_bailout(VOID) __attribute__((used));
VOID ami_crash_bailout(VOID)
{
    int i;

    /* Take this task out of the trap path BEFORE the reporting below.  The
       report runs on the crashed task's stack -- whose overflow, or a
       corrupted Exec list Open() walks, may be what crashed -- so a fault here
       must not re-enter _ami_crash_trap and overwrite the first crash's
       evidence with a second, unbounded one.  A re-fault then takes the
       previous handler (normally Exec's), a Guru, which is the right outcome
       for a faulting crash reporter.  ami_crash_remove() already restores in
       this order (F-082). */
    ami_crash_task->tc_TrapCode = ami_crash_old_trap;

    ami_crash.number = ami_crash_saved_number;
    ami_crash.pc     = ami_crash_saved_pc;
    ami_crash.sr     = ami_crash_saved_sr;
    ami_crash.format = ami_crash_saved_format;
    ami_crash.valid  = TRUE;

    for (i = 0; i < 8; i++)
        ami_crash.d[i] = ami_crash_saved_regs[i];
    for (i = 0; i < 7; i++)
        ami_crash.a[i] = ami_crash_saved_regs[8 + i];

    AMI_ERROR("*** CRASH: %s (exception %ld)",
              (LONG)ami_crash_name(ami_crash.number), (LONG)ami_crash.number);
    AMI_ERROR("    PC=%08lx  SR=%04lx  format=%04lx",
              (LONG)ami_crash.pc, (LONG)ami_crash.sr, (LONG)ami_crash.format);

    if (ami_crash_ref != NULL)
    {
        /*
         * No %c here: RawDoFmt consumes a word for %c, but the C caller pushes
         * a longword, which misaligns every argument after it. Only %ld, %lx
         * and %s, all longword, are safe in this formatter.
         */
        LONG delta = (LONG)ami_crash.pc - (LONG)ami_crash_ref;

        AMI_ERROR("    reference %s = %08lx", (LONG)ami_crash_ref_label,
                  (LONG)ami_crash_ref);
        if (delta >= 0)
            AMI_ERROR("    PC is reference + %ld (0x%lx)", (LONG)delta, (LONG)delta);
        else
            AMI_ERROR("    PC is reference - %ld (0x%lx)", (LONG)(-delta),
                      (LONG)(-delta));
    }

    AMI_ERROR("    d0=%08lx d1=%08lx d2=%08lx d3=%08lx",
              (LONG)ami_crash.d[0], (LONG)ami_crash.d[1],
              (LONG)ami_crash.d[2], (LONG)ami_crash.d[3]);
    AMI_ERROR("    d4=%08lx d5=%08lx d6=%08lx d7=%08lx",
              (LONG)ami_crash.d[4], (LONG)ami_crash.d[5],
              (LONG)ami_crash.d[6], (LONG)ami_crash.d[7]);
    AMI_ERROR("    a0=%08lx a1=%08lx a2=%08lx a3=%08lx",
              (LONG)ami_crash.a[0], (LONG)ami_crash.a[1],
              (LONG)ami_crash.a[2], (LONG)ami_crash.a[3]);
    AMI_ERROR("    a4=%08lx a5=%08lx a6=%08lx",
              (LONG)ami_crash.a[4], (LONG)ami_crash.a[5], (LONG)ami_crash.a[6]);

    /*
     * Leave a record that the host can read even if the unwind below fails.
     * The harness stages DH0: from a host directory, so this file appears
     * outside the emulator at once.
     */
    {
        BPTR fh = Open((STRPTR)"DH0:crash.txt", MODE_NEWFILE);

        if (fh != 0)
        {
            char line[80];

            ami_crash_format(line, sizeof(line));
            FPuts(fh, (STRPTR)line);
            Close(fh);
        }
    }

    /*
     * Unwind to the ami_crash_install() call site.
     *
     * This does not reliably return control. Recovery from a patched exception
     * frame lands here in user mode with the stack pointer of the crashed
     * task, and the setjmp context is not always honoured from that state.
     * Under FS-UAE the resume was observed after the call site, not at it. The
     * report above is dependable and the resume is not, so callers must treat
     * a caught crash as fatal and must not continue.
     *
     * The tc_TrapCode restore was moved to the top of this function (F-082):
     * it must happen before the reporting, not after, so a fault inside the
     * report cannot re-enter the trap.
     */
    longjmp(ami_crash_jmp, 1);
}

BOOL ami_crash_install(VOID)
{
    if (setjmp(ami_crash_jmp) != 0)
        return FALSE;               /* arrived here from a crash */

    ami_crash_task     = FindTask(NULL);
    ami_crash_old_trap = ami_crash_task->tc_TrapCode;
    ami_crash_task->tc_TrapCode = (APTR)ami_crash_trap;
    ami_crash.valid    = FALSE;

    return TRUE;
}

VOID ami_crash_remove(VOID)
{
    if (ami_crash_task != NULL)
    {
        ami_crash_task->tc_TrapCode = ami_crash_old_trap;
        ami_crash_task = NULL;
    }
}

const AmiCrashInfo *ami_crash_info(VOID)
{
    return ami_crash.valid ? &ami_crash : NULL;
}

/* ------------------------------------------------------- exec Alert hook, */

/*
 * A Guru is not a CPU exception. When Exec detects corruption, it calls its
 * own Alert(), LVO -108, with the alert number in d7. That path never goes
 * near tc_TrapCode, so the trap handler above cannot see a double free, a
 * corrupt memory list or a reused IORequest.
 *
 * SetFunction() on the Alert vector of exec catches them. The trampoline logs
 * and then tail-jumps to the original, so normal Guru behaviour is unchanged.
 *
 * This patches Exec machine-wide, so it is a debugging aid. Install it in
 * tests and tools under the emulator, and always remove it before exit.
 */

/* Gurus on their way through this image's trampoline, the report included,
   which calls DOS and can Wait() (F-080).  Counted in by a stub before it
   enters this image and out by that stub's exit after the Guru has left it,
   one instruction each, so no interrupt splits them.  Nothing in this image
   touches it: ami_crash_remove_alert_hook() waits for zero, after which the
   image can be unloaded under nobody.  Only its address is taken, from C, so
   it needs none of the global linkage the asm()'s own symbols do. */
static volatile ULONG ami_alert_inflight;

/* A removal is waiting for ami_alert_inflight; an install waits for it to
   finish first, so the two never overlap (F-080). */
static volatile BOOL ami_alert_draining;

/* The trampoline's asm() pushes the alert number on the stack and jsr's
   this, so it must read 4(sp) -- see aminetxduo/asm_abi.h. */
AMIGA_ASM_ARGS VOID ami_alert_report(ULONG num);
VOID ami_alert_trampoline(VOID);

__asm__(
"       .text                                   \n"
"       .globl  _ami_alert_trampoline           \n"
"_ami_alert_trampoline:                         \n"
"       movem.l %d0-%d1/%a0-%a1,-(%sp)          \n"  /* scratch regs only    */
"       move.l  %d7,-(%sp)                      \n"  /* alert number as arg  */
"       jsr     _ami_alert_report               \n"
"       addq.l  #4,%sp                          \n"
"       movem.l (%sp)+,%d0-%d1/%a0-%a1          \n"
"       rts                                     \n"  /* to the exit the stub */
                                                        /* pushed on the way in */
);

const char *ami_crash_alert_name(ULONG num)
{
    switch (num & 0x7FFFFFFFUL)
    {
        case 0x01000001: return "68000 exception vector checksum";
        case 0x01000002: return "ExecBase checksum";
        case 0x01000003: return "library checksum failure";
        case 0x01000005: return "corrupt memory list detected in FreeMem";
        case 0x01000006: return "no memory for interrupt servers";
        case 0x01000007: return "InitStruct() of an APTR source";
        case 0x01000008: return "semaphore in an illegal state";
        case 0x01000009: return "FREEING MEMORY ALREADY FREED (double free)";
        case 0x0100000A: return "illegal 68k exception taken";
        case 0x0100000B: return "attempt to reuse an active IORequest";
        case 0x0100000C: return "sanity check on memory list failed";
        case 0x0100000D: return "IO attempted on a closed IORequest";
        case 0x0100000E: return "stack appears to extend out of range";
        case 0x0100000F: return "memory header not found (bad FreeMem address)";
        case 0x01000010: return "illegal Remove() of a node";
        default:         break;
    }
    if ((num & 0x00FF0000UL) == 0x00010000UL)
        return "out of memory";
    return "see exec/alerts.h";
}

/*
 * WHAT A GURU MAY DO HERE (F-081).  Alert() is called from wherever Exec
 * found the fault: an interrupt, the middle of FreeMem() with Forbid() held
 * and the memory list it just called corrupt, a plain Task with no DOS.  The
 * report used to Open() a file there, which needs a Process, may Wait() and
 * so breaks the Forbid() the caller relied on, and allocates from the list
 * that is broken -- a second Guru inside the first.
 *
 * No context proves otherwise: a double free is reported from a Process in
 * user mode with nothing held, and the allocator behind Open() is the thing
 * that is broken.  So the report does only what is safe anywhere: it records
 * the alert in static storage and prints it with RawDoFmt()/RawPutChar(),
 * which allocate nothing and never wait.  The file, and AMI_ERROR() with the
 * log hook behind it, wait for ami_crash_remove_alert_hook(), after its
 * drain, in the Process that removes the hook.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define AMI_ALERT_NAME_LEN  32

static struct
{
    ULONG   num;
    APTR    task;
    char    name[AMI_ALERT_NAME_LEN];
    BOOL    pending;                    /* recorded, not yet in crash.txt    */
    ULONG   seq;                        /* bumped by every record            */
} ami_alert_rec;

static volatile BOOL ami_alert_reporting;   /* a Guru inside the report      */

static VOID ami_alert_put(register UBYTE c __asm("d0"),
                          register APTR data __asm("a3"))
{
    (VOID)data;
    if (c != '\0')
        RawPutChar(c);
}

static VOID ami_alert_say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, (APTR)args, (void (*)())ami_alert_put, NULL);
    va_end(args);
}

/* The deferred half: the log, with its hook, and DH0:crash.txt.  Only from
   ami_crash_remove_alert_hook().  A record the file could not take stays
   pending for the next removal. */
static VOID ami_alert_flush(VOID)
{
    ULONG num;
    APTR  task;
    ULONG seq;
    char  name[AMI_ALERT_NAME_LEN];
    ULONG i;
    BPTR  fh;

    /* A copy: a Guru can land while this writes, and it would otherwise be
       written half over the older one and then forgotten. */
    Disable();
    num  = ami_alert_rec.num;
    task = ami_alert_rec.task;
    seq  = ami_alert_rec.seq;
    for (i = 0; i < AMI_ALERT_NAME_LEN; i++)
        name[i] = ami_alert_rec.name[i];
    Enable();

    AMI_ERROR("*** GURU %08lx: %s", (LONG)num, (LONG)ami_crash_alert_name(num));
    AMI_ERROR("    task %08lx \"%s\"", (LONG)task, (LONG)name);

    fh = Open((STRPTR)"DH0:crash.txt", MODE_NEWFILE);
    if (fh == 0)
        return;                         /* still pending: the next removal */

    FPuts(fh, (STRPTR)"GURU: ");
    FPuts(fh, (STRPTR)ami_crash_alert_name(num));
    FPuts(fh, (STRPTR)"\n");
    Close(fh);

    /* Done only if no newer Guru replaced the record meanwhile. */
    Disable();
    if (ami_alert_rec.seq == seq)
        ami_alert_rec.pending = FALSE;
    Enable();
}

/* `used': the only caller is the `jsr _ami_alert_report' in the trampoline's
   asm() above. Not static, which is not protection -- a whole-program view is
   entitled to privatise and then drop it. */
AMIGA_ASM_ARGS VOID ami_alert_report(ULONG num) __attribute__((used));
AMIGA_ASM_ARGS VOID ami_alert_report(ULONG num)
{
    struct Task *task = SysBase->ThisTask;
    const char  *name = "?";
    ULONG        i;

    /* A Guru raised by this report itself: record nothing more, go on. */
    if (ami_alert_reporting)
        return;
    ami_alert_reporting = TRUE;

    if (task != NULL && task->tc_Node.ln_Name != NULL)
        name = (const char *)task->tc_Node.ln_Name;

    ami_alert_rec.num  = num;
    ami_alert_rec.task = (APTR)task;
    for (i = 0; i + 1 < AMI_ALERT_NAME_LEN && name[i] != '\0'; i++)
        ami_alert_rec.name[i] = name[i];
    ami_alert_rec.name[i] = '\0';
    ami_alert_rec.seq++;
    ami_alert_rec.pending = TRUE;

    ami_alert_say("[ERR ] *** GURU %08lx: %s\n", (LONG)num,
                  (LONG)ami_crash_alert_name(num));
    ami_alert_say("[ERR ]     task %08lx \"%s\"\n", (LONG)task,
                  (LONG)ami_alert_rec.name);

    ami_alert_reporting = FALSE;
}

/*
 * WHO OWNS THE VECTOR (F-080).  Alert() is Exec's, and another image -- the
 * library and a tool, two tools -- patches it the same way.  Taken out of
 * order, a hook put Exec's own Alert() back over the other image's, and that
 * image, removing later, put back this one's trampoline: in code that had
 * been unloaded, reached by the next Guru.  Checking before restoring is not
 * enough on its own: the image above has saved this one's address, and calls
 * it whether or not the vector still does.
 *
 * So the vector points at a stub in public memory that outlives the image:
 *
 *     +0   addq.l  #1,ami_alert_inflight   counted, then into the image
 *     +6   pea     EXIT(pc)                this Guru's own way out
 *     +10  move.l  TARGET(pc),-(sp)
 *     +14  rts                             to the trampoline
 *     +16  TARGET: this image's trampoline, never changed
 *     +20  OLD:    what the vector held before
 *     +24  EXIT:   subq.l  #1,ami_alert_inflight
 *     +30          move.l  OLD(pc),-(sp)
 *     +34          rts                     to the Alert() below this one
 *     +36  PASS:   move.l  OLD(pc),-(sp)
 *     +40          rts
 *
 * The trampoline returns to EXIT, so a Guru is counted out only after it has
 * left the image, and through the stub it came in by.  Taking the hook out
 * rewrites the first word as bra.s PASS: a Guru that has not yet run the
 * addq passes through uncounted, and one that has goes on to the trampoline,
 * still there, and out through EXIT.  Removal then waits for the count to
 * reach zero, and an install waits for that.  The vector is put back only if
 * it still points at the stub.  The stub is never freed: another image may
 * hold its address.
 */
#define AMI_ALERT_STUB_BYTES    42
#define AMI_ALERT_STUB_TARGET   16      /* byte offsets                      */
#define AMI_ALERT_STUB_OLD      20
#define AMI_ALERT_STUB_EXIT     24
#define AMI_ALERT_STUB_PASS     36

static UWORD *ami_alert_stub;

static APTR *ami_alert_stub_cell(UWORD *stub, ULONG offset)
{
    return (APTR *)((UBYTE *)stub + offset);
}

/* A PC-relative displacement: from the extension word at `ext' to `to'. */
static UWORD ami_alert_disp(ULONG ext, ULONG to)
{
    return (UWORD)(to - ext);
}

static VOID ami_alert_stub_build(UWORD *stub)
{
    stub[0]  = 0x52B9;                              /* addq.l #1,(abs).l     */
    *ami_alert_stub_cell(stub, 2) = (APTR)&ami_alert_inflight;
    stub[3]  = 0x487A;                              /* pea (d16,pc)          */
    stub[4]  = ami_alert_disp(8, AMI_ALERT_STUB_EXIT);
    stub[5]  = 0x2F3A;                              /* move.l (d16,pc),-(sp) */
    stub[6]  = ami_alert_disp(12, AMI_ALERT_STUB_TARGET);
    stub[7]  = 0x4E75;                              /* rts                   */
    *ami_alert_stub_cell(stub, AMI_ALERT_STUB_TARGET) =
        (APTR)ami_alert_trampoline;
    *ami_alert_stub_cell(stub, AMI_ALERT_STUB_OLD) = NULL;
    stub[12] = 0x53B9;                              /* subq.l #1,(abs).l     */
    *ami_alert_stub_cell(stub, AMI_ALERT_STUB_EXIT + 2) =
        (APTR)&ami_alert_inflight;
    stub[15] = 0x2F3A;                              /* move.l (d16,pc),-(sp) */
    stub[16] = ami_alert_disp(32, AMI_ALERT_STUB_OLD);
    stub[17] = 0x4E75;                              /* rts                   */
    stub[18] = 0x2F3A;                              /* PASS: move.l ...      */
    stub[19] = ami_alert_disp(38, AMI_ALERT_STUB_OLD);
    stub[20] = 0x4E75;                              /* rts                   */
}

BOOL ami_crash_install_alert_hook(VOID)
{
    UWORD *stub;
    APTR   old;

    if (ami_alert_stub != NULL)
        return TRUE;

    /* Allocated first: AllocMem() cannot be called with interrupts off. */
    stub = (UWORD *)AllocMem(AMI_ALERT_STUB_BYTES, MEMF_PUBLIC);
    if (stub == NULL)
        return FALSE;
    ami_alert_stub_build(stub);
    CacheClearU();

    /* The check, the patch and the publishing are one step, so a second
       installer in this image finds the hook in, not half of it; and none
       starts while a removal is still draining. */
    for (;;)
    {
        Disable();
        if (ami_alert_stub != NULL)
        {
            Enable();
            FreeMem(stub, AMI_ALERT_STUB_BYTES);     /* never published */
            return TRUE;
        }
        if (!ami_alert_draining)
            break;
        Enable();
        Delay(1);
    }
    old = SetFunction((struct Library *)SysBase, -108, (APTR)stub);
    *ami_alert_stub_cell(stub, AMI_ALERT_STUB_OLD) = old;
    ami_alert_stub = stub;
    Enable();

    return TRUE;
}

VOID ami_crash_remove_alert_hook(VOID)
{
    UWORD *stub;
    APTR   old;
    APTR   was;

    /*
     * With no hook to take, this removal still waits out another one's drain,
     * so its caller does not unload the image under it, and then writes a
     * record still pending, owning the drain while it does.  With a hook,
     * taking the stub, restoring the vector and closing the stub's counted
     * way in are one step.
     */
    for (;;)
    {
        Disable();
        stub = ami_alert_stub;
        if (stub != NULL)
            break;
        if (!ami_alert_draining)
        {
            if (!ami_alert_rec.pending)
            {
                Enable();
                return;
            }
            ami_alert_draining = TRUE;
            Enable();
            ami_alert_flush();
            ami_alert_draining = FALSE;
            return;
        }
        Enable();
        Delay(1);
    }
    ami_alert_stub     = NULL;
    ami_alert_draining = TRUE;

    old = *ami_alert_stub_cell(stub, AMI_ALERT_STUB_OLD);
    was = SetFunction((struct Library *)SysBase, -108, old);
    if (was != (APTR)stub)
        (VOID)SetFunction((struct Library *)SysBase, -108, was);
    stub[0] = (UWORD)(0x6000 | (AMI_ALERT_STUB_PASS - 2)); /* bra.s PASS   */
    CacheClearU();
    Enable();

    /* A Guru already counted in, perhaps still in the report, leaves through
       EXIT before this returns and the image can be unloaded.  Callers are
       Processes: the flush below uses DOS. */
    while (ami_alert_inflight != 0)
        Delay(1);

    /* A Guru's log and file, deferred from where it happened (F-081).  Still
       draining while they run: they are this image's code, and a second
       removal must not return, and its caller unload the image, under them. */
    if (ami_alert_rec.pending)
        ami_alert_flush();

    ami_alert_draining = FALSE;
}
