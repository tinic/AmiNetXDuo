/*
 * AmiNetXDuo, the allocation census.  Nothing here expands to anything unless
 * AMINETXDUO_ALLOCCENSUS is defined.
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
 * The tag is the literal "file.c:123", and its POINTER is the aggregation key:
 * one literal per call site, grouped by pointer comparison.  On m68k-amigaos
 * these land in .text, so a census build is the only place they may exist.
 */
#define AMI_CENSUS_STR2(x)  #x
#define AMI_CENSUS_STR(x)   AMI_CENSUS_STR2(x)
#define AMI_CENSUS_SITE     __FILE__ ":" AMI_CENSUS_STR(__LINE__)

/* What compat.h redirects ami_alloc()/ami_alloc_flags() to. */
APTR ami_alloc_tagged(ULONG size, const char *site);
APTR ami_alloc_flags_tagged(ULONG size, ULONG memf, const char *site);

/* For memory that does not come from ami_alloc(): the direct AllocMem()/
   AllocVec() sites, enrolled by hand. */
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
