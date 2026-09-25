/*
 * Whether bsdsocket.library could fit, decided from what was free before the
 * open and from the load file's own hunk table.
 *
 * A failed OpenLibrary() says nothing about why: ramlib does the LoadSeg, so
 * the caller's IoErr() stays 0, and a LoadSeg that fails takes nothing, so
 * free memory afterwards reads as it did before. Measured on a 512 KB A2000,
 * 2.04 (#55): 344,000 bytes free, a 344,260-byte library, LoadSeg IoErr 103.
 *
 * Nothing here calls AmigaOS, so test/test_libfit.c runs the same code.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_LIBFIT_H
#define AMINETXDUO_LIBFIT_H

#include <exec/types.h>

/* HUNK_HEADER with its size table; 16 hunks, each with an attribute long. */
#define LIBFIT_HEAD_MAX     (20UL + 16UL * 8UL)

typedef struct LibFitNeed
{
    ULONG total;                        /* every hunk, with LoadSeg's 8 bytes */
    ULONG largest;                      /* the largest single allocation      */
} LibFitNeed;

/*
 * What LoadSeg allocates for the load file whose first `len` bytes are `head`.
 * FALSE, and a zero need, for anything that is not a load file.
 */
BOOL libfit_need(const UBYTE *head, ULONG len, LibFitNeed *need);

/*
 * TRUE when memory, and not the device, is why the library did not open:
 * the load file does not fit in what was free before the open, or leaves
 * less than `headroom` for the stack. `need` is zero when nothing had to be
 * loaded (already resident) or the file could not be read.
 */
BOOL libfit_short(ULONG free_before, ULONG largest_before,
                  const LibFitNeed *need, ULONG headroom);

#endif
