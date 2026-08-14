/*
 * memprobe, what opening the stack costs the machine.
 *
 * AvailMem() at four points in one run: before OpenLibrary(), with the stack
 * up, after the last close (which stops the stack but leaves the segment
 * loaded), and after the segment has been expunged.  Nothing else measures the
 * library code itself, netstat -h reads the same AvailMem, but it needs a
 * running stack and so has no baseline to compare against.
 *
 * The last two never print today: the final CloseLibrary() does not return
 * (docs/RESEARCH.md 81.5).  The reports either side of it are what localised
 * that, so they stay.
 *
 * Output goes to the serial port, which is what the emulator harnesses
 * capture.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID m_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID) unused;
    if (c != '\0')
        RawPutChar(c);
}

static VOID m_log(const char *fmt, ...)
{
va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) m_put_char, NULL);
    va_end(args);
    RawPutChar('\n');
}

static VOID m_report(const char *when)
{
    m_log("MEM %s public=%lu largest=%lu chip=%lu fast=%lu",
          (LONG) when,
          AvailMem(MEMF_PUBLIC),
          AvailMem(MEMF_PUBLIC | MEMF_LARGEST),
          AvailMem(MEMF_CHIP),
          AvailMem(MEMF_FAST));
}

/* Exec expunges libraries when an allocation cannot be met, so ask for one
   that never can and let the low-memory handler run. */
static VOID m_force_expunge(VOID)
{
    APTR huge = AllocMem(0x7F000000UL, MEMF_PUBLIC);

    if (huge != NULL)
        FreeMem(huge, 0x7F000000UL);
}

int main(int argc, char **argv)
{
struct Library *sock;
struct Library *ug;
LONG            hold = 0;

    (VOID) argv;
    if (argc > 1)
        hold = 1;

    m_report("baseline");

    sock = OpenLibrary((STRPTR) "bsdsocket.library", 0UL);
    if (sock == NULL)
    {
        m_log("MEM openfailed bsdsocket.library");
        m_report("afterfail");
        m_force_expunge();
        m_report("afterfail-expunged");
        m_log("MEMPROBE FAIL");
        return RETURN_FAIL;
    }

    m_log("MEM opened bsdsocket.library version %lu.%lu",
          (ULONG) sock->lib_Version, (ULONG) sock->lib_Revision);
    m_report("stackup");

    ug = OpenLibrary((STRPTR) "usergroup.library", 0UL);
    if (ug != NULL)
    {
        m_log("MEM opened usergroup.library version %lu.%lu",
              (ULONG) ug->lib_Version, (ULONG) ug->lib_Revision);
        m_report("usergroup");
    }
    else
    {
        m_log("MEM openfailed usergroup.library");
    }

    if (hold != 0)
    {
        m_log("MEM holding");
        Delay(250);
        m_report("held");
    }

    if (ug != NULL)
    {
        m_log("MEM closing usergroup.library");
        CloseLibrary(ug);
        m_report("ugclosed");
    }

    m_log("MEM closing bsdsocket.library");
    CloseLibrary(sock);
    m_report("closed");

    m_log("MEM expunging");
    m_force_expunge();
    m_report("expunged");

    m_log("MEMPROBE PASS");
    return RETURN_OK;
}
