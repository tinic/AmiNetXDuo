/* ICMPv6 optnames must not alias IPv6 socket options when CMSG is disabled. */
#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <string.h>

static struct AmiSocketBase h_base;
static AmiSocket h_sock;
static int h_checks;
static int h_failures;
static LONG h_enter_result;
static int h_enters;
static int h_leaves;
static int h_applies;

#define CHECK(expr) do { h_checks++; if (!(expr)) { \
    h_failures++; printf("FAIL line %d: %s\n", __LINE__, #expr); \
} } while (0)

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size)
{
    memcpy(dst, src, size);
}

LONG bsd_cmsg_option(struct AmiSocketBase *base, AmiSocket *sock, LONG level,
                     LONG optname, APTR optval, socklen_t *optlen, BOOL set)
{
    (VOID)base; (VOID)sock; (VOID)level; (VOID)optname;
    (VOID)optval; (VOID)optlen; (VOID)set;
    return 1; /* CMSG=OFF contract: not handled here. */
}

LONG bsd_nx_enter(struct AmiSocketBase *base)
{ (VOID)base; h_enters++; return h_enter_result; }
VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; h_leaves++; }
VOID bsd_opt_apply_ip(AmiSocket *sock) { (VOID)sock; h_applies++; }

/* This host shim has Linux socket layouts; the target ABI assertions are
   covered by the m68k build, while this test exercises the option logic. */
#define _Static_assert(condition, message)
#include "in6.c"
#undef _Static_assert

static void test_hops_and_class(void)
{
    static const LONG options[2][2] = {
        { AMI_IPV6_UNICAST_HOPS_BSD, AMI_IPV6_UNICAST_HOPS_LINUX },
        { AMI_IPV6_TCLASS_BSD, AMI_IPV6_TCLASS_LINUX }
    };
    static const LONG values[] = { -2, -1, 0, 255, 256, 65536 };
    unsigned int kind, numbering, raw, width, v;

    for (kind = 0; kind < 2; kind++)
    for (numbering = 0; numbering < 2; numbering++)
    for (raw = 0; raw < 2; raw++)
    for (width = 0; width < 2; width++)
    for (v = 0; v < sizeof(values) / sizeof(values[0]); v++)
    {
        LONG value = values[v];
        WORD short_value = (WORD)value;
        LONG actual = width == 0 ? value : (LONG)short_value;
        LONG rc;
        BOOL numbered = raw == 0 || numbering == 0 ||
                        options[kind][numbering] == options[kind][0];
        BOOL valid = actual >= -1 && actual <= 255;
        LONG expected = actual < 0
                            ? (kind == 0 ? (LONG)NX_IP_TIME_TO_LIVE : 0)
                            : actual;

        memset(&h_base, 0, sizeof(h_base));
        memset(&h_sock, 0, sizeof(h_sock));
        h_sock.as_Flags = ASF_INET6 | (raw ? ASF_RAW : 0UL);
        h_sock.as_Ttl = 9;
        h_sock.as_Tos = 17;
        h_enter_result = 0;
        h_enters = h_leaves = h_applies = 0;
        rc = bsd_setsockopt_ipv6(&h_base, &h_sock, IPPROTO_IPV6,
                                 options[kind][numbering],
                                 width == 0 ? (APTR)&value : (APTR)&short_value,
                                 (socklen_t)(width == 0 ? sizeof(value)
                                                        : sizeof(short_value)));
        if (numbered && valid)
        {
            CHECK(rc == 0);
            CHECK(h_sock.as_Ttl == (kind == 0 ? expected : 9));
            CHECK(h_sock.as_Tos == (kind == 1 ? expected : 17));
            CHECK(h_enters == 1 && h_applies == 1 && h_leaves == 1);
        }
        else
        {
            CHECK(rc == -1);
            CHECK(h_base.sb_Errno == (numbered ? AMI_EINVAL : AMI_ENOPROTOOPT));
            CHECK(h_sock.as_Ttl == 9 && h_sock.as_Tos == 17);
            CHECK(h_enters == 0 && h_applies == 0 && h_leaves == 0);
        }
    }

    for (kind = 0; kind < 2; kind++)
    {
        LONG value = 42;

        h_sock.as_Flags = ASF_INET6;
        h_sock.as_Ttl = 9;
        h_sock.as_Tos = 17;
        h_enter_result = -1;
        h_enters = h_leaves = h_applies = 0;
        CHECK(bsd_setsockopt_ipv6(&h_base, &h_sock, IPPROTO_IPV6,
                                  options[kind][0], &value,
                                  (socklen_t)sizeof(value)) == -1);
        CHECK(h_base.sb_Errno == AMI_ENETDOWN);
        CHECK(h_sock.as_Ttl == (kind == 0 ? value : 9));
        CHECK(h_sock.as_Tos == (kind == 1 ? value : 17));
        CHECK(h_enters == 1 && h_applies == 0 && h_leaves == 0);
    }
}

int main(void)
{
    LONG value = 42;
    socklen_t len = (socklen_t)sizeof(value);

    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    h_sock.as_Flags = ASF_INET6;
    h_sock.as_Ttl = 9;

    CHECK(bsd_setsockopt_ipv6(&h_base, &h_sock, IPPROTO_ICMPV6,
                              AMI_IPV6_UNICAST_HOPS_BSD, &value,
                              (socklen_t)sizeof(value)) == -1);
    CHECK(h_base.sb_Errno == AMI_ENOPROTOOPT);
    CHECK(h_sock.as_Ttl == 9);

    CHECK(bsd_getsockopt_ipv6(&h_base, &h_sock, IPPROTO_ICMPV6,
                              AMI_IPV6_UNICAST_HOPS_BSD, &value, &len) == -1);
    CHECK(h_base.sb_Errno == AMI_ENOPROTOOPT);
    CHECK(value == 42);

    test_hops_and_class();

    CHECK(bsd_setsockopt_ipv6(&h_base, &h_sock, IPPROTO_IPV6,
                              AMI_IPV6_UNICAST_HOPS_BSD, &value,
                              (socklen_t)sizeof(value)) == 0);
    CHECK(h_sock.as_Ttl == 42);
    value = 0;
    CHECK(bsd_getsockopt_ipv6(&h_base, &h_sock, IPPROTO_IPV6,
                              AMI_IPV6_UNICAST_HOPS_BSD, &value, &len) == 0);
    CHECK(value == 42);

    printf("RESULT in6_option_level checks=%d failures=%d\n",
           h_checks, h_failures);
    return h_failures != 0;
}
