#ifndef MINLIB_H
#define MINLIB_H

#include <exec/types.h>
#include <exec/libraries.h>

/* -DMINLIB_KEEP marks the three objects exec finds by SCANNING the segment
   rather than by any reference from the program.  Without it LTO's
   whole-program view sees them as unreferenced and removes them. */
#ifdef MINLIB_KEEP
#define MINLIB_USED __attribute__((used))
#else
#define MINLIB_USED
#endif

extern const APTR MinVectorTable[] MINLIB_USED;

struct Library *min_lib_init(register struct Library *base   __asm("d0"),
                             register APTR             seg    __asm("a0"),
                             register APTR             sysbase __asm("a6"));

/* The four exec standard vectors. */
struct Library *min_open (register struct Library *base __asm("a6"));
LONG            min_close(register struct Library *base __asm("a6"));
LONG            min_expunge(register struct Library *base __asm("a6"));
LONG            min_reserved(register struct Library *base __asm("a6"));

/* Ordinary vectors, each with a different register-argument shape.  These are
 * reached ONLY through MinVectorTable, exactly as a real library's are. */
LONG min_add(register LONG a __asm("d0"),
             register LONG b __asm("d1"),
             register struct Library *base __asm("a6"));

LONG min_sub(register LONG a __asm("d1"),
             register LONG b __asm("d0"),
             register struct Library *base __asm("a6"));

LONG min_ptr(register const char *p __asm("a0"),
             register LONG n __asm("d0"),
             register struct Library *base __asm("a6"));

LONG min_mix(register const char *p __asm("a0"),
             register const char *q __asm("a1"),
             register LONG n __asm("d0"),
             register LONG m __asm("d1"),
             register struct Library *base __asm("a6"));

LONG min_enosys(register struct Library *base __asm("a6"));

/* Hand-written 68k assembly, src/net68k / src/crypto68k in miniature. */
LONG min_asm_sum(register const UBYTE *p __asm("a0"),
                 register LONG n __asm("d0"));

#endif
