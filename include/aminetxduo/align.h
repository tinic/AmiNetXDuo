/*
 * Longword alignment pins for structure members on the hot paths.
 *
 * m68k GCC aligns a LONG or a pointer to 2 bytes, so a UWORD or BOOL added in
 * front of one moves every longword after it to 2 mod 4, and a 68020 or later
 * then takes two bus cycles for each access.  #424 did that to the per-call
 * fields of struct AmiSocketBase and cost 1-3% of A1200 upload.  -malign-int
 * is not the fix: it changes the layout of every OS and NDK structure.
 *
 * AMI_ALIGN_LONG(type, member) fails the build when the member's offset is
 * not a multiple of 4.  It costs no code and no data.  The tables sit after
 * each structure, with its #if guards; a member that fires takes
 * __attribute__((aligned(4))).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ALIGN_H
#define AMINETXDUO_ALIGN_H

#include <stddef.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define AMI_ALIGN_LONG(type, member) \
    _Static_assert((offsetof(type, member) & 3) == 0, \
                   #type "." #member " is not longword aligned")
#else
#  define AMI_ALIGN_CAT2(a, b) a##b
#  define AMI_ALIGN_CAT(a, b)  AMI_ALIGN_CAT2(a, b)
#  define AMI_ALIGN_LONG(type, member) \
    typedef char AMI_ALIGN_CAT(ami_align_long_, __LINE__) \
        [(offsetof(type, member) & 3) == 0 ? 1 : -1]
#endif

#endif /* AMINETXDUO_ALIGN_H */
