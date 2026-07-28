/*
 * AmiNetXDuo -- crash guard implementation.
 *
 * How a CPU exception reaches us:
 *   1. The 68k takes the exception and enters supervisor mode.
 *   2. Exec's exception handler pushes the trap number as a longword on the
 *      supervisor stack and jumps to the task's tc_TrapCode.
 *   3. Above that longword sits the exception frame: SR.w, PC.l, and on 68010+
 *      a format/vector word.
 *
 * The handler saves the registers, records the frame, then rewrites the return
 * PC in the frame to point at a user-mode bail-out routine before executing
 * RTE. The processor drops back to user mode running the recovery code instead
 * of re-executing the faulting instruction, so the process can report and exit
 * rather than taking the machine down.
 *
 * This assumes the exception happened in user mode. If SR in the frame has the
 * supervisor bit set there is nothing safe to return to, so no recovery is
 * attempted.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/crashguard.h"
#include "aminetxduo/compat.h"

#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <setjmp.h>

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


/* One-line summary, for the DH0:crash.txt breadcrumb. */
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

/* Runs in user mode, on the crashing task's stack, straight after the RTE. */
VOID ami_crash_bailout(VOID)
{
    int i;

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
         * No %c here: RawDoFmt consumes a *word* for %c but the C caller pushes
         * a longword, which misaligns every argument after it. Only %ld/%lx/%s
         * (all longword) are safe in this formatter.
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
     * Leave a breadcrumb the host can read even if the unwind below fails --
     * the harness stages DH0: from a host directory, so this file appears
     * outside the emulator immediately.
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
     * KNOWN LIMITATION: this does not reliably return control. Recovery from a
     * patched exception frame lands here in user mode with the crashing task's
     * stack pointer, and the setjmp context is not always honoured from that
     * state -- observed under FS-UAE resuming after the call site rather than
     * at it. The *report* above is dependable; the resume is not, so callers
     * must treat a caught crash as fatal and never rely on continuing.
     */
    ami_crash_task->tc_TrapCode = ami_crash_old_trap;
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

/* ------------------------------------------------------- exec Alert hook -- */

/*
 * A Guru is not a CPU exception. When Exec detects corruption it calls its own
 * Alert() -- LVO -108, with the alert number in d7 -- which never goes near
 * tc_TrapCode, so the trap handler above cannot see a double free, a corrupt
 * memory list or a reused IORequest.
 *
 * SetFunction() on exec's Alert vector catches them. The trampoline logs and
 * then tail-jumps to the original, so normal Guru behaviour is unchanged.
 *
 * This patches Exec machine-wide, so it is a debugging aid: install it in
 * tests and tools under the emulator, and always remove it before exiting.
 */

static APTR ami_alert_old;

VOID ami_alert_report(ULONG num);
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
"       move.l  _ami_alert_old,-(%sp)           \n"  /* tail-jump to the     */
"       rts                                     \n"  /* original Alert()     */
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

VOID ami_alert_report(ULONG num)
{
    struct Task *task = FindTask(NULL);

    AMI_ERROR("*** GURU %08lx: %s", (LONG)num, (LONG)ami_crash_alert_name(num));
    AMI_ERROR("    task %08lx \"%s\"", (LONG)task,
              (LONG)((task != NULL && task->tc_Node.ln_Name != NULL)
                         ? (const char *)task->tc_Node.ln_Name : "?"));

    {
        BPTR fh = Open((STRPTR)"DH0:crash.txt", MODE_NEWFILE);

        if (fh != 0)
        {
            FPuts(fh, (STRPTR)"GURU: ");
            FPuts(fh, (STRPTR)ami_crash_alert_name(num));
            FPuts(fh, (STRPTR)"\n");
            Close(fh);
        }
    }
}

BOOL ami_crash_install_alert_hook(VOID)
{
    if (ami_alert_old != NULL)
        return TRUE;

    Forbid();
    ami_alert_old = SetFunction((struct Library *)SysBase, -108,
                                (APTR)ami_alert_trampoline);
    Permit();

    return ami_alert_old != NULL;
}

VOID ami_crash_remove_alert_hook(VOID)
{
    if (ami_alert_old == NULL)
        return;

    Forbid();
    (VOID)SetFunction((struct Library *)SysBase, -108, ami_alert_old);
    Permit();
    ami_alert_old = NULL;
}
