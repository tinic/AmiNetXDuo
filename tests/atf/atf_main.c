/*
 * The runner behind tests/atf/atf-c.h.
 *
 * One process, cases in registration order, a jmp_buf per case for the fatal
 * assertions.  Output is the shape tests/sockopt/sockopt_test.c uses -- one
 * line per assertion, a "N checks, M failures -- PASS/FAIL" trailer -- because
 * that is what the emulator run scripts read.
 *
 * The interface comes from tests/tcpdrill/tapdev.c, a SANA-II device made at
 * run time, so an adopted test needs no a2065.device and runs wherever tier 2
 * runs.  It is installed before bsdsocket.library is opened, for the same
 * reason sockopt_test.c does it: the device lives in the installer's address
 * space.
 *
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <stdint.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <inline/macros.h>

#include <stdarg.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <errno.h>

#define ATF_NO_SOCKET_ALIASES   /* the runner calls the library by its own names */
#include "atf-c.h"

#include "tapdev.h"

struct Library *SocketBase;             /* the NDK inlines dereference this */

/*
 * SetErrnoPtr by hand, the way tests/sockopt/sockopt_test.c calls the whole
 * library.  The NDK's inline for it does not compile under GCC 15: it binds
 * errno_ptr to a0 and size to d0 as input register variables and then names
 * d0 and a0 in the clobber list of the same asm, which is
 *
 *   error: 'asm' specifier for variable '__SetErrnoPtr_errno_ptr' conflicts
 *          with 'asm' clobber list
 *
 * Every void-returning bsdsocket inline is written that way, so this is the
 * form to reach for if another one is needed.  Vector -168 is
 * src/bsdsocket/bsdsocket_vectors.c slot 27.
 */
static VOID atfc_set_errno_ptr(APTR ptr, LONG size)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register APTR            a0 __asm("a0") = ptr;
    register LONG            d0 __asm("d0") = size;
    register LONG            _d1 __asm("d1");
    register LONG            _a1 __asm("a1");

    __asm __volatile ("jsr a6@(-168:W)"
                      : "=r" (_d1), "=r" (_a1)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "cc", "memory");
}

/* ---------------------------------------------------------------- state --- */

#define ATFC_MAX_CASES  64

jmp_buf     atfc_case_jmp;

static atf_tc_t     atfc_cases[ATFC_MAX_CASES];
static atf_tp_t     atfc_tp = { atfc_cases, 0, ATFC_MAX_CASES };
static atf_tc_t    *atfc_current;
static unsigned long atfc_checks;
static unsigned long atfc_failures;
static int           atfc_in_case;

/* ---------------------------------------------------------------- output --- */

/*
 * Output() rather than stdout: a guest test's stdout is what the run script
 * captures, and Write() to it needs no flush at a point the test may not reach.
 *
 * AND THE SERIAL PORT, which is the half that was missing.  Output() is a file
 * on the emulated drive: a run that never returns leaves it whatever length
 * the handler last committed, which for this binary is zero, and a harness
 * cannot tell that from a program that never started.  RawPutChar goes out a
 * byte at a time as it happens and survives a hang, a Guru and a reset, which
 * is what every other harness here reads.  It is also what showed that this
 * binary crashes before main() rather than hanging: see tests/HARNESSES.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static void atfc_serial(const char *s)
{
    while (*s != '\0')
    {
        RawPutChar((UBYTE)*s++);
    }
}

/*
 * Before main(), to tell a crash in the C startup from a crash in a case.
 * AtfTcpSocket prints neither this nor the first line of atfc_run(), which is
 * what puts its Address Error ahead of every line of code in this file.
 */
static void atfc_ctor(void) __attribute__((constructor));
static void atfc_ctor(void)
{
    atfc_serial("atf: ctor\n");
}

static void atfc_emit(const char *s)
{
    BPTR out;

    atfc_serial(s);

    out = Output();

    if (out != (BPTR)0)
        (void)Write(out, (APTR)s, (LONG)strlen(s));
}

static void atfc_line(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if ((size_t)n > sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n]     = '\n';
    buf[n + 1] = '\0';

    atfc_emit(buf);
}

void atfc_note(const char *fmt, ...)
{
    char    buf[384];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    atfc_line("  note %s", buf);
}

/* --------------------------------------------------------------- checks --- */

int atfc_record(int ok, const char *what)
{
    atfc_checks++;
    if (ok)
    {
        atfc_line("  ok   %s", what);
    }
    else
    {
        atfc_failures++;
        atfc_line("  FAIL %s (errno %d)", what, errno);
    }

    return ok;
}

void atfc_recordf(int ok, const char *what, const char *fmt, ...)
{
    char    buf[384];
    va_list ap;

    atfc_checks++;
    if (ok)
    {
        atfc_line("  ok   %s", what);
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    atfc_failures++;
    atfc_line("  FAIL %s -- %s", what, buf);
}

/*
 * ATF ends a case by ending the process it runs in.  There is one process here,
 * so it ends by longjmp back into atfc_run().  Outside a case -- which is where
 * a head calling atf_tc_skip() would be -- there is nothing to jump to, so the
 * outcome is recorded and the caller returns normally.
 */
void atfc_end(int how)
{
    if (!atfc_in_case)
        return;

    longjmp(atfc_case_jmp, how + 1);
}

void atfc_endf(int how, const char *fmt, ...)
{
    char    buf[384];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (how == 1)
    {
        atfc_checks++;
        atfc_failures++;
        atfc_line("  FAIL %s", buf);
    }
    else
    {
        atfc_line("  skip %s", buf);
    }

    atfc_end(how);
}

/* -------------------------------------------------------------- metadata --- */

/*
 * Only two variables can be answered here, and both answer "skip": there are no
 * user IDs to require and no kyua to supply a config variable.  The rest are
 * named in the log rather than dropped, so a case that turns out to depend on
 * one is visible in the transcript.
 */
void atf_tc_set_md_var(atf_tc_t *tc, const char *name, const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (tc == NULL)
        return;

    if (strcmp(name, "require.user") == 0)
    {
        tc->tc_skip        = 1;
        tc->tc_skip_reason = "require.user: AmigaOS has no user IDs";
    }
    else if (strcmp(name, "require.config") == 0)
    {
        tc->tc_skip        = 1;
        tc->tc_skip_reason = "require.config: no kyua configuration here";
    }
    else if (strcmp(name, "descr") == 0)
    {
        atfc_line("  descr %s", buf);
    }
    else
    {
        atfc_line("  md    %s=%s (ignored)", name, buf);
    }
}

int atf_tc_has_config_var(const atf_tc_t *tc, const char *name)
{
    (void)tc; (void)name;
    return 0;
}

const char *atf_tc_get_config_var(const atf_tc_t *tc, const char *name)
{
    (void)tc; (void)name;
    return NULL;
}

const char *atf_tc_get_config_var_wd(const atf_tc_t *tc, const char *name,
                                     const char *def)
{
    (void)tc; (void)name;
    return def;
}

/* ---------------------------------------------------------- registration --- */

int atfc_register(atf_tp_t *tp, const char *name,
                  void (*head)(atf_tc_t *),
                  void (*body)(const atf_tc_t *),
                  void (*cleanup)(const atf_tc_t *))
{
    atf_tc_t *tc;

    if (tp == NULL || tp->tp_count >= tp->tp_max)
        return -1;

    tc = &tp->tp_cases[tp->tp_count++];
    tc->tc_name        = name;
    tc->tc_head        = head;
    tc->tc_body        = body;
    tc->tc_cleanup     = cleanup;
    tc->tc_skip        = 0;
    tc->tc_skip_reason = NULL;

    return 0;
}

/* --------------------------------------------------------------- the run --- */

static int atfc_run_case(atf_tc_t *tc)
{
    int reason;

    atfc_line("");
    atfc_line("%s", tc->tc_name);

    /* The head runs outside the case so a skip it sets is a decision, not an
       exit: atfc_end() has nowhere to jump until atfc_in_case is set. */
    if (tc->tc_head != NULL)
        tc->tc_head(tc);

    if (tc->tc_skip)
    {
        atfc_line("  skip %s", tc->tc_skip_reason);
        return 2;
    }

    atfc_current = tc;
    atfc_in_case = 1;

    reason = setjmp(atfc_case_jmp);
    if (reason == 0)
    {
        tc->tc_body(tc);
        reason = 1;                     /* fell off the end == passed */
    }

    atfc_in_case = 0;
    atfc_current = NULL;

    /*
     * ATF runs a cleanup in a process of its own, after the body's has gone.
     * Inline is the closest this platform gets; a cleanup that undoes global
     * state still works, one that assumes a fresh process does not.
     */
    if (tc->tc_cleanup != NULL)
        tc->tc_cleanup(tc);

    return reason - 1;
}

int atfc_run(int (*add_tcs)(atf_tp_t *), const char *progname)
{
    static const UBYTE tap_mac[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x09 };
    int i;

    atfc_serial("atf: entered\n");

    atfc_line("AmiNetXDuo -- %s, adopted from FreeBSD tests/sys/netinet",
              progname);

    atfc_serial("atf: tap_install\n");

    if (tap_install(tap_mac) != 0)
    {
        atfc_line("cannot install the test interface");
        return 20;
    }

    atfc_serial("atf: OpenLibrary\n");

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        atfc_line("bsdsocket.library not available");
        tap_remove();
        return 20;
    }

    /*
     * What makes an unmodified `errno == EADDRINUSE` work: the library writes
     * every failure through this pointer, so the test reads the real newlib
     * errno and strerror() reads the same one.
     */
    atfc_set_errno_ptr((APTR)&errno, (LONG)sizeof(errno));

    if (add_tcs(&atfc_tp) != 0)
    {
        atfc_line("test case table did not build");
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        tap_remove();
        return 20;
    }

    atfc_serial("atf: cases\n");

    for (i = 0; i < atfc_tp.tp_count; i++)
        (void)atfc_run_case(&atfc_tp.tp_cases[i]);

    CloseLibrary(SocketBase);
    SocketBase = NULL;
    tap_remove();

    atfc_line("");
    atfc_line("%lu checks, %lu failures -- %s", atfc_checks, atfc_failures,
              (atfc_failures == 0UL) ? "PASS" : "FAIL");

    return (atfc_failures == 0UL) ? 0 : 20;
}
