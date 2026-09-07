/*
 * BSD syslog priority -> ami_log() level.  See src/common/ami_syslog_level.c
 * for why this is a file of its own rather than part of errno.c.
 *
 * The three values are AMI_LOG_ERROR, AMI_LOG_WARN and AMI_LOG_INFO from
 * <aminetxduo/compat.h>, repeated here so that this header pulls in nothing:
 * ami_syslog_level() has to compile on a host that has no Amiga types, and a
 * _Static_assert in errno.c holds the two sets equal.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SYSLOG_LEVEL_H
#define AMINETXDUO_SYSLOG_LEVEL_H

#define AMI_SYSLOG_LEVEL_ERROR  0
#define AMI_SYSLOG_LEVEL_WARN   1
#define AMI_SYSLOG_LEVEL_INFO   2

int ami_syslog_level(unsigned long priority);

#endif /* AMINETXDUO_SYSLOG_LEVEL_H */
