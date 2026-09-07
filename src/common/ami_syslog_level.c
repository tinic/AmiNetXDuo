/*
 * BSD syslog priority -> ami_log() level.
 *
 * IN A FILE OF ITS OWN, AND PURE, so it can be tested without the Amiga.
 * The obvious home was beside bsd_vsyslog() in errno.c, but errno.c reaches
 * <aminetxduo/bpf.h>, whose `struct if_nameindex` collides with the host's
 * <net/if.h>, so a host test of it does not compile.  The arithmetic is the
 * only part that can be wrong, and it needs no headers at all.
 *
 * syslog.h packs a facility and a severity into one value and the severity is
 * the low three bits.  A daemon logging LOG_LOCAL0|LOG_ERR sends 131; read
 * whole, 131 is not a severity, so masking is not a tidiness question.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/syslog_level.h"

int ami_syslog_level(unsigned long priority)
{
    switch (priority & 7UL)
    {
    case 0:                             /* LOG_EMERG   */
    case 1:                             /* LOG_ALERT   */
    case 2:                             /* LOG_CRIT    */
    case 3:                             /* LOG_ERR     */
        return AMI_SYSLOG_LEVEL_ERROR;
    case 4:                             /* LOG_WARNING */
        return AMI_SYSLOG_LEVEL_WARN;
    default:                            /* NOTICE..DEBUG */
        return AMI_SYSLOG_LEVEL_INFO;
    }
}
