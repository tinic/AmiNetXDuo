/*
 * __wrap_main -- give a ported Unix client a real POSIX argv[] and a big stack.
 *
 * THE PROBLEM
 *
 *   This toolchain's newlib crt0 does not turn the CLI command line into an
 *   argv[]: on the Shell path it hands main() argc = 1 and a single "argv"
 *   that is the whole raw argument string (and, before tools/fix-toolchain-
 *   crt0.py repaired the indirection, the ADDRESS of that pointer rather than
 *   the pointer).  AmiNetXDuo's own commands never noticed -- they read their
 *   arguments through ReadArgs() and only ever look at argc -- but curl and
 *   Dropbear parse argv and nothing else, so every invocation comes back as
 *   "no URL" / "no host", or dereferences the garbage and crashes.
 *
 * THE FIX
 *
 *   -Wl,--wrap=main (clients/amiga-client.sh) routes the crt0's call to main()
 *   through here, leaving the client's real main() reachable as __real_main().
 *   We build argv from dos.library -- GetProgramName() for argv[0], GetArgStr()
 *   for the tail -- split on whitespace with "..." grouping and the '*' escape
 *   AmigaDOS uses inside quotes, and hand the real main() the array it expects.
 *
 *   This is authoritative regardless of what the crt0 did: GetArgStr() is the
 *   same command tail ReadArgs() would parse, so it stays correct even if a
 *   later toolchain crt0 tokenises argv itself -- we simply rebuild the same
 *   answer.  Compiled into libamigaclient, so curl and Dropbear share it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

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

/*
 * Run __real_main() on the swapped stack.  NO locals and NO arguments, and
 * noinline on purpose: between the two StackSwap() calls the stack pointer is
 * the new stack's, so anything this function touched on the old one would be
 * the wrong memory.  Everything it needs is static -- the discipline
 * src/tools/fetch.c documents at length for exactly this.
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
 * exit() unwinds THROUGH the swapped stack.
 *
 * A client that ends by calling exit() rather than returning from main() --
 * Dropbear always, curl on some paths -- never comes back to
 * argv_run_on_stack(), so its second StackSwap() does not run.  The crt0's exit
 * restores the stack POINTER from its own saved copy, but not tc_SPLower/
 * tc_SPUpper: the task is left advertising the swapped (about-to-be-abandoned)
 * stack as its bounds, and the next stack check sees the pointer outside them
 * and traps -- an illegal instruction "right at the end", after everything
 * appeared to work.
 *
 * StackSwap() cannot put them back from here: it moves its OWN return address
 * onto the stack it swaps to, and the frames between an exit() call site and
 * this point live on the stack we are abandoning, so the returns would unwind
 * through the wrong memory.  Restore the two bounds directly instead, from what
 * the first StackSwap() saved into argv_sss -- the pointer itself is the crt0's
 * to restore.  -Wl,--wrap=exit,--wrap=_exit routes both here.
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

void __wrap_exit(int status)
{
    argv_restore_bounds();
    __real_exit(status);
}

void __wrap__exit(int status)
{
    argv_restore_bounds();
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
    APTR            stack;

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
     * to spare 256 KB, run on the caller's stack rather than refuse -- a small
     * program may still fit, and refusing helps nobody.
     */
    stack = AllocMem(AMIGA_ARGV_STACK, MEMF_ANY);
    if (stack != NULL)
    {
        argv_sss.stk_Lower   = stack;
        argv_sss.stk_Upper   = (ULONG)stack + AMIGA_ARGV_STACK;
        argv_sss.stk_Pointer = (APTR)((ULONG)stack + AMIGA_ARGV_STACK);

        argv_run_on_stack();

        FreeMem(stack, AMIGA_ARGV_STACK);
    }
    else
    {
        argv_result = __real_main(argv_argc, argv_vec);
    }

    return argv_result;
}
