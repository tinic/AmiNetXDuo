/*
 * TU 1 of the reduced AmigaOS shared-library reproducer.
 *
 * Everything an AmigaOS 3.x .library needs and an ELF target has no analogue
 * for, and nothing else:
 *
 *   - a "moveq #-1 / rts" that must sit at offset 0 of the first code hunk
 *   - a `struct Resident` romtag, which NOTHING in the program references:
 *     exec finds it by scanning the loaded segment for RTC_MATCHWORD
 *   - an RTF_AUTOINIT init table, referenced only by the romtag
 *   - the LVO vector table, referenced only by the init table
 *   - a "$VER:" string that nothing reads
 *
 * Link order matters: this file first.
 */
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <exec/libraries.h>

#include "minlib.h"

/* Running the library file from a Shell must return -1, not execute a random
 * function.  This has to be at offset 0 of the first code hunk. */
asm("    .text                   \n"
    "    .globl _minlib_entry    \n"
    "_minlib_entry:              \n"
    "    moveq  #-1,%d0          \n"
    "    rts                     \n");

static char min_lib_name[] = "min.library";
static char min_lib_id[]   = "min.library 1.0 (21.8.2026)\r\n";

static const char min_lib_ver[] __attribute__((used)) =
    "$VER: min.library 1.0 (21.8.2026)";

static const APTR min_init_table[4] MINLIB_USED =
{
    (APTR)(LONG)sizeof(struct Library),
    (APTR)MinVectorTable,
    (APTR)NULL,
    (APTR)min_lib_init
};

const struct Resident min_romtag MINLIB_USED =
{
    RTC_MATCHWORD,
    (struct Resident *)&min_romtag,
    (APTR)(&min_romtag + 1),
    RTF_AUTOINIT,
    1,
    NT_LIBRARY,
    0,
    min_lib_name,
    min_lib_id,
    (APTR)min_init_table
};

struct Library *min_lib_init(register struct Library *base   __asm("d0"),
                             register APTR             seg    __asm("a0"),
                             register APTR             sysbase __asm("a6"))
{
    (void)seg;
    (void)sysbase;
    return base;
}
