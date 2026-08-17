/*
 * Host test shim, struct Hook.
 *
 * <aminetxduo/tlslib.h> includes this for TLSA_VerifyHook, and tls_internal.h
 * includes that header, so the tlslib host tests need it even though none of
 * them calls a hook.  Referenced only through pointers here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_UTILITY_HOOKS_H
#define AMINETXDUO_TEST_UTILITY_HOOKS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Hook {
    struct MinNode  h_MinNode;
    ULONG         (*h_Entry)(void);
    ULONG         (*h_SubEntry)(void);
    APTR            h_Data;
};

#endif
