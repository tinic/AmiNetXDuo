/*
 * Run the real getaddrinfo() against controlled DNS answers. An IPv4 LAN
 * with automatic link-local IPv6 must make no AAAA query by default, even
 * when the name has both A and AAAA records. Count queries as well as results:
 * returning IPv4 first would still leave the extra DNS timeout regression.
 * The RFC 6724 ordering cases model the stack's source selection: no source
 * of a wide enough scope, or off-link with no router, means no route.
 * SPDX-License-Identifier: MIT
 */
#include "bsdsocket_vectors.h"
#include "aminetxduo/config.h"
#include "interfaces.h"
#include "../ipv6/ipv6_srcsel.h"

/* libc supplies the host structures, not the target resolver flag namespace. */
#undef AI_PASSIVE
#undef AI_CANONNAME
#undef AI_NUMERICHOST
#undef AI_NUMERICSERV
#undef AI_ADDRCONFIG
#undef AI_MASK
#undef AI_EXT
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define AI_EXT 8
#define AI_NUMERICSERV 16
#include "aminetxduo/addrinfo.h"
#define AI_MASK (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV)
/* Linux lacks these BSD errors; no host resolver consumes these values. */
#ifndef EAI_BADHINTS
#define EAI_BADHINTS (-1001)
#endif
#ifndef EAI_PROTOCOL
#define EAI_PROTOCOL (-1002)
#endif
#ifndef EAI_ADDRFAMILY
#define EAI_ADDRFAMILY (-1003)
#endif
#ifndef EAI_NODATA
#define EAI_NODATA (-1004)
#endif
#ifndef NI_WITHSCOPEID
#define NI_WITHSCOPEID 32
#endif

/* As in host_prelude.h's IPv6 shape, only the target writes this structure. */
#if !defined(__APPLE__)
/* shim/net/route.h maps sin_len onto glibc sin_zero; this one has both. */
#undef sin_len
#define sockaddr_in ami_sockaddr_in
struct ami_sockaddr_in {
    UBYTE sin_len;
    UBYTE sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    UBYTE sin_zero[8];
};
#endif

static struct AmiSocketBase h_base;
static BOOL h_enabled, h_global, h_v4, h_v6;
static unsigned h_queries4, h_queries6, h_checks, h_failures, h_live;
static LONG h_reply6;
/* Ordering model: one IPv6 source, IPv4 192.168.1.10/24. */
static BOOL h_have_s6, h_router6, h_route4, h_gw4;
static ULONG h_s6[4], h_s6_len, h_aaaa[4], h_a;
/* The same source address on two interfaces; the route picks h_sel_if. */
static ULONG h_s6_state[2];
static unsigned h_sel_if;

#define CHECK(expr) do { h_checks++; if (!(expr)) { \
    h_failures++; printf("FAIL line %d: %s\n", __LINE__, #expr); \
} } while (0)

BOOL netstack_ipv6_enabled(VOID) { return h_enabled; }
BOOL netstack_ipv6_have_global(VOID) { return h_global; }
BOOL netstack_ipv4_configured(VOID) { return h_v4; }
BOOL netstack_ipv6_configured(VOID) { return h_v6; }
BOOL bsd_resolve_break(VOID *arg, ULONG pause)
{ (VOID)arg; (VOID)pause; return FALSE; }
LONG bsd_if_index_of(NX_IP *ip, const char *name)
{ (VOID)ip; (VOID)name; return -1; }

LONG netstack_resolve_until(const char *name, ULONG *out, ULONG timeout,
                            AmiNetGiveUpFn stop, VOID *arg)
{
    (VOID)timeout; (VOID)stop; (VOID)arg;
    CHECK(strcmp(name, "dual.example") == 0);
    h_queries4++;
    *out = h_a;
    return AMI_NET_OK;
}

LONG netstack_resolve6_until(const char *name, ULONG out[4], ULONG timeout,
                             AmiNetGiveUpFn stop, VOID *arg)
{
    (VOID)timeout; (VOID)stop; (VOID)arg;
    CHECK(strcmp(name, "dual.example") == 0);
    h_queries6++;
    memcpy(out, h_aaaa, sizeof(h_aaaa));
    return h_reply6;
}

LONG bsd_nx_enter(struct AmiSocketBase *base) { (VOID)base; return 0; }
VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; }

BOOL netstack_ipv6_source_state(const ULONG dest[4], ULONG addr_out[4],
                                ULONG *state_out)
{
    if (!h_have_s6 || anx6_scope(h_s6) < anx6_scope(dest))
        return FALSE;
    if (anx6_scope(dest) > 2 && !h_router6 &&
        (ULONG)anx6_common_prefix_len(h_s6, dest) < h_s6_len)
        return FALSE;
    memcpy(addr_out, h_s6, sizeof(h_s6));
    *state_out = h_s6_state[h_sel_if];
    return TRUE;
}

BOOL netstack_ipv4_route(ULONG dest, LONG index, UWORD *index_out,
                         ULONG *next_hop_out, ULONG *source_out)
{
    (VOID)index; (VOID)index_out; (VOID)next_hop_out;
    if (!h_route4 || (!h_gw4 && (dest & 0xffffff00UL) != 0xc0a80100UL))
        return FALSE;
    *source_out = 0xc0a8010aUL;
    return TRUE;
}

APTR ami_alloc(ULONG size)
{
    APTR p = malloc(size);
    if (p != NULL) h_live++;
    return p;
}
VOID ami_free(APTR p) { if (p != NULL) h_live--; free(p); }
VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }
ULONG bsd_strlen(const char *s) { return (ULONG)strlen(s); }
VOID bsd_strncpy(char *dst, const char *src, ULONG size)
{ if (size) { strncpy(dst, src, size); dst[size - 1] = 0; } }
VOID bsd_set_errno(struct AmiSocketBase *base, LONG err) { base->sb_Errno = err; }
VOID bsd_addr_from_v4(NXD_ADDRESS *out, ULONG v4)
{ out->nxd_ip_version = NX_IP_VERSION_V4; out->nxd_ip_address.v4 = v4; }
VOID bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16])
{
    unsigned i;
    for (i = 0; i < 16; i++)
        bytes[i] = (UBYTE)(words[i / 4] >> (24 - (i % 4) * 8));
}
const AmiNetdbEntry *ami_netdb_serv_by_name(const char *name, const char *proto)
{ (VOID)name; (VOID)proto; return NULL; }

#include "addrinfo.c"

static void lookup(const char *name, LONG family, LONG flags, BOOL null_hints,
                   unsigned queries4, unsigned queries6, LONG first, LONG second)
{
    struct addrinfo hints, *list = NULL;
    LONG rc;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_flags = flags;
    hints.ai_socktype = SOCK_STREAM;
    h_queries4 = h_queries6 = 0;
    rc = bsd_getaddrinfo((STRPTR)name, (STRPTR)"80",
                         null_hints ? NULL : &hints, &list, &h_base);
    CHECK(rc == (first == AF_UNSPEC ? EAI_NONAME : 0));
    CHECK(h_queries4 == queries4);
    CHECK(h_queries6 == queries6);
    if (first == AF_UNSPEC)
        CHECK(list == NULL);
    else
    {
        CHECK(list != NULL);
        if (list != NULL)
        {
            CHECK(list->ai_family == first);
            if (second == AF_UNSPEC)
                CHECK(list->ai_next == NULL);
            else
            {
                CHECK(list->ai_next != NULL);
                if (list->ai_next != NULL)
                {
                    CHECK(list->ai_next->ai_family == second);
                    CHECK(list->ai_next->ai_next == NULL);
                }
            }
        }
    }
    bsd_freeaddrinfo(list, &h_base);
    CHECK(h_live == 0);
}

static void set6(ULONG w0, ULONG w1, ULONG w3, ULONG len)
{
    h_have_s6 = TRUE;
    h_s6[0] = w0; h_s6[1] = w1; h_s6[2] = 0; h_s6[3] = w3;
    h_s6_len = len;
    h_s6_state[0] = h_s6_state[1] = NX_IPV6_ADDR_STATE_PREFERRED;
    h_sel_if = 0;
}

static void aaaa(ULONG w0, ULONG w1)
{ h_aaaa[0] = w0; h_aaaa[1] = w1; h_aaaa[2] = 0; h_aaaa[3] = 1; }

/* AF_UNSPEC with AI_ADDRCONFIG: both families queried, then ordered. */
static void order(const char *why, LONG first)
{
    printf("case %s\n", why);
    lookup("dual.example", AF_UNSPEC, AI_ADDRCONFIG, FALSE, 1, 1, first,
           first == AF_INET ? AF_INET6 : AF_INET);
}

/* Detach the second node, free one half, read the other's name. */
static void canon_detach(LONG first, BOOL free_head)
{
    struct addrinfo hints, *list = NULL, *tail, *keep;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG | AI_CANONNAME;
    CHECK(bsd_getaddrinfo((STRPTR)"dual.example", (STRPTR)"80", &hints,
                          &list, &h_base) == 0);
    if (list == NULL || list->ai_next == NULL)
    {
        CHECK(!"two nodes");
        bsd_freeaddrinfo(list, &h_base);
        return;
    }
    CHECK(list->ai_family == first);
    CHECK(list->ai_canonname != NULL &&
          strcmp(list->ai_canonname, "dual.example") == 0);
    tail = list->ai_next;
    CHECK(tail->ai_next == NULL);
    list->ai_next = NULL;
    bsd_freeaddrinfo(free_head ? list : tail, &h_base);
    keep = free_head ? tail : list;
    CHECK(keep->ai_canonname != NULL &&
          strcmp(keep->ai_canonname, "dual.example") == 0);
    bsd_freeaddrinfo(keep, &h_base);
    CHECK(h_live == 0);
}

static void rfc6724(void)
{
    h_enabled = h_v4 = h_v6 = TRUE;
    h_global = FALSE;
    h_reply6 = AMI_NET_OK;

    /* Usable IPv4 against IPv6 that fails rule 1. */
    set6(0xfe800000UL, 0, 0x10, 64);
    aaaa(0x20010db8UL, 0x00010000UL);
    order("link_local_source_global_aaaa", AF_INET);

    /* fd00::10/64 with a router: rule 5, labels 13 and 1 differ. */
    set6(0xfd000000UL, 0, 0x10, 64);
    h_router6 = TRUE;
    order("ula_source_global_aaaa", AF_INET);

    /* On-link ULA, no router: usable, but RFC 6724 2.1 ranks fc00::/7
       (precedence 3) below IPv4 (35), rule 6. */
    h_router6 = FALSE;
    aaaa(0xfd000000UL, 0);
    order("ula_onlink_aaaa_usable_ipv4", AF_INET);
    /* The same AAAA against an off-link A with no IPv4 gateway. */
    h_gw4 = FALSE;
    order("ula_onlink_aaaa_unusable_ipv4", AF_INET6);
    /* Off-link ULA too: both fail rule 1, a tie keeps IPv6 first. */
    aaaa(0xfd000001UL, 0);
    order("stable_tie_both_unusable", AF_INET6);
    h_gw4 = TRUE;

    /* Global source. */
    set6(0x20010db8UL, 0x00010000UL, 0x10, 64);
    aaaa(0x20010db8UL, 0x00020000UL);
    h_router6 = TRUE;
    order("global_source_router", AF_INET6);
    /* Rule 3 follows the selected interface's entry, not the first one
       holding the same address. */
    h_s6_state[0] = NX_IPV6_ADDR_STATE_DEPRECATED;
    h_sel_if = 1;
    order("deprecated_twin_not_selected", AF_INET6);
    h_sel_if = 0;
    order("deprecated_global_source", AF_INET);
    h_s6_state[0] = NX_IPV6_ADDR_STATE_PREFERRED;
    h_router6 = FALSE;
    order("global_source_no_router_offlink", AF_INET);

    /* IPv6-only host: the A answer has no IPv4 route. */
    h_router6 = TRUE;
    h_global = TRUE;
    h_route4 = FALSE;
    printf("case ipv6_only_host\n");
    lookup("dual.example", AF_UNSPEC, 0, FALSE, 1, 1, AF_INET6, AF_INET);
    h_route4 = TRUE;

    /* Each node owns its ai_canonname: free either one detached, then read
       the other (ASan in the sanitize build). Swapped, then not. */
    printf("case canonname_after_swap\n");
    set6(0xfe800000UL, 0, 0x10, 64);
    canon_detach(AF_INET, FALSE);
    canon_detach(AF_INET, TRUE);
    printf("case canonname_unswapped\n");
    set6(0x20010db8UL, 0x00010000UL, 0x10, 64);
    canon_detach(AF_INET6, FALSE);
    canon_detach(AF_INET6, TRUE);
}

int main(void)
{
    /* Default model: a routed global IPv6 source orders AAAA first. */
    set6(0x20010db8UL, 0x00010000UL, 0x10, 64);
    aaaa(0x20010db8UL, 0);
    h_a = 0xc0000201UL;
    h_router6 = h_route4 = h_gw4 = TRUE;

    h_enabled = h_v4 = h_v6 = TRUE;
    h_global = FALSE; /* IPv4 LAN plus link-local, or ULA without global IPv6. */
    h_reply6 = AMI_NET_OK;
    lookup("dual.example", AF_UNSPEC, 0, FALSE, 1, 0, AF_INET, AF_UNSPEC);
    lookup("dual.example", AF_UNSPEC, 0, TRUE, 1, 0, AF_INET, AF_UNSPEC);
    lookup("dual.example", AF_INET, 0, FALSE, 1, 0, AF_INET, AF_UNSPEC);
    /* Explicit family and configuration requests still support private IPv6. */
    lookup("dual.example", AF_INET6, 0, FALSE, 0, 1, AF_INET6, AF_UNSPEC);
    lookup("dual.example", AF_UNSPEC, AI_ADDRCONFIG, FALSE, 1, 1, AF_INET6, AF_INET);
    lookup("dual.example", AF_INET6, AI_ADDRCONFIG, FALSE, 0, 1, AF_INET6, AF_UNSPEC);
    /* Numeric and wildcard results are not subject to the DNS-only gate. */
    lookup("fd00::1", AF_UNSPEC, AI_NUMERICHOST, FALSE, 0, 0, AF_INET6, AF_UNSPEC);
    lookup(NULL, AF_UNSPEC, AI_PASSIVE, FALSE, 0, 0, AF_INET6, AF_INET);

    h_v6 = FALSE;
    lookup("dual.example", AF_UNSPEC, AI_ADDRCONFIG, FALSE, 1, 0, AF_INET, AF_UNSPEC);
    lookup("dual.example", AF_INET6, AI_ADDRCONFIG, FALSE, 0, 0, AF_UNSPEC, AF_UNSPEC);
    h_v4 = FALSE;
    lookup("dual.example", AF_UNSPEC, AI_ADDRCONFIG, FALSE, 0, 0, AF_UNSPEC, AF_UNSPEC);
    h_v6 = TRUE;
    lookup("dual.example", AF_UNSPEC, AI_ADDRCONFIG, FALSE, 0, 1, AF_INET6, AF_UNSPEC);

    h_global = h_v4 = TRUE;
    lookup("dual.example", AF_UNSPEC, 0, FALSE, 1, 1, AF_INET6, AF_INET);
    lookup("dual.example", AF_UNSPEC, 0, TRUE, 1, 1, AF_INET6, AF_INET);
    lookup("dual.example", AF_INET, 0, FALSE, 1, 0, AF_INET, AF_UNSPEC);
    h_reply6 = AMI_NET_ERR_TIMEOUT;
    lookup("dual.example", AF_UNSPEC, 0, FALSE, 1, 1, AF_INET, AF_UNSPEC);
    h_enabled = h_global = h_v6 = FALSE;
    lookup("dual.example", AF_UNSPEC, 0, FALSE, 1, 0, AF_INET, AF_UNSPEC);
    lookup("dual.example", AF_INET6, 0, FALSE, 0, 0, AF_UNSPEC, AF_UNSPEC);
    rfc6724();
    printf("RESULT addrinfo_dns checks=%u failures=%u\n", h_checks, h_failures);
    return h_failures != 0;
}
