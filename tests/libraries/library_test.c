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
 * against our code.
 *
 * Stage the libraries into LIBS: first -- see tests/libraries/run-fsuae.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
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
 * The library bases are declared here rather than pulled from <proto/...>:
 * the inline headers expect the base to be a global with a specific name,
 * which is the ABI detail under test.
 */
struct Library *SocketBase;

/* bsdsocket.library LVOs, from the NDK's bsdsocket_lib.fd and
   <inline/bsdsocket.h>. */
#define LVO_socket          (-30)
#define LVO_CloseSocket     (-120)
#define LVO_Inet_NtoA       (-174)
#define LVO_inet_addr       (-180)
#define LVO_inet_network    (-204)
#define LVO_gethostbyname   (-210)
#define LVO_gethostname     (-282)
#define LVO_inet_pton       (-606)

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

static ULONG bsd_inet_addr(struct Library *base, const char *cp)
{

register struct Library *a6 __asm("a6") = base;
register const char     *a0 __asm("a0") = cp;
register ULONG           res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-180:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}

static ULONG bsd_inet_network(struct Library *base, const char *cp)
{

register struct Library *a6 __asm("a6") = base;
register const char     *a0 __asm("a0") = cp;
register ULONG           res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-204:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_inet_pton(struct Library *base, LONG af, const char *src,
                          APTR dst)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = af;
register const char     *a0 __asm("a0") = src;
register APTR            a1 __asm("a1") = dst;
register LONG            res __asm("d0");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");


    __asm __volatile ("jsr a6@(-606:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
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


/* --------------------------------------------------- the bsdsocket phase -- */

/*
 * Run in a Process of its own so main() can put a clock on it -- specifically
 * on the CloseLibrary() at the end, which drops the last netstack reference and
 * tears the stack down.  That close hung for ever: netstack_shutdown() stopped
 * the ARexx host by waiting on a MsgPort created during bring-up, on a Task
 * that no longer existed, so the signal went nowhere (netstack_rexx.c).  It is
 * invisible in normal use because AddNetInterface keeps a reference; any
 * program that opens the library, uses it and exits hits it.
 *
 * A wedged close cannot be unwedged, so a failing run leaves this Process
 * parked in Exec while main() reports and exits.  It is parked in Wait(), not
 * in this hunk, and the run is over either way.
 */

#define T_BRINGUP_LIMIT     900UL       /* 200 ms units: 180 s to come up   */
#define T_CLOSE_LIMIT       300UL       /* 200 ms units: 60 s to close      */
#define T_POLL_TICKS        10UL        /* Delay() units: 200 ms            */
#define T_CHILD_STACK       65536UL

static volatile ULONG   t_closing;
static volatile ULONG   t_child_done;

static VOID t_bsd_main(VOID)
{

struct Library  *sbase;
char             hostname[64];
LONG             sock;


    /*
     * This is the whole stack: OpenLibrary() runs the romtag init, clones a
     * per-opener base and calls netstack_startup(), which opens the SANA-II
     * device, starts ThreadX and blocks for a DHCP lease.
     */
    t_log("opening bsdsocket.library (this brings the stack up)");

    sbase =  OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4UL);
    if (!t_check((BOOL) (sbase != NULL), "OpenLibrary(bsdsocket.library)", 0UL))
    {
        t_child_done =  1UL;
        return;
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

    /*
     * The address conversions, against the NDK 3.2 autodoc's own rules: the
     * short dotted forms, INADDR_NONE for anything malformed, and
     * inet_network()'s byte pack -- "192.168.1" is the network number
     * 0x00C0A801, not an address with a zero host part.
     */
    {
        ULONG net;

        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "192.168.1.1")
                               == 0xC0A80101UL), "inet_addr(dotted quad)", 0UL);
        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "10.1") == 0x0A000001UL),
                       "inet_addr(two-part form)", 0UL);
        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "") == 0xFFFFFFFFUL),
                       "inet_addr(\"\") is INADDR_NONE", 0UL);
        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "1.2.3.4x")
                               == 0xFFFFFFFFUL),
                       "inet_addr(trailing garbage) is INADDR_NONE", 0UL);

        net =  bsd_inet_network(sbase, "192.168.1");
        (VOID) t_check((BOOL) (net == 0x00C0A801UL),
                       "inet_network(\"192.168.1\")", net);
        (VOID) t_check((BOOL) (bsd_inet_network(sbase, "127") == 127UL),
                       "inet_network(\"127\")", 0UL);
    }

    {
        ULONG addr =  0UL;

        (VOID) t_check((BOOL) (bsd_inet_pton(sbase, 2L, "1.2.3.4", &addr) == 1L
                               && addr == 0x01020304UL),
                       "inet_pton(AF_INET, dotted quad)", addr);
        /* Only the full a.b.c.d form; a short form is 0, not 1. */
        (VOID) t_check((BOOL) (bsd_inet_pton(sbase, 2L, "1.2.3", &addr) == 0L),
                       "inet_pton(AF_INET, short form) is 0", 0UL);
        /* An unknown family is -1, which is not the same answer as invalid. */
        (VOID) t_check((BOOL) (bsd_inet_pton(sbase, 99L, "1.2.3.4", &addr)
                               == -1L),
                       "inet_pton(bad family) is -1", 0UL);
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
    t_log("closing bsdsocket.library (tears the stack down)");

    SocketBase =  NULL;
    t_closing  =  1UL;

    CloseLibrary(sbase);

    t_log("  closed bsdsocket.library");
    t_child_done =  1UL;
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

struct Library  *ugbase;
struct Process  *child;
struct TagItem   tags[6];
ULONG            waited;
ULONG            closed_after;


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

    /* ---- bsdsocket.library, on a watched Process ------------------------- */

    tags[0].ti_Tag  =  NP_Entry;      tags[0].ti_Data =  (ULONG) t_bsd_main;
    tags[1].ti_Tag  =  NP_Name;       tags[1].ti_Data =  (ULONG) "library_test bsd";
    tags[2].ti_Tag  =  NP_StackSize;  tags[2].ti_Data =  T_CHILD_STACK;
    tags[3].ti_Tag  =  NP_Cli;        tags[3].ti_Data =  (ULONG) FALSE;
    tags[4].ti_Tag  =  TAG_DONE;      tags[4].ti_Data =  0;

    child =  CreateNewProc(tags);
    if (!t_check((BOOL) (child != NULL), "CreateNewProc(bsdsocket phase)", 0UL))
    {
        t_log("");
        t_log("%ld checks, %ld failures -- FAIL", t_checks, t_failures);
        return(20);
    }

    closed_after =  0UL;
    for (waited = 0UL; t_child_done == 0UL; waited++)
    {
        if (t_closing != 0UL)
        {
            closed_after++;
            if (closed_after > T_CLOSE_LIMIT)
            {
                break;
            }
        }
        else if (waited > T_BRINGUP_LIMIT)
        {
            break;
        }

        Delay(T_POLL_TICKS);
    }

    t_log("");
    if (t_closing == 0UL)
    {
        (VOID) t_check(FALSE, "bsdsocket phase reached the close", waited / 5UL);
    }
    else
    {
        (VOID) t_check((BOOL) (t_child_done != 0UL),
                       "CloseLibrary(bsdsocket.library) returned",
                       closed_after / 5UL);
        if (t_child_done == 0UL)
        {
            t_log("  the last close did not return in %ld s -- the teardown is "
                  "stuck", T_CLOSE_LIMIT / 5UL);
        }
    }

    t_log("");
    t_log("%ld checks, %ld failures -- %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");

    return((t_failures == 0UL) ? 0 : 20);
}
