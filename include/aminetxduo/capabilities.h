/* Safe extension checks through the existing SocketBaseTagList vector.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_CAPABILITIES_H
#define AMINETXDUO_CAPABILITIES_H
#include <exec/libraries.h>
#include <string.h>

/* Obtain release with SBTM_GETVAL(SBTC_RELEASESTRPTR), initial ti_Data=0.
 * Use it only when SocketBaseTagList returns zero. Unknown stacks/tags fail
 * closed. The returned string belongs to the open library; do not free it.
 * No private tag number is allocated or assumed to be globally reserved.
 *
 * Revision numbers have meaning only AFTER identifying the implementation.
 * last_offset is the positive magnitude of the last required negative LVO.
 * Evaluate this with stable local variables, not expressions with side effects.
 */
#define AMI_SOCKET_HAS_EXTENSION(base, release, revision, last_offset) \
    ((base) != NULL && (release) != NULL && \
     strncmp((release) != NULL ? (const char *)(release) : "", "AmiNetXDuo ", 11) == 0 && \
     (base)->lib_Version >= 4 && (base)->lib_Revision >= (revision) && \
     (base)->lib_NegSize >= (last_offset))

#define AMI_SOCKET_HAS_IFINDEX(base, release) \
    AMI_SOCKET_HAS_EXTENSION(base, release, 3, 0x384)
#define AMI_SOCKET_HAS_NETSTATUS(base, release) \
    AMI_SOCKET_HAS_EXTENSION(base, release, 10, 0x36c)
#endif
