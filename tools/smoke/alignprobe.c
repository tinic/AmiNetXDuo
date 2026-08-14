/*
 * alignprobe, what this machine's alignment rules actually are.
 *
 * Run it on an A600 or an A500 and every number below comes from a real 68000,
 * which is the only CPU in the family that faults rather than tolerates.  On a
 * 68020 it prints the same numbers and proves nothing, which is the point.
 *
 * It reports, and does not assume:
 *
 *   - the compiler's alignment for a long and for struct cmsghdr, which on
 *     m68k is 2 and not 4, and which is why CMSG_BUFFER() needs an attribute;
 *   - the alignment CMSG_BUFFER() delivers, on the stack and in static
 *     storage, which is the thing that was wrong;
 *   - what AllocVec() and AllocMem() return, which docs/ALIGNMENT.md and
 *     addralloc.c both rely on without ever having measured it;
 *   - that a longword load from an address 2 mod 4 completes, since half of
 *     every m68k object lands there and the library used to refuse those.
 *
 * It does NOT provoke an address error.  Nothing here reads a longword from an
 * odd address: that is a Guru, not a test result, and the hazard being real is
 * not in question.  What is in question is whether our own pointers can ever
 * be odd, and that is answered by the numbers.
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
#include <stddef.h>

#include "aminetxduo/cmsg.h"

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID a_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID) unused;
    if (c != '\0')
        RawPutChar(c);
}

static VOID a_log(const char *fmt, ...)
{
va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) a_put_char, NULL);
    va_end(args);
    RawPutChar('\n');
}

static LONG a_fails;

static VOID a_check(const char *what, BOOL ok, ULONG value)
{
    a_log("ALIGN %s %s (%lu)", (LONG) (ok ? "ok" : "FAIL"), (LONG) what, value);
    if (!ok)
        a_fails++;
}

/* Static storage gets the attribute too, and lands somewhere else entirely. */
CMSG_BUFFER(a_static_cbuf, CMSG_SPACE(16));

/*
 * The deterministic case, and the one that made this a bug rather than a coin
 * flip: a control buffer inside a struct, behind something two bytes wide.
 * Where the union's alignment is 2, which is all m68k gives it without the
 * attribute, the compiler puts the buffer at offset 2 and every instance of
 * this struct has it 2 mod 4.  Where it is 4, the compiler pads and the offset
 * is 4.  A stack local's address depends on the frame; a struct member's does
 * not, so this is the half of the probe that answers the same way every run.
 */
typedef struct
{
    UWORD  before;
    CMSG_BUFFER(cbuf, CMSG_SPACE(16));
} AlignHeldBuffer;

/*
 * A longword read from an address 2 mod 4.  Legal on every 68k, and the case
 * `& 3` used to refuse.  noinline and through a volatile pointer so nothing
 * folds it away.
 */
static __attribute__((noinline)) ULONG a_read_long(const volatile ULONG *p)
{
    return *p;
}

int main(int argc, char **argv)
{
CMSG_BUFFER(cbuf, CMSG_SPACE(16));
UBYTE   raw[16];
APTR    v1, v2, v3;
APTR    m1;
ULONG   got;
UWORD   i;

    (VOID) argc;
    (VOID) argv;

    a_log("ALIGN alignof long=%lu short=%lu ptr=%lu cmsghdr=%lu",
          (ULONG) __alignof__(long), (ULONG) __alignof__(short),
          (ULONG) __alignof__(void *), (ULONG) __alignof__(struct cmsghdr));

    /*
     * 2, on this toolchain.  Recorded rather than asserted: if it ever becomes
     * 4 the attribute in cmsg.h stops being load bearing, and this line is how
     * anyone would find that out.
     */
    a_log("ALIGN sizeof(struct{char;long;})=%lu",
          (ULONG) sizeof(struct { char c; long l; }));

    a_log("ALIGN cmsgbuf stack=%08lx static=%08lx",
          (ULONG) CMSG_BUFFER_PTR(cbuf), (ULONG) CMSG_BUFFER_PTR(a_static_cbuf));

    a_check("CMSG_BUFFER on the stack is longword aligned",
            (((ULONG) CMSG_BUFFER_PTR(cbuf)) & 3UL) == 0UL,
            ((ULONG) CMSG_BUFFER_PTR(cbuf)) & 3UL);
    a_check("CMSG_BUFFER in static storage is longword aligned",
            (((ULONG) CMSG_BUFFER_PTR(a_static_cbuf)) & 3UL) == 0UL,
            ((ULONG) CMSG_BUFFER_PTR(a_static_cbuf)) & 3UL);

    a_log("ALIGN cmsgbuf offset in a struct = %lu",
          (ULONG) offsetof(AlignHeldBuffer, cbuf));
    a_check("CMSG_BUFFER behind a UWORD is longword aligned",
            (offsetof(AlignHeldBuffer, cbuf) & 3UL) == 0UL,
            (ULONG) (offsetof(AlignHeldBuffer, cbuf) & 3UL));

    /*
     * What AllocVec() actually guarantees.  AllocMem() is documented as 8-byte
     * aligned; AllocVec() puts the size in the longword in front of what it
     * returns, so the answer is not the same one.  ami_alloc() is AllocVec().
     */
    v1 = AllocVec(1UL, MEMF_PUBLIC);
    v2 = AllocVec(37UL, MEMF_PUBLIC);
    v3 = AllocVec(1024UL, MEMF_PUBLIC);
    m1 = AllocMem(1UL, MEMF_PUBLIC);

    a_log("ALIGN allocvec %08lx %08lx %08lx allocmem %08lx",
          (ULONG) v1, (ULONG) v2, (ULONG) v3, (ULONG) m1);

    a_check("AllocVec is at least longword aligned",
            v1 != NULL && v2 != NULL && v3 != NULL &&
            ((((ULONG) v1) | ((ULONG) v2) | ((ULONG) v3)) & 3UL) == 0UL,
            ((((ULONG) v1) | ((ULONG) v2) | ((ULONG) v3)) & 3UL));
    a_check("AllocMem is at least longword aligned",
            m1 != NULL && (((ULONG) m1) & 3UL) == 0UL,
            ((ULONG) m1) & 3UL);

    if (v1 != NULL) FreeVec(v1);
    if (v2 != NULL) FreeVec(v2);
    if (v3 != NULL) FreeVec(v3);
    if (m1 != NULL) FreeMem(m1, 1UL);

    /* A longword out of an address 2 mod 4, on the machine that would fault on
       an odd one. */
    for (i = 0; i < 16; i++)
        raw[i] = (UBYTE) i;

    {
        ULONG aligned = ((ULONG) raw + 3UL) & ~3UL;

        got = a_read_long((const volatile ULONG *) (const VOID *) aligned);
        a_log("ALIGN long at 0 mod 4 = %08lx", got);

        got = a_read_long((const volatile ULONG *) (const VOID *) (aligned + 2UL));
        a_check("longword load from an address 2 mod 4 completes", TRUE, got);
    }

    if (a_fails == 0)
    {
        a_log("ALIGNPROBE PASS");
        return RETURN_OK;
    }

    a_log("ALIGNPROBE FAIL %ld", a_fails);
    return RETURN_FAIL;
}
