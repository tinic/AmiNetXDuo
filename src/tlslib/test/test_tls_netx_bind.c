/*
 * tls.library, refresh a borrowed bsdsocket.library context on every open.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include "crypto68k.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;
static int h_obtain_calls;
static APTR h_refused_base;

#define CHECK(expr)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(expr))                                                       \
        {                                                                  \
            failures++;                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #expr);             \
        }                                                                  \
    } while (0)

VOID (*c68k_yield_hook)(VOID);

static const AmiNetXDuoContext h_context_a =
{
    .nxc_Magic   = AMI_NXD_CONTEXT_MAGIC,
    .nxc_Version = AMI_NXD_CONTEXT_VERSION,
    .nxc_Size    = sizeof(AmiNetXDuoContext)
};

static const AmiNetXDuoContext h_context_b =
{
    .nxc_Magic   = AMI_NXD_CONTEXT_MAGIC,
    .nxc_Version = AMI_NXD_CONTEXT_VERSION,
    .nxc_Size    = sizeof(AmiNetXDuoContext)
};

static struct Library h_socket_a;
static struct Library h_socket_b;

LONG tls_test_obtain_context(APTR socket_base, const AmiNetXDuoContext **out)
{
    h_obtain_calls++;

    if (socket_base == h_refused_base)
        return -1;

    *out = (socket_base == &h_socket_a) ? &h_context_a : &h_context_b;
    return 0;
}

int main(void)
{
    printf("tls_netx: every TLSOpen refreshes the borrowed context\n");

    memset(&h_socket_a, 0, sizeof(h_socket_a));
    memset(&h_socket_b, 0, sizeof(h_socket_b));
    h_socket_a.lib_NegSize = (UWORD)(-AMI_NXD_CONTEXT_LVO);
    h_socket_b.lib_NegSize = (UWORD)(-AMI_NXD_CONTEXT_LVO);

    CHECK(tls_netx_bind(&h_socket_a) == 0);
    CHECK(tls_netx_ctx() == &h_context_a);
    CHECK(h_obtain_calls == 1);

    /* A failed replacement must not break a connection that still uses the
       previous, live library. */
    h_refused_base = &h_socket_b;
    CHECK(tls_netx_bind(&h_socket_b) == -1);
    CHECK(tls_netx_ctx() == &h_context_a);
    CHECK(h_obtain_calls == 2);

    /* Once the replacement is valid, its table replaces the old segment's
       table instead of the stale pointer being treated as permanent. */
    h_refused_base = NULL;
    CHECK(tls_netx_bind(&h_socket_b) == 0);
    CHECK(tls_netx_ctx() == &h_context_b);
    CHECK(h_obtain_calls == 3);
    CHECK(c68k_yield_hook != NULL);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
