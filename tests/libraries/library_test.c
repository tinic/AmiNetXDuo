/*
 * AmiNetXDuo -- shared-library load test.
 *
 * Proves that the two images we ship are real AmigaOS shared libraries: that
 * Exec finds the romtag in the loaded segment, runs the RTF_AUTOINIT init
 * vector, and hands back a working base -- and, for bsdsocket.library, that
 * OpenLibrary() brings the whole netstack up behind it and the LVO jump table
 * dispatches.
 *
 * It is a separate executable from tests/netstack because it exercises the
 * libraries through their ABI, not through the C API: nothing here is linked
 * against our code at all.
 *
 * Stage the libraries into LIBS: first -- see tests/libraries/run-fsuae.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        RawPutChar(c);
    }
}

static VOID t_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    RawPutChar('\n');
}

static ULONG    t_checks;
static ULONG    t_failures;

static BOOL t_check(BOOL ok, const char *what, ULONG detail)
{

    t_checks++;
    if (!ok)
    {
        t_failures++;
        t_log("  FAIL %s (0x%lx)", what, detail);
    }
    else
    {
        t_log("  ok   %s", what);
    }

    return(ok);
}


/* -------------------------------------------------------------- vectors -- */

/*
 * The library bases are declared here rather than pulled from <proto/...>
 * because the inline headers expect the base to be a global with a specific
 * name, and that is exactly the ABI detail worth testing.
 */
struct Library *SocketBase;

/* bsdsocket.library LVOs, straight out of the NDK's bsdsocket_lib.fd. */
/* Offsets taken from the NDK's <inline/bsdsocket.h>, not guessed. */
#define LVO_socket          (-30)
#define LVO_CloseSocket     (-120)
#define LVO_Inet_NtoA       (-174)
#define LVO_gethostbyname   (-210)
#define LVO_gethostname     (-282)

static LONG bsd_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = domain;
register LONG            d1 __asm("d1") = type;
register LONG            d2 __asm("d2") = proto;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");


    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_close_socket(struct Library *base, LONG sock)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = sock;
register LONG            res __asm("d0");


    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_gethostname(struct Library *base, char *name, ULONG len)
{

register struct Library *a6 __asm("a6") = base;
register char           *a0 __asm("a0") = name;
register ULONG           d0 __asm("d0") = len;
register LONG            res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-282:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}

static char *bsd_inet_ntoa(struct Library *base, ULONG addr)
{

register struct Library *a6 __asm("a6") = base;
register ULONG           d0 __asm("d0") = addr;
register char           *res __asm("d0");


    __asm __volatile ("jsr a6@(-174:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return(res);
}

static APTR bsd_gethostbyname(struct Library *base, const char *name)
{

register struct Library *a6 __asm("a6") = base;
register const char     *a0 __asm("a0") = name;
register APTR            res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

struct Library  *ugbase;
struct Library  *sbase;
char             hostname[64];
LONG             sock;


    t_log("AmiNetXDuo -- shared library load test");

    /* ---- usergroup.library ---------------------------------------------- */

    ugbase =  OpenLibrary((CONST_STRPTR) "usergroup.library", 0UL);
    if (t_check((BOOL) (ugbase != NULL), "OpenLibrary(usergroup.library)", 0UL))
    {

        t_log("  usergroup.library %ld.%ld: %s",
              (ULONG) ugbase -> lib_Version, (ULONG) ugbase -> lib_Revision,
              (ugbase -> lib_IdString != NULL) ? ugbase -> lib_IdString
                                               : (STRPTR) "(no id)");

        (VOID) t_check((BOOL) (ugbase -> lib_Version >= 4),
                       "usergroup.library version >= 4",
                       (ULONG) ugbase -> lib_Version);

        CloseLibrary(ugbase);
    }

    /* ---- bsdsocket.library ---------------------------------------------- */

    /*
     * This is the whole stack: OpenLibrary() runs the romtag init, clones a
     * per-opener base and calls netstack_startup(), which opens the SANA-II
     * device, starts ThreadX and blocks for a DHCP lease.
     */
    t_log("opening bsdsocket.library (this brings the stack up)");

    sbase =  OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4UL);
    if (!t_check((BOOL) (sbase != NULL), "OpenLibrary(bsdsocket.library)", 0UL))
    {
        t_log("");
        t_log("%ld checks, %ld failures -- FAIL", t_checks, t_failures);
        return(20);
    }

    SocketBase =  sbase;

    t_log("  bsdsocket.library %ld.%ld: %s",
          (ULONG) sbase -> lib_Version, (ULONG) sbase -> lib_Revision,
          (sbase -> lib_IdString != NULL) ? sbase -> lib_IdString
                                          : (STRPTR) "(no id)");

    hostname[0] =  '\0';
    (VOID) t_check((BOOL) (bsd_gethostname(sbase, hostname,
                                           (ULONG) sizeof(hostname)) == 0),
                   "gethostname()", 0UL);
    t_log("  hostname '%s'", hostname);

    {
        char *dotted =  bsd_inet_ntoa(sbase, 0x0A00020FUL);   /* 10.0.2.15 */

        t_log("  Inet_NtoA(10.0.2.15) = '%s'",
              (dotted != NULL) ? dotted : "(null)");
        (VOID) t_check((BOOL) (dotted != NULL), "Inet_NtoA()", 0UL);
    }

    sock =  bsd_socket(sbase, 2L /* AF_INET */, 1L /* SOCK_STREAM */, 0L);
    if (t_check((BOOL) (sock >= 0), "socket(AF_INET, SOCK_STREAM)", (ULONG) sock))
    {
        (VOID) t_check((BOOL) (bsd_close_socket(sbase, sock) == 0),
                       "CloseSocket()", 0UL);
    }

    {
        APTR he =  bsd_gethostbyname(sbase, "localhost");

        (VOID) t_check((BOOL) (he != NULL), "gethostbyname(\"localhost\")", 0UL);
    }

    t_log("");
    t_log("%ld checks, %ld failures -- %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");

    /*
     * Reported last, and separately, because CloseLibrary() drops the final
     * netstack reference and can hang in teardown: on Commodore's
     * a2065.device 2.16 an AbortIO() on a pending SANA-II CMD_READ is never
     * honoured, so the reader thread's WaitIO() never returns.
     */
    t_log("");
    t_log("closing bsdsocket.library (tears the stack down)");

    SocketBase =  NULL;
    CloseLibrary(sbase);
    t_log("  closed bsdsocket.library");

    return((t_failures == 0UL) ? 0 : 20);
}
