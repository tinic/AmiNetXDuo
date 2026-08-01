/*
 * __wrap_main -- give a ported Unix client a real POSIX argv[] and a big stack.
 *
 * This toolchain's newlib crt0 does not turn the CLI command line into an
 * argv[]: on the Shell path it hands main() argc = 1 and a single "argv" that
 * is the whole raw argument string (and, before tools/fix-toolchain-crt0.py
 * repaired the indirection, the address of that pointer rather than the
 * pointer).  AmiNetXDuo's own commands never noticed -- they read their
 * arguments through ReadArgs() and only look at argc -- but curl and Dropbear
 * parse argv and nothing else, so every invocation comes back as "no URL" /
 * "no host", or dereferences the garbage and crashes.
 *
 * -Wl,--wrap=main (clients/amiga-client.sh) routes the crt0's call to main()
 * through here, leaving the client's real main() reachable as __real_main().
 * argv is built from dos.library -- GetProgramName() for argv[0], GetArgStr()
 * for the tail -- split on whitespace with "..." grouping and the '*' escape
 * AmigaDOS uses inside quotes.
 *
 * GetArgStr() is the same command tail ReadArgs() would parse, so this stays
 * correct even if a later toolchain crt0 tokenises argv itself: the same
 * answer is rebuilt either way.  Compiled into libamigaclient, which every
 * ported client links.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
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
 * A ported client needs far more stack than the Shell's 4 KB default -- curl's
 * TLS work and Dropbear's bignum key exchange both run deep -- so the same
 * shim that fixes argv also brings the stack, and the user never has to type
 * `stack 200000` first.  256 KB covers both.
 */
#define AMIGA_ARGV_STACK    (256UL * 1024UL)

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
 * 256 KB is a guess that has never been checked, and it is 25% of the smallest
 * machine this runs on.  Painting the block with a pattern and finding the
 * lowest word still holding it gives the high-water mark: the stack grows down
 * from stk_Upper, so everything below the deepest frame is untouched.
 *
 * Off unless the environment variable is set, because it costs a pass over
 * 256 KB at startup and a line of output that a client's caller did not ask
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
 * exit() unwinds through the swapped stack.
 *
 * A client that ends by calling exit() rather than returning from main() --
 * Dropbear always, curl on some paths -- never comes back to
 * argv_run_on_stack(), so its second StackSwap() does not run.  The crt0's exit
 * restores the stack pointer from its own saved copy, but not tc_SPLower/
 * tc_SPUpper: the task is left advertising the swapped, about-to-be-abandoned
 * stack as its bounds, and the next stack check sees the pointer outside them
 * and traps with an illegal instruction right at the end, after everything
 * appeared to work.
 *
 * StackSwap() cannot put them back from here: it moves its own return address
 * onto the stack it swaps to, and the frames between an exit() call site and
 * this point live on the stack being abandoned, so the returns would unwind
 * through the wrong memory.  Restore the two bounds directly instead, from what
 * the first StackSwap() saved into argv_sss; the pointer itself is the crt0's
 * to restore.  -Wl,--wrap=exit,--wrap=_exit routes both here.
 *
 * The same path also loses the stack itself.  AmigaOS does not reclaim
 * AllocMem() memory when a process exits, and the FreeMem() in __wrap_main()
 * sits after argv_run_on_stack() returns -- which it never does on an exit().
 * That is 256 KB per invocation of every client that ends this way, gone until
 * reboot, on a machine whose supported floor is 1 MB.
 *
 * So the exit wrappers longjmp() back into __wrap_main(), which is still on the
 * caller's stack, and the free happens there.
 *
 * BOTH wrappers, not only __wrap__exit().  This crt0 defines exit(), _exit()
 * and __exit() as three names for ONE function -- `nm` puts all three at the
 * same address -- so a client that calls exit() never makes a second call that
 * the linker can see and redirect.  --wrap=_exit gives a wrapper that is never
 * reached, and an earlier version of this file put the longjmp only there:
 * measured on an emulated A1200, every dbclient run still lost 266,368 bytes,
 * the same figure five times running.
 *
 * stdio is flushed here rather than left to the crt0, because after the
 * longjmp we are on the caller's stack -- 4 KB under a Shell -- and the flush
 * wants the big one.  What is left to run then is the atexit() handlers and
 * the return to DOS, which __real__exit() does from __wrap_main().
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

/* Only the Task that ran setjmp() may jump back into its frame; the console
   reader child in clients/dropbear has its own.  Returns on a task that may
   not, and the caller then takes the real exit. */
static VOID argv_exit_via_main(int status)
{
    if (argv_exit_task == NULL || argv_exit_task != FindTask(NULL))
        return;

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

    (void)argc_ignored;
    (void)argv_ignored;

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
        /* argv[0] -- the program name, as the Shell knows it. */
        argv_name[0] = '\0';
        (void)GetProgramName((STRPTR)argv_name, (LONG)sizeof(argv_name));
        argv_vec[argc++] = argv_name;

        /* argv[1..] -- the argument tail, tokenised in a private buffer. */
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
     * Run the client on a stack of our own.  On a machine too short of memory
     * to spare 256 KB, fall back to the caller's stack rather than refuse: a
     * small program may still fit.
     */
    argv_stack = AllocMem(AMIGA_ARGV_STACK, MEMF_ANY);
    if (argv_stack != NULL)
    {
        argv_sss.stk_Lower   = argv_stack;
        argv_sss.stk_Upper   = (ULONG)argv_stack + AMIGA_ARGV_STACK;
        argv_sss.stk_Pointer = (APTR)((ULONG)argv_stack + AMIGA_ARGV_STACK);

        /* setjmp() here, so a client that exits instead of returning comes
           back to this frame -- on the caller's stack -- and the FreeMem()
           below runs either way.  argv_stack is static because a local
           written before setjmp() is not guaranteed to survive the longjmp. */
        if (argv_stackcheck_wanted())
            argv_paint();

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

    return argv_result;
}
