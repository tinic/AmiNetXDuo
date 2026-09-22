/*
 * SelfCause: does a software interrupt that Cause()s ITSELF from inside its
 * own handler run a second time?
 *
 * The netdev driver's bottom half (netdev_soft, src/netdev/netdev_device.c)
 * depends on the answer.  Our reading of exec is that the dispatcher puts
 * ln_Type back to NT_INTERRUPT before it calls is_Code, so a Cause() during
 * the run re-queues the node and the handler runs again: count 2.  This
 * asks the ROM.
 *
 * The control is the documented rule: two Cause() calls while the interrupt
 * is still PENDING (under Disable(), so it cannot run in between) queue it
 * once, count +1.  A rig that cannot show that cannot be trusted on the
 * first question either.
 *
 * exec.library and dos.library only; nothing of ours.  Output is key=value
 * for tests/tools/run-selfcause.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: SelfCause 1.0 (22.9.2026)";

struct SelfCause
{
    struct Interrupt sc_Int;
    volatile ULONG   sc_Runs;       /* every entry of the handler */
    volatile ULONG   sc_SelfCause;  /* 1 once the handler has re-Caused itself */
};

/* Delay() ticks of 1/50 s.  A software interrupt runs before Cause()
   returns to a user-mode caller, so this is rope, not a wait. */
#define SETTLE_TICKS 25

static VOID say(const char *fmt, LONG a, LONG b)
{
    LONG args[2];

    args[0] = a;
    args[1] = b;
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    Flush(Output());
}

/*
 * The register convention this tree uses for a software interrupt: is_Data in
 * A1.  Nothing else is shared with the task; the two counters are volatile.
 */
static ULONG selfcause_code(register struct SelfCause *sc __asm("a1"))
{
    sc->sc_Runs++;
    if (sc->sc_Runs == 1 && sc->sc_SelfCause == 0)
    {
        sc->sc_SelfCause = 1;
        Cause(&sc->sc_Int);
    }
    return 0;
}

/* The ROM's own version word, at offset 12 of the 512 KB Kickstart at
   $F80000: this is the number the ROM label carries (40.68, 37.175), as
   distinct from exec's lib_Version (40.10, 37.175). */
#define KICK_ROM_BASE   0x00F80000UL

int main(void)
{
    struct SelfCause sc;
    struct ExecBase *eb = SysBase;
    const UWORD *rom = (const UWORD *)KICK_ROM_BASE;
    ULONG runs_first, runs_pending;
    LONG self_ok, pending_ok;
    const char *id;
    char idbuf[64];
    ULONG n;

    /* exec's id string, one line, the trailing \r\n dropped. */
    id = (const char *)eb->LibNode.lib_IdString;
    for (n = 0; n < sizeof(idbuf) - 1 && id != NULL && id[n] != '\0' &&
                id[n] != '\r' && id[n] != '\n'; n++)
        idbuf[n] = id[n];
    idbuf[n] = '\0';

    say("exec_id=%s\n", (LONG)idbuf, 0);
    say("exec_version=%ld.%ld\n", eb->LibNode.lib_Version,
                                   eb->LibNode.lib_Revision);
    say("kickstart_rom=%ld.%ld\n", rom[6], rom[7]);
    say("attn_flags=0x%lx\n", eb->AttnFlags, 0);

    sc.sc_Int.is_Node.ln_Type = NT_INTERRUPT;
    sc.sc_Int.is_Node.ln_Pri  = 0;
    sc.sc_Int.is_Node.ln_Name = (char *)"SelfCause";
    sc.sc_Int.is_Data         = &sc;
    sc.sc_Int.is_Code         = (VOID (*)())selfcause_code;
    sc.sc_Runs      = 0;
    sc.sc_SelfCause = 0;

    /* 1. Cause() once from the task; the handler Cause()s itself on its
          first run.  2 means the re-Cause was queued and ran. */
    Cause(&sc.sc_Int);
    Delay(SETTLE_TICKS);
    Delay(SETTLE_TICKS);
    runs_first = sc.sc_Runs;
    self_ok = (runs_first == 2);
    say("selfcause_runs=%ld\n", runs_first, 0);

    /* 2. The control.  Two Cause() while it is pending and cannot run: the
          second must find ln_Type == NT_SOFTINT and do nothing, so the
          count advances by exactly one. */
    Disable();
    Cause(&sc.sc_Int);
    Cause(&sc.sc_Int);
    Enable();
    Delay(SETTLE_TICKS);
    Delay(SETTLE_TICKS);
    runs_pending = sc.sc_Runs - runs_first;
    pending_ok = (runs_pending == 1);
    say("selfcause_pending_runs=%ld\n", runs_pending, 0);
    say("selfcause_pending_suppressed=%ld\n", pending_ok, 0);

    say("selfcause=%s\n", (LONG)(self_ok && pending_ok ? "PASS" : "FAIL"), 0);

    return (self_ok && pending_ok) ? RETURN_OK : RETURN_FAIL;
}
