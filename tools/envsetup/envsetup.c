/*
 * envsetup, prepare the AmigaDOS environment for a harness test run.
 *
 * The harness boots from a bare directory hard drive with a minimal
 * Startup-Sequence, so none of the assigns a normal Workbench boot makes exist.
 * Anything calling GetVar()/SetVar() (ENV:), or writing scratch files (T:),
 * fails without them.
 *
 * Doing this in C rather than with C:Assign and C:MakeDir keeps the harness
 * self-contained, no Workbench binaries have to be extracted or staged.
 *
 * ENV: and T: are backed by directories on the *host*, not RAM:, so env vars a
 * test sets are visible from the host after the run, and can be pre-seeded by
 * staging an env/ directory.
 *
 * IT ALSO SIGNS THE TRANSCRIPT.
 *
 *   envsetup <token>
 *
 * prints `ANXD-RUN <token>` out the serial port before anything under test
 * runs.  The harness reads the guest's serial line over a TCP socket, and the
 * bytes that arrive carry nothing that says which emulator sent them: when two
 * runs collided on one port on 2026-08-25, one arm read the other's guest and
 * reported findings about a machine it had never booted.  A token the harness
 * generated and the guest echoed closes that: a transcript either opens with
 * this run's token or it is not this run's transcript, and that stays true a
 * week later when the only thing left is the log file.
 *
 * The token goes out with RawPutChar (exec LVO -516) rather than Printf,
 * because Printf writes to the CLI's console and the serial line is the only
 * sink the harness reads.  RawIOInit first, or the serial hardware is never
 * set up and most of what follows is dropped -- an earlier probe elsewhere in
 * this tree printed "start." for "start.in " before it learned that.  The spin
 * between characters is for the same reason: the emulated line drops anything
 * written faster than it clocks out.
 *
 * SPDX-License-Identifier: MIT
 */

#include <proto/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif
#ifndef RawIOInit
#  define RawIOInit() \
      LP0NR(0x1F8, RawIOInit, , EXEC_BASE_NAME)
#endif

static void serial_putc(UBYTE c)
{
    volatile ULONG spin;

    RawPutChar(c);
    for (spin = 0; spin < 3000UL; spin++)
        ;
}

static void serial_puts(const char *s)
{
    while (*s != '\0')
        serial_putc((UBYTE)*s++);
}

/* `ANXD-RUN <token>` and a newline.  Called before any assign is made, so a
   run that dies in bring-up still has a signed transcript. */
static void announce(const char *token)
{
    RawIOInit();
    serial_puts("ANXD-RUN ");
    serial_puts(token);
    serial_putc((UBYTE)'\n');
}

struct Assignment
{
    const char *name;       /* assign name, without the colon */
    const char *path;       /* directory to create and assign to */
};

static const struct Assignment assignments[] =
{
    { "ENV",    "DH0:env"       },
    { "ENVARC", "DH0:envarc"    },
    { "T",      "DH0:t"         },
    { "CLIPS",  "DH0:clips"     },
    /* LIBS: and DEVS: so a staged bsdsocket.library or SANA-II driver can be
       found.  A command that links the stack does not need them; one that
       opens the library, nc, ping, bsdsocktest, finds nothing without
       them, and fails in a way that looks like the stack rather than the
       harness.  CreateDir() below makes them when nothing was staged, so an
       empty assign is harmless. */
    { "LIBS",   "DH0:libs"      },
    { "DEVS",   "DH0:devs"      },
    { NULL,     NULL            }
};

int main(int argc, char **argv)
{
    const struct Assignment *a;
    LONG failures = 0;

    /* The token is optional: a caller that does not pass one gets exactly the
       envsetup this always was.  argv[1] carries it, and the AmigaDOS shell
       has already stripped the quoting. */
    if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
        announce(argv[1]);

    for (a = assignments; a->name != NULL; a++)
    {
        BPTR lock;

        /* Create it if it isn't staged already; an existing directory is fine. */
        lock = CreateDir((STRPTR)a->path);
        if (lock != 0)
            UnLock(lock);

        lock = Lock((STRPTR)a->path, SHARED_LOCK);
        if (lock == 0)
        {
            Printf("envsetup: cannot lock %s\n", (LONG)a->path);
            failures++;
            continue;
        }

        /* AssignLock() takes ownership of the lock, do not UnLock it. */
        if (!AssignLock((STRPTR)a->name, lock))
        {
            Printf("envsetup: cannot assign %s: %s\n", (LONG)a->name, (LONG)a->path);
            UnLock(lock);
            failures++;
        }
    }

    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}
