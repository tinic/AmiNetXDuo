/*
 * AmiNetXDuo, the allocation census.
 *
 * Off unless AMINETXDUO_ALLOCCENSUS is defined, and then every ami_alloc()
 * carries the file and line it came from, and a teardown can print what is
 * still held and by which site.  With the flag off nothing here expands to
 * anything and ami_alloc() is the plain function it always was.
 *
 * The counters in compat.h say HOW MANY blocks are outstanding.  That is
 * enough to notice a leak and not enough to find one: the three leaks of
 * 2026-08-09 were each a block whose owner was not the thing that freed it,
 * and the question every one of them turned on was WHICH allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ALLOCCENSUS_H
#define AMINETXDUO_ALLOCCENSUS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AMINETXDUO_ALLOCCENSUS

/*
 * The tag is the literal "file.c:123".  On m68k-amigaos there is no .rodata,
 * so these land in plain .text alongside the code (the long note at the head
 * of src/tools/tool_devdiag.c), which is why they exist only in a census
 * build: a shipping binary would carry every one of them forever.
 *
 * A compact site id was the alternative.  It was not taken because the tag is
 * also the aggregation KEY: one literal per call site means the pointer is
 * unique per site inside a translation unit, so the report groups by pointer
 * comparison and needs no registry, no init order and no id space to keep in
 * step with the sources.
 */
#define AMI_CENSUS_STR2(x)  #x
#define AMI_CENSUS_STR(x)   AMI_CENSUS_STR2(x)
#define AMI_CENSUS_SITE     __FILE__ ":" AMI_CENSUS_STR(__LINE__)

/* What compat.h redirects ami_alloc()/ami_alloc_flags() to. */
APTR ami_alloc_tagged(ULONG size, const char *site);
APTR ami_alloc_flags_tagged(ULONG size, ULONG memf, const char *site);

/*
 * For memory that does not come from ami_alloc().  src/ has a handful of
 * direct AllocMem()/AllocVec() sites that cannot use the wrapper: the child
 * library bases, which must be MEMF_PUBLIC and are freed with FreeMem() by
 * code that never sees an ami_alloc() header, and the ThreadX caller records.
 * Those are exactly the owner-ambiguous blocks, so they are enrolled by hand.
 */
VOID ami_census_add(APTR ptr, ULONG size, const char *site);
VOID ami_census_drop(APTR ptr);

/* Print the outstanding set to the serial port, ordered by bytes. */
VOID ami_census_report(const char *scope);

#define AMI_CENSUS_ADD(ptr, size)   ami_census_add((APTR)(ptr), (ULONG)(size), \
                                                   AMI_CENSUS_SITE)
#define AMI_CENSUS_DROP(ptr)        ami_census_drop((APTR)(ptr))
#define AMI_CENSUS_REPORT(scope)    ami_census_report((scope))

#else   /* !AMINETXDUO_ALLOCCENSUS */

#define AMI_CENSUS_ADD(ptr, size)   ((VOID)0)
#define AMI_CENSUS_DROP(ptr)        ((VOID)0)
#define AMI_CENSUS_REPORT(scope)    ((VOID)0)

#endif  /* AMINETXDUO_ALLOCCENSUS */

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_ALLOCCENSUS_H */
