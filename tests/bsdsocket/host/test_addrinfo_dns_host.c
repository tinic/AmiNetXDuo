/*
 * Run the real getaddrinfo() against controlled DNS answers. An IPv4 LAN
 * with automatic link-local IPv6 must make no AAAA query by default, even
 * when the name has both A and AAAA records. Count queries as well as results:
 * returning IPv4 first would still leave the extra DNS timeout regression.
 * SPDX-License-Identifier: MIT
 */
#include "bsdsocket_vectors.h"
#include "aminetxduo/config.h"
#include "interfaces.h"

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
#define AI_ADDRCONFIG 0x0400
#define AI_MASK (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV | AI_ADDRCONFIG)
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

#define CHECK(expr) do { h_checks++; if (!(expr)) { \
    h_failures++; printf("FAIL line %d: %s\n", __LINE__, #expr); \
} } while (0)

BOOL netstack_ipv6_enabled(VOID) { return h_enabled; }
BOOL netstack_ipv6_have_global(VOID) { return h_global; }
BOOL netstack_ipv4_configured(VOID) { return h_v4; }
BOOL netstack_ipv6_configured(VOID) { return h_v6; }
BOOL bsd_resolve_break(VOID *arg) { (VOID)arg; return FALSE; }
LONG bsd_if_index_of(NX_IP *ip, const char *name)
{ (VOID)ip; (VOID)name; return -1; }

LONG netstack_resolve_until(const char *name, ULONG *out, ULONG timeout,
                            AmiNetGiveUpFn stop, VOID *arg)
{
    (VOID)timeout; (VOID)stop; (VOID)arg;
    CHECK(strcmp(name, "dual.example") == 0);
    h_queries4++;
    *out = 0xc0000201UL;
    return AMI_NET_OK;
}

LONG netstack_resolve6_until(const char *name, ULONG out[4], ULONG timeout,
                             AmiNetGiveUpFn stop, VOID *arg)
{
    (VOID)timeout; (VOID)stop; (VOID)arg;
    CHECK(strcmp(name, "dual.example") == 0);
    h_queries6++;
    out[0] = 0x20010db8UL; out[1] = out[2] = 0; out[3] = 1;
    return h_reply6;
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

int main(void)
{
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
    printf("RESULT addrinfo_dns checks=%u failures=%u\n", h_checks, h_failures);
    return h_failures != 0;
}
