/*
 * TU 2: the LVO vector table.  This array IS the ABI: exec copies each entry
 * into a JMP instruction at -6*(i+1) from the library base.  Nothing in the
 * program calls any of them, and nothing but the romtag's init table refers to
 * the array.
 */
#include "minlib.h"

const APTR MinVectorTable[] MINLIB_USED =
{
    (APTR)min_open,             /* -6  */
    (APTR)min_close,            /* -12 */
    (APTR)min_expunge,          /* -18 */
    (APTR)min_reserved,         /* -24 */

    (APTR)min_add,              /* -30 */
    (APTR)min_sub,              /* -36 */
    (APTR)min_ptr,              /* -42 */
    (APTR)min_mix,              /* -48 */
    (APTR)min_enosys,           /* -54, a reserved slot: same fn twice */
    (APTR)min_enosys,           /* -60 */

    (APTR)-1
};
