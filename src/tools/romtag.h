/*
 * The name a device file gives itself: rt_Name of the first RomTag in an
 * AmigaOS load file, read without loading it.
 *
 * exec finds an open device by that name, compared with the FilePart of what
 * OpenDevice() was given; the file name plays no part once the file is loaded.
 * A copy saved under another name therefore loads and then never matches.
 *
 * Nothing here calls AmigaOS, so test/test_romtag.c runs the same code.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ROMTAG_H
#define AMINETXDUO_ROMTAG_H

#include <exec/types.h>

/* Largest load file read; SANA-II drivers are a few tens of KB. */
#define ROMTAG_FILE_MAX     (256UL * 1024UL)

/*
 * rt_Name of the first RomTag in `file` (a whole load file, `len` bytes) into
 * `name`, NUL-terminated and cut at `namelen` - 1. TRUE when found; FALSE for
 * anything that is not a load file, has no RomTag, or whose name is not a
 * string inside the file.
 */
BOOL romtag_name(const UBYTE *file, ULONG len, char *name, ULONG namelen);

#endif
