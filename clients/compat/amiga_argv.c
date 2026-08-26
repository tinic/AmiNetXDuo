/*
 * __wrap_main, give a ported Unix client a real POSIX argv[] and a big stack.
 *
 * This toolchain's newlib crt0 does not turn the CLI command line into an
 * argv[], so -Wl,--wrap=main routes the crt0's call through here and argv is
 * built from GetProgramName()/GetArgStr(), the same tail ReadArgs() parses.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>   /* AttnFlags, for ami_rt_cpu_select() */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <setjmp.h>
#include <stdio.h>

extern int __real_main(int argc, char **argv);

/* Generous, and static: this runs before the client's main() sets anything up,
   on whatever stack the crt0 provided.  A command line longer than this is
   truncated rather than overrun. */
#define AMIGA_ARGV_MAX      256
#define AMIGA_ARGV_BUFSIZE  8192
#define AMIGA_ARGV_NAMESIZE 256

static char  argv_name[AMIGA_ARGV_NAMESIZE];
static char  argv_buf[AMIGA_ARGV_BUFSIZE];
static char *argv_vec[AMIGA_ARGV_MAX + 1];

/*
 * A ported client needs more stack than the Shell's 4 KB default.  8 KB,
 * measured: dbclient's high-water is 5,008 bytes, and AMIGA_ARGV_STACKCHECK
 * below is what measures it.  Re-measure before adding a second client.
 */
#define AMIGA_ARGV_STACK    (8UL * 1024UL)

static struct StackSwapStruct argv_sss;
static int                    argv_argc;
static int                    argv_result;
static BOOL                   argv_on_swapped;   /* TRUE between the swaps */
static APTR                   argv_stack;        /* the swapped stack, to free */

/* The way back onto the caller's stack when the client exits rather than
   returns; see the note above __wrap__exit(). */
static jmp_buf                argv_exit_jmp;
static struct Task           *argv_exit_task;    /* who set it, NULL when unset */
static int                    argv_exit_status;

/*
 * How much of the stack the client actually used.
 *
 * What sized AMIGA_ARGV_STACK, and what has to re-size it for a new client.
 * Painting the block and finding the lowest word still holding the pattern
 * gives the high-water mark: the stack grows down from stk_Upper, so
 * everything below the deepest frame is untouched.
 *
 * Off unless the environment variable is set, because it costs a pass over the
 * block at startup and a line of output that a client's caller did not ask
 * for.  `setenv AMIGA_ARGV_STACKCHECK 1` turns it on for a Shell.
 */
#define ARGV_PAINT          0xA5A5A5A5UL
#define ARGV_STACKCHECK_VAR "AMIGA_ARGV_STACKCHECK"

static BOOL                   argv_painted;

static BOOL argv_stackcheck_wanted(VOID)
{
    char one[2];

    return (GetVar((CONST_STRPTR)ARGV_STACKCHECK_VAR, (STRPTR)one,
                   (LONG)sizeof(one), 0) > 0) ? TRUE : FALSE;
}

static VOID argv_paint(VOID)
{
    ULONG *word = (ULONG *)argv_stack;
    ULONG  left = AMIGA_ARGV_STACK / sizeof(ULONG);

    while (left-- > 0)
        *word++ = ARGV_PAINT;

    argv_painted = TRUE;
}

/*
 * Where a run's memory goes, under the same variable.
 *
 * A ported client loses a constant amount per invocation that is not the stack
 * 4,224 bytes for dbclient, the same for `dbclient -V`, which opens no
 * socket.  Three readings bracket it: entering __wrap_main, the moment before
 * __real_main, and after the stack has been freed.  Whatever is missing
 * between the last of those and what the parent sees is the crt0's teardown
 * rather than anything here.
 */
static VOID argv_mem(const char *where)
{
    if (argv_stackcheck_wanted())
        Printf("[argv: mem %s %ld]\n", (LONG)where, (LONG)AvailMem(MEMF_ANY));
}

/* Called with the client finished and the stack not yet freed, on either the
   return or the longjmp path. */
static VOID argv_report_high_water(VOID)
{
    const ULONG *word = (const ULONG *)argv_stack;
    ULONG        left = AMIGA_ARGV_STACK / sizeof(ULONG);
    ULONG        untouched = 0;

    if (!argv_painted)
        return;

    argv_painted = FALSE;
    while (untouched < left && word[untouched] == ARGV_PAINT)
        untouched++;

    Printf("[argv: stack high-water %ld of %ld bytes]\n",
           (LONG)(AMIGA_ARGV_STACK - untouched * sizeof(ULONG)),
           (LONG)AMIGA_ARGV_STACK);
}

/*
 * Run __real_main() on the swapped stack.  No locals, no arguments, and
 * noinline: between the two StackSwap() calls the stack pointer is the new
 * stack's, so anything this function touched on the old one would be the wrong
 * memory.  Everything it needs is static, the same discipline
 * src/tools/fetch.c documents.
 */
static __attribute__((noinline)) VOID argv_run_on_stack(VOID)
{
    argv_on_swapped = TRUE;
    StackSwap(&argv_sss);
    argv_result = __real_main(argv_argc, argv_vec);
    StackSwap(&argv_sss);
    argv_on_swapped = FALSE;
}

/*
 * exit() never returns to argv_run_on_stack(), so its second StackSwap() does
 * not run: tc_SPLower/tc_SPUpper are restored by hand from argv_sss and both
 * exit wrappers longjmp() back into __wrap_main(), which frees the stack.
 */
extern void __real_exit(int status);
extern void __real__exit(int status);

static VOID argv_restore_bounds(VOID)
{
    if (argv_on_swapped)
    {
        struct Task *self = FindTask(NULL);

        argv_on_swapped   = FALSE;
        self->tc_SPLower  = argv_sss.stk_Lower;
        self->tc_SPUpper  = (APTR)argv_sss.stk_Upper;
    }
}

/*
 * atexit() does nothing on this crt0, so run the handlers here.
 *
 * exit(), _exit() and __exit() are one function at one address (see below),
 * and it terminates without walking the atexit list, measured, not read: a
 * Printf() put inside amiga_dropbear.c's amiga_sock_cleanup(), which is
 * registered with atexit(), never appeared on any run.
 *
 * That is not cosmetic.  Dropbear registers two handlers that way.  One closes
 * bsdsocket.library, so every ssh left a phantom opener the library could
 * never expunge and lost 4,224 bytes of the machine until reboot; the other
 * stops the console reader child, whose own comment says AmigaOS reclaims
 * neither its structure nor its two signal bits.
 *
 * Nothing else calls them, the leak was the same size before the longjmp
 * below existed and the exit went straight through __real_exit(), so calling
 * them here runs each exactly once.  Newlib walks its own list and empties it,
 * so a second call is a no-op regardless.
 */
extern void __call_exitprocs(int status, void *dso);

/* Only the Task that ran setjmp() may jump back into its frame; the console
   reader child in clients/dropbear has its own.  Returns on a task that may
   not, and the caller then takes the real exit. */
static VOID argv_exit_via_main(int status)
{
    if (argv_exit_task == NULL || argv_exit_task != FindTask(NULL))
        return;

    /* Before the flush: a handler may still write, and before the jump because
       a handler wants the big stack rather than the caller's 4 KB. */
    __call_exitprocs(status, NULL);
    fflush(NULL);

    argv_exit_task   = NULL;
    argv_exit_status = status;
    longjmp(argv_exit_jmp, 1);
}

void __wrap_exit(int status)
{
    argv_restore_bounds();
    argv_exit_via_main(status);
    __real_exit(status);
}

void __wrap__exit(int status)
{
    argv_restore_bounds();
    argv_exit_via_main(status);
    __real__exit(status);
}

static int argv_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

int __wrap_main(int argc_ignored, char **argv_ignored)
{
    struct Process *proc = (struct Process *)FindTask(NULL);
    const char     *args;
    int             argc = 0;
    ULONG           w = 0;              /* write cursor into argv_buf   */
    ULONG           r;                  /* read cursor into args        */
    int             exited;

    argv_mem("enter");

    (void)argc_ignored;
    (void)argv_ignored;

    /* The compiler runtime's CPU choice, before anything this program does.
       A ported client is -m68000 code -- one binary for every 68k -- so every
       32-bit multiply and divide is a call into src/common/ami_udivdi3.c, and
       those five have a one-instruction form from the 68020 up.  It is the
       whole difference between a 5 s ssh handshake on an A1200 and a slower
       one; declared here rather than included because this file is compat
       glue and pulls in no headers of ours. */
    {
        extern void ami_rt_cpu_select(int have_68020, int have_mulul);

        /* Two facts, not one: the 68060 has the 68020 instructions but not
           MULU.L's 64-bit result, which is the one the bignum inner loop
           wants.  Same split as src/crypto68k/c68k_variant.h. */
        ami_rt_cpu_select((SysBase->AttnFlags & AFF_68020) != 0,
                          (SysBase->AttnFlags & AFF_68020) != 0 &&
                          (SysBase->AttnFlags & AFF_68060) == 0);
    }

    /*
     * A Workbench launch has no CLI, and GetArgStr()/GetProgramName() are a
     * CLI's.  A ported client cannot do anything useful from Workbench anyway,
     * so hand main() an empty argv rather than touch dos.library without one.
     */
    if (proc == NULL || proc->pr_CLI == 0)
    {
        argv_vec[0] = (char *)"";
        argc = 1;
    }
    else
    {
        /* argv[0], the program name, as the Shell knows it. */
        argv_name[0] = '\0';
        (void)GetProgramName((STRPTR)argv_name, (LONG)sizeof(argv_name));
        argv_vec[argc++] = argv_name;

        /* argv[1..], the argument tail, tokenised in a private buffer. */
        args = (const char *)GetArgStr();
        if (args != NULL)
        {
            r = 0;
            while (argc < AMIGA_ARGV_MAX && w < AMIGA_ARGV_BUFSIZE - 1)
            {
                while (argv_is_space(args[r]))
                    r++;
                if (args[r] == '\0')
                    break;

                argv_vec[argc++] = &argv_buf[w];

                if (args[r] == '"')
                {
                    /* Quoted: spaces are literal, '*' is the escape. */
                    r++;
                    while (args[r] != '\0' && args[r] != '"' &&
                           w < AMIGA_ARGV_BUFSIZE - 1)
                    {
                        char c = args[r++];

                        if (c == '*' && args[r] != '\0')
                        {
                            char e = args[r++];
                            c = (e == 'n' || e == 'N') ? '\n'
                              : (e == 'e' || e == 'E') ? (char)0x1b
                              :                          e;   /* *", **, ... */
                        }

                        argv_buf[w++] = c;
                    }
                    if (args[r] == '"')
                        r++;
                }
                else
                {
                    while (args[r] != '\0' && !argv_is_space(args[r]) &&
                           w < AMIGA_ARGV_BUFSIZE - 1)
                        argv_buf[w++] = args[r++];
                }

                argv_buf[w++] = '\0';
            }
        }
    }

    argv_vec[argc] = NULL;
    argv_argc      = argc;

    /*
     * Run the client on a stack of our own.  On a machine that cannot spare
     * even this, fall back to the caller's stack rather than refuse: a small
     * program may still fit.
     */
    argv_stack = AllocMem(AMIGA_ARGV_STACK, MEMF_ANY);
    if (argv_stack != NULL)
    {
        argv_sss.stk_Lower   = argv_stack;
        argv_sss.stk_Upper   = (ULONG)argv_stack + AMIGA_ARGV_STACK;
        argv_sss.stk_Pointer = (APTR)((ULONG)argv_stack + AMIGA_ARGV_STACK);

        /* setjmp() here, so a client that exits instead of returning comes
           back to this frame, on the caller's stack, and the FreeMem()
           below runs either way.  argv_stack is static because a local
           written before setjmp() is not guaranteed to survive the longjmp. */
        if (argv_stackcheck_wanted())
            argv_paint();

        argv_mem("pre-main");

        exited = setjmp(argv_exit_jmp);
        if (exited == 0)
        {
            argv_exit_task = FindTask(NULL);
            argv_run_on_stack();
        }
        argv_exit_task = NULL;

        argv_report_high_water();
        FreeMem(argv_stack, AMIGA_ARGV_STACK);
        argv_stack = NULL;
        argv_mem("post-main");

        /* Back from an exit(): everything newlib had left to do has been done
           on the big stack, so finish the termination rather than return a
           status the client did not ask for. */
        if (exited != 0)
            __real__exit(argv_exit_status);
    }
    else
    {
        argv_result = __real_main(argv_argc, argv_vec);
    }

    /* The other way out: a client that RETURNED from main() never went through
       the exit wrappers, and the crt0 it returns into does not walk the atexit
       list either.  Same call, and newlib empties the list as it runs it, so a
       client that took both paths does not run a handler twice. */
    __call_exitprocs(argv_result, NULL);

    return argv_result;
}
