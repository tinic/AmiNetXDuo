/*
 * F-036: an IPv6 build without RFC 3542 ancillary data (AMINETXDUO_IPV6 on,
 * AMINETXDUO_CMSG off, a legal combination).  raw.c applies as_Icmp6Filter
 * to every raw ICMPv6 socket whatever AMINETXDUO_CMSG says, and ICMP6_FILTER,
 * the only setter, is not built.  The stub bsd_cmsg_reset() must therefore
 * install the RFC 3542 3.2 default, pass everything, as the built one does;
 * left zero, the socket received nothing but an empty ICMPv6 message.
 *
 * cmsg.c and socket.c are #included, built as that combination. New socket
 * defaults are checked for each family/type against the cleared allocator
 * contract, including the nonzero filter, multicast and address version tags.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AMINETXDUO_IPV6) || defined(AMINETXDUO_CMSG)
#error "this test is the IPV6=ON, CMSG=OFF build"
#endif

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }
VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size) { memcpy(dst, src, size); }

static BOOL h_alloc_fail;
static LONG h_socket_delta;

APTR ami_alloc(ULONG size)
{
    return h_alloc_fail ? NULL : calloc(1, (size_t)size);
}

VOID ami_mem_socket_delta(LONG delta) { h_socket_delta += delta; }

#include "cmsg.c"
#include "socket.c"

/* raw.c:288-297, the test a delivered ICMPv6 message of this type passes. */
static int passes(const AmiSocket *sock, ULONG type)
{
    return (sock->as_Icmp6Filter[type >> 5] & (1UL << (type & 31))) != 0UL;
}

int main(void)
{
    static AmiSocket sock;              /* zeroed, as a new socket's memory is */
    ULONG            type;
    int              all = 1;

    bsd_cmsg_reset(&sock);

    for (type = 0; type < 256; type++)
    {
        if (!passes(&sock, type))
            all = 0;
    }
    CHECK(all, "every ICMPv6 type passes a new raw socket's filter");
    CHECK(passes(&sock, 128) && passes(&sock, 129),
          "echo request and reply among them (ping6)");
    CHECK(sock.as_CmsgSticky.cs_Have == FALSE,
          "and the sticky ancillary state is still cleared");

    {
        static const UWORD domains[] = { AF_INET, AF_INET6 };
        static const UWORD types[] = { SOCK_STREAM, SOCK_DGRAM, SOCK_RAW };
        static const ULONG flags[] = { ASF_TCP, ASF_UDP, ASF_RAW };
        struct AmiSocketBase base;
        NXD_ADDRESS expected;
        unsigned int d, t;

        memset(&base, 0, sizeof(base));
        for (d = 0; d < sizeof(domains) / sizeof(domains[0]); d++)
        {
            memset(&expected, 0, sizeof(expected));
            expected.nxd_ip_version = (domains[d] == AF_INET6)
                                          ? NX_IP_VERSION_V6 : NX_IP_VERSION_V4;
            for (t = 0; t < sizeof(types) / sizeof(types[0]); t++)
            {
                AmiSocket *fresh = bsd_socket_alloc(&base, domains[d], types[t],
                                                    IPPROTO_ICMP);
                LONG before;

                CHECK(fresh != NULL, "new socket allocation succeeds");
                if (fresh == NULL)
                    continue;
                CHECK(memcmp(&fresh->as_LocalAddr, &expected,
                             sizeof(expected)) == 0 &&
                      memcmp(&fresh->as_PeerAddr, &expected,
                             sizeof(expected)) == 0,
                      "both complete unspecified addresses have family tags");
                CHECK(fresh->as_Flags == (flags[t] |
                          ((domains[d] == AF_INET6) ? ASF_INET6 : 0UL)),
                      "new socket family and type flags");
                CHECK(fresh->as_Owner == &base && fresh->as_RefCount == 1 &&
                      fresh->as_Domain == domains[d] &&
                      fresh->as_Type == types[t] &&
                      fresh->as_Protocol == IPPROTO_ICMP &&
                      fresh->as_Ttl == (LONG)NX_IP_TIME_TO_LIVE,
                      "new socket ownership, protocol and TTL defaults");
                CHECK(fresh->as_LocalPort == 0 && fresh->as_PeerPort == 0 &&
                      fresh->as_LocalScopeId == 0 && fresh->as_PeerScopeId == 0,
                      "new socket ports and scopes remain unset");
                CHECK(fresh->as_McastTtl == 1 && fresh->as_McastLoop == 1 &&
                      fresh->as_McastIf == -1 && fresh->as_Mcast6Hops == 1 &&
                      fresh->as_Mcast6If == -1,
                      "new socket multicast defaults retained");
                all = 1;
                for (type = 0; type < 256; type++)
                    if (!passes(fresh, type))
                        all = 0;
                CHECK(all && fresh->as_CmsgSticky.cs_Have == FALSE,
                      "allocated socket preserves ancillary/filter reset");
                free(fresh);

                before = h_socket_delta;
                h_alloc_fail = TRUE;
                CHECK(bsd_socket_alloc(&base, domains[d], types[t],
                                       IPPROTO_ICMP) == NULL,
                      "new socket allocation failure returns NULL");
                CHECK(h_socket_delta == before,
                      "failed socket allocation does not change accounting");
                h_alloc_fail = FALSE;
            }
        }
        CHECK(h_socket_delta == 6, "six successful allocations counted once");
    }

    printf("icmp6_filter_nocmsg: %lu checks, %lu failures\n",
           h_checks, h_failures);
    return (h_failures != 0) ? 1 : 0;
}
