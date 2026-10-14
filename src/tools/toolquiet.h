/*
 * QUIET's return code, as the AmigaDOS Command Reference documents it for
 * AddNetInterface, AddNetRoute, ConfigureNetInterface and DeleteNetRoute:
 * "if the program encounters an error it will flag this as failure code 5".
 * A Roadshow S:Network-Startup line that fails then trips `IF WARN`, and a
 * script at the default FAILAT 10 goes on.
 *
 * Plain C, so the host test runs the mapping the commands run.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLQUIET_H
#define AMINETXDUO_TOOLQUIET_H

#define TOOL_QUIET_RC   5L      /* RETURN_WARN */

static inline long tool_quiet_rc(int quiet, long rc)
{
    return (quiet && rc > TOOL_QUIET_RC) ? TOOL_QUIET_RC : rc;
}

#endif /* AMINETXDUO_TOOLQUIET_H */
