/*
 * AmiNetXDuo, host-side test for the configuration and netdb parsers.
 *
 * Host build (cc -std=c99), not the Amiga: file I/O and the DEVS:NetInterfaces
 * scan are replaced by the in-memory stubs below.  config_file.c is therefore
 * NOT covered here.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../config_internal.h"
#include "aminetxduo/compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;
static int   stub_verbose;

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    p = calloc(1, size);
    if (p != NULL)
        stub_outstanding++;

    return p;
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID)
{
    return stub_outstanding;
}

VOID ami_log(int level, const char *fmt, ...)
{
    va_list args;

    (void)level;
    if (!stub_verbose)
        return;

    va_start(args, fmt);
    fputs("  [log] ", stdout);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
}

/* Fixture table standing in for the DEVS: files. */
static struct Fixture
{
    const char *path;
    const char *text;
}
fixtures[48];       /* a drawer of nine, plus the DEVS:Internet files */

static void set_fixture(const char *path, const char *text)
{
    unsigned i;

    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++)
    {
        if (fixtures[i].path == NULL || strcmp(fixtures[i].path, path) == 0)
        {
            fixtures[i].path = path;
            fixtures[i].text = text;
            return;
        }
    }
}

static void clear_fixtures(void)
{
    memset(fixtures, 0, sizeof(fixtures));
}

#define DRAWER_MAX 16

static char     drawer_name[DRAWER_MAX][AMI_CFG_NAME_LEN];
static char     drawer_path[DRAWER_MAX][AMI_CFG_NAME_LEN + 32];
static char     drawer_text[DRAWER_MAX][160];
static unsigned drawer_count;

static void clear_drawer(void)
{
    drawer_count = 0;
}

/* One interface file: it appears in the scan AND to ami_cfg_read_file(). */
static void stage_interface(const char *name, unsigned unit)
{
    unsigned i = drawer_count++;

    (void)snprintf(drawer_name[i], sizeof(drawer_name[i]), "%s", name);
    (void)snprintf(drawer_path[i], sizeof(drawer_path[i]),
                   "DEVS:NetInterfaces/%s", name);
    (void)snprintf(drawer_text[i], sizeof(drawer_text[i]),
                   "DEVICE=ariadne.device\nUNIT=%u\nCONFIGURE=DHCP\n", unit);

    set_fixture(drawer_path[i], drawer_text[i]);
}

/* Handed over in STAGING order, not sorted. */
BOOL ami_cfg_scan_interfaces(AmiConfig *cfg, AmiCfgIfaceSink sink)
{
    unsigned i;

    if (cfg == NULL || sink == NULL)
        return FALSE;

    for (i = 0; i < drawer_count; i++)
        sink(cfg, drawer_name[i]);

    return TRUE;
}

APTR ami_cfg_read_file(const char *path, ULONG *size_out)
{
    unsigned i;

    if (size_out != NULL)
        *size_out = 0;

    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++)
    {
        if (fixtures[i].path != NULL && strcmp(fixtures[i].path, path) == 0)
        {
            size_t len = strlen(fixtures[i].text);
            char  *buf = (char *)ami_alloc((ULONG)len + 1);

            if (buf == NULL)
                return NULL;

            memcpy(buf, fixtures[i].text, len + 1);
            if (size_out != NULL)
                *size_out = (ULONG)len;

            return buf;
        }
    }

    return NULL;       /* absent, which is never an error */
}

/* --------------------------------------------------------------- harness */

static int failures;
static int checks;

/* CHECK_STR is handed arrays as often as pointers, and `arr ? arr : ...` is
   -Waddress. The decay happens at the call, so the test is a real one here. */
static const char *or_null(const char *s)
{
    return s != NULL ? s : "(null)";
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        checks++;                                                            \
        if ((got) == NULL || strcmp((got), (want)) != 0) {                    \
            failures++;                                                      \
            printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n",            \
                   __FILE__, __LINE__, (want), or_null(got));                \
        }                                                                    \
    } while (0)

#define CHECK_IP(got, a, b, c, d)                                            \
    CHECK((got) == (((ULONG)(a) << 24) | ((ULONG)(b) << 16) |                \
                    ((ULONG)(c) << 8)  |  (ULONG)(d)))

/*
 * A configuration with `count` empty descriptions, ready to be filled.
 * Frees the previous list first, so the caller must zero the struct once at
 * its declaration and must end with ami_config_free().
 */
static void cfg_reset(AmiConfig *cfg, UWORD count)
{
    ami_config_free(cfg);
    memset(cfg, 0, sizeof(*cfg));

    if (count > 0)
    {
        CHECK(ami_config_reserve(cfg, count));
        cfg->interface_count = count;
    }
}

/* A mutable copy, because every parser here tokenises in place. */
static char *dup_text(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);

    strcpy(p, s);
    return p;
}

/* ------------------------------------------------------------------ tests */

static void test_text_helpers(void)
{
    char *buf = dup_text("first\r\nsecond\rthird\nfourth");
    char *cursor = buf;
    char *line;

    printf("text helpers\n");

    line = ami_cfg_next_line(&cursor); CHECK_STR(line, "first");
    line = ami_cfg_next_line(&cursor); CHECK_STR(line, "second");
    line = ami_cfg_next_line(&cursor); CHECK_STR(line, "third");
    line = ami_cfg_next_line(&cursor); CHECK_STR(line, "fourth");
    CHECK(ami_cfg_next_line(&cursor) == NULL);
    free(buf);

    buf = dup_text("  \tdevice=x.device \t \r");
    CHECK_STR(ami_cfg_trim(buf), "device=x.device");
    free(buf);

    buf = dup_text("address=1.2.3.4 ; trailing comment");
    ami_cfg_strip_comment(buf, "#;");
    CHECK_STR(ami_cfg_trim(buf), "address=1.2.3.4");
    free(buf);

    /* A comment character inside quotes is not a comment. */
    buf = dup_text("id=\"my#host\" # real comment");
    ami_cfg_strip_comment(buf, "#;");
    CHECK_STR(ami_cfg_trim(buf), "id=\"my#host\"");
    free(buf);

    /* KEY=value, KEY = value, KEY value, several pairs per line. */
    {
        char *key, *value;

        buf = dup_text("dst = 10.0.0.0 via 192.168.1.1 metric=3");
        cursor = buf;
        CHECK(ami_cfg_next_pair(&cursor, &key, &value));
        CHECK_STR(key, "dst"); CHECK_STR(value, "10.0.0.0");
        CHECK(ami_cfg_next_pair(&cursor, &key, &value));
        CHECK_STR(key, "via"); CHECK_STR(value, "192.168.1.1");
        CHECK(ami_cfg_next_pair(&cursor, &key, &value));
        CHECK_STR(key, "metric"); CHECK_STR(value, "3");
        CHECK(!ami_cfg_next_pair(&cursor, &key, &value));
        free(buf);

        /* AmigaDOS quoting, including the '*' escape. */
        buf = dup_text("id=\"my *\"quoted*\" host\"");
        cursor = buf;
        CHECK(ami_cfg_next_pair(&cursor, &key, &value));
        CHECK_STR(key, "id");
        CHECK_STR(value, "my \"quoted\" host");
        free(buf);
    }

    {
        ULONG n = 0;

        CHECK(ami_cfg_parse_ulong("1500", &n) && n == 1500);
        CHECK(ami_cfg_parse_ulong("0x800", &n) && n == 2048);
        CHECK(ami_cfg_parse_ulong("4294967295", &n) &&
              n == (ULONG)0xFFFFFFFFUL);
        CHECK(ami_cfg_parse_ulong("0xffffffff", &n) &&
              n == (ULONG)0xFFFFFFFFUL);

        n = 77;
        CHECK(!ami_cfg_parse_ulong("4294967296", &n) && n == 77);
        CHECK(!ami_cfg_parse_ulong("4294967297", &n) && n == 77);
        CHECK(!ami_cfg_parse_ulong("0x100000000", &n) && n == 77);
        CHECK(!ami_cfg_parse_ulong("15x0", &n));
        CHECK(!ami_cfg_parse_ulong("", &n));
    }

    {
        BOOL b = FALSE;

        CHECK(ami_cfg_parse_bool("YES", &b) && b == TRUE);
        CHECK(ami_cfg_parse_bool("no", &b) && b == FALSE);
        CHECK(!ami_cfg_parse_bool("maybe", &b));
    }
}

static void test_ip(void)
{
    ULONG addr = 0;
    char  text[32];

    printf("ip addresses\n");

    CHECK(ami_config_parse_ip("192.168.1.100", &addr));
    CHECK_IP(addr, 192, 168, 1, 100);

    CHECK(ami_config_parse_ip(" 255.255.255.0 ", &addr));
    CHECK_IP(addr, 255, 255, 255, 0);

    CHECK(!ami_config_parse_ip("192.168.1", &addr));
    CHECK(!ami_config_parse_ip("192.168.1.256", &addr));
    CHECK(!ami_config_parse_ip("192.168.1.4294967296", &addr));
    CHECK(!ami_config_parse_ip("192.168.1.1.1", &addr));
    CHECK(!ami_config_parse_ip("dhcp", &addr));
    CHECK(!ami_config_parse_ip("", &addr));

    ami_config_format_ip(0xC0A80164UL, text, sizeof(text));
    CHECK_STR(text, "192.168.1.100");
    ami_config_format_ip(0, text, sizeof(text));
    CHECK_STR(text, "0.0.0.0");
    ami_config_format_ip(0xFFFFFFFFUL, text, sizeof(text));
    CHECK_STR(text, "255.255.255.255");

    /* Truncation must not overrun. */
    ami_config_format_ip(0xC0A80164UL, text, 5);
    CHECK_STR(text, "192.");

    /* /etc/networks shorthand, BSD inet_network() semantics. */
    CHECK(ami_cfg_parse_net_number("127", &addr) && addr == 127);
    CHECK(ami_cfg_parse_net_number("192.168.1", &addr) && addr == 0x00C0A801UL);
    CHECK(!ami_cfg_parse_net_number("cheese", &addr));
}


/* Four host-order ULONGs from eight groups, for the expectations below. */
#define IP6(a, b, c, d, e, f, g, h)                                          \
    { ((ULONG)(a) << 16) | (ULONG)(b), ((ULONG)(c) << 16) | (ULONG)(d),      \
      ((ULONG)(e) << 16) | (ULONG)(f), ((ULONG)(g) << 16) | (ULONG)(h) }

static int ip6_equal(const ULONG got[4], const ULONG want[4])
{
    return got[0] == want[0] && got[1] == want[1] &&
           got[2] == want[2] && got[3] == want[3];
}

static void test_ip6(void)
{
    ULONG addr[4];
    ULONG prefix;
    char  text[AMI_CFG_IP6_STRLEN];

    printf("ipv6 addresses\n");

    /* ---- the grammar, accepted ---------------------------------------- */
    {
        static const ULONG full[4] =
            IP6(0x2001, 0x0db8, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001);

        CHECK(ami_config_parse_ip6("2001:db8:0:0:0:0:0:1", addr, NULL));
        CHECK(ip6_equal(addr, full));

        CHECK(ami_config_parse_ip6("2001:0db8:0000:0000:0000:0000:0000:0001",
                                   addr, NULL));
        CHECK(ip6_equal(addr, full));

        CHECK(ami_config_parse_ip6("2001:db8::1", addr, NULL));
        CHECK(ip6_equal(addr, full));

        CHECK(ami_config_parse_ip6("2001:DB8::1", addr, NULL));
        CHECK(ip6_equal(addr, full));
    }

    {
        static const ULONG loop[4] = IP6(0, 0, 0, 0, 0, 0, 0, 1);
        static const ULONG any[4]  = IP6(0, 0, 0, 0, 0, 0, 0, 0);
        static const ULONG lead[4] = IP6(0, 0, 0, 0, 0, 0, 0, 0x1234);
        static const ULONG trail[4] =
            IP6(0xfe80, 0, 0, 0, 0, 0, 0, 0);

        CHECK(ami_config_parse_ip6("::1", addr, NULL));
        CHECK(ip6_equal(addr, loop));

        CHECK(ami_config_parse_ip6("::", addr, NULL));
        CHECK(ip6_equal(addr, any));

        CHECK(ami_config_parse_ip6("::1234", addr, NULL));
        CHECK(ip6_equal(addr, lead));

        CHECK(ami_config_parse_ip6("fe80::", addr, NULL));
        CHECK(ip6_equal(addr, trail));
    }

    /* A trailing dotted quad, which is what makes v4-mapped writable. */
    {
        static const ULONG mapped[4] =
            IP6(0, 0, 0, 0, 0, 0xffff, 0xc0a8, 0x0101);

        CHECK(ami_config_parse_ip6("::ffff:192.168.1.1", addr, NULL));
        CHECK(ip6_equal(addr, mapped));
    }

    /* A link-local address as the RAM-driver test produces it. */
    {
        static const ULONG ll[4] =
            IP6(0xfe80, 0, 0, 0, 0x0211, 0x22ff, 0xfe33, 0x4456);

        CHECK(ami_config_parse_ip6("fe80::211:22ff:fe33:4456", addr, NULL));
        CHECK(ip6_equal(addr, ll));
    }

    /* ---- the grammar, rejected ---------------------------------------- */

    CHECK(!ami_config_parse_ip6("", addr, NULL));
    CHECK(!ami_config_parse_ip6(":", addr, NULL));
    CHECK(!ami_config_parse_ip6(":::", addr, NULL));
    CHECK(!ami_config_parse_ip6("1:2:3:4:5:6:7", addr, NULL));   /* too short */
    CHECK(!ami_config_parse_ip6("1:2:3:4:5:6:7:8:9", addr, NULL)); /* too long */
    CHECK(!ami_config_parse_ip6("1::2::3", addr, NULL));         /* two runs */
    CHECK(!ami_config_parse_ip6("1:2:3:4:5:6:7::8", addr, NULL));/* empty run */
    CHECK(!ami_config_parse_ip6("12345::1", addr, NULL));        /* 5 digits */
    CHECK(!ami_config_parse_ip6("::g", addr, NULL));
    CHECK(!ami_config_parse_ip6("2001:db8::1:", addr, NULL));
    CHECK(!ami_config_parse_ip6("192.168.1.1", addr, NULL));
    CHECK(!ami_config_parse_ip6("::ffff:192.168.1.256", addr, NULL));

    /* ---- the two dialects --------------------------------------------- */

    /* inet_pton() must not accept a prefix; the config file must. */
    CHECK(!ami_config_parse_ip6("2001:db8::1/64", addr, NULL));

    prefix = 64;
    CHECK(ami_config_parse_ip6("2001:db8::1/48", addr, &prefix));
    CHECK(prefix == 48);

    prefix = 64;
    CHECK(ami_config_parse_ip6("2001:db8::1", addr, &prefix));
    CHECK(prefix == 64);                    /* untouched, so the default holds */

    prefix = 64;
    CHECK(!ami_config_parse_ip6("2001:db8::1/129", addr, &prefix));
    CHECK(!ami_config_parse_ip6("2001:db8::1/42949672960", addr, &prefix));
    CHECK(!ami_config_parse_ip6("2001:db8::1/", addr, &prefix));

    /* ---- RFC 5952 output ----------------------------------------------- */
    {
        static const ULONG cases[][4] = {
            IP6(0, 0, 0, 0, 0, 0, 0, 0),
            IP6(0, 0, 0, 0, 0, 0, 0, 1),
            IP6(0x2001, 0x0db8, 0, 0, 0, 0, 0, 1),
            IP6(0xfe80, 0, 0, 0, 0x0211, 0x22ff, 0xfe33, 0x4456),
            IP6(0x2001, 0x0db8, 0, 1, 0, 0, 0, 1),
            IP6(0x2001, 0, 0, 1, 0, 0, 0, 0),
            IP6(0, 0, 0, 0, 0, 0xffff, 0xc0a8, 0x0101),
            IP6(1, 2, 3, 4, 5, 6, 7, 8),
            /* 4.2.3: equal runs, and the FIRST one is the one elided. The
               rule most stacks get wrong, so both a middle and a leading
               tie are pinned here. */
            IP6(0x2001, 0x0db8, 0, 0, 1, 0, 0, 1),
            IP6(0, 0, 1, 0, 0, 1, 2, 3),
            /* 4.2.2 again, from the RFC's own example: a lone 16-bit zero
               stays a "0" even with no longer run anywhere to prefer. */
            IP6(0x2001, 0x0db8, 0, 1, 1, 1, 1, 1),
            /* 4.1 + 4.3 together: every group needs trimming, and every
               letter must come out lowercase. */
            IP6(0x000a, 0x00bc, 0x0def, 0xfeed, 0x0001, 0x0020, 0x0300, 0x4000),
        };
        static const char *want[] = {
            "::",
            "::1",
            "2001:db8::1",
            "fe80::211:22ff:fe33:4456",
            "2001:db8:0:1::1",
            "2001:0:0:1::",
            "::ffff:192.168.1.1",
            "1:2:3:4:5:6:7:8",
            "2001:db8::1:0:0:1",
            "::1:0:0:1:2:3",
            "2001:db8:0:1:1:1:1:1",
            "a:bc:def:feed:1:20:300:4000",
        };
        size_t i;

        for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        {
            ami_config_format_ip6(cases[i], text, sizeof(text));
            CHECK_STR(text, want[i]);
        }
    }

    /* ---- RFC 4007 11: <address>%<zone_id> ------------------------------ */
    {
        static const ULONG ll[4] = IP6(0xfe80, 0, 0, 0, 0, 0, 0, 1);
        char  zone[AMI_CFG_IP6_ZONE_LEN];
        char  wide[AMI_CFG_IP6_ZONE_STRLEN];
        ULONG got[4];

        /* A name and a numeric index are both accepted; RFC 4007 asks for
           at least the numbers and allows the names. */
        CHECK(ami_config_parse_ip6_zone("fe80::1%eth0", got, NULL,
                                        zone, sizeof(zone)));
        CHECK(ip6_equal(got, ll));
        CHECK_STR(zone, "eth0");

        CHECK(ami_config_parse_ip6_zone("fe80::1%1", got, NULL,
                                        zone, sizeof(zone)));
        CHECK_STR(zone, "1");

        /* No zone leaves the buffer empty rather than stale. */
        CHECK(ami_config_parse_ip6_zone("fe80::1", got, NULL,
                                        zone, sizeof(zone)));
        CHECK_STR(zone, "");

        /* A zone in front of a prefix, which is where it goes. */
        {
            ULONG pfx = 0;

            CHECK(ami_config_parse_ip6_zone("fe80::1%eth0/64", got, &pfx,
                                            zone, sizeof(zone)));
            CHECK(pfx == 64);
            CHECK_STR(zone, "eth0");
        }

        CHECK(!ami_config_parse_ip6_zone("fe80::1%", got, NULL,
                                         zone, sizeof(zone)));
        CHECK(!ami_config_parse_ip6_zone("fe80::1%averylonginterfacename",
                                         got, NULL, zone, sizeof(zone)));

        /* The plain parser refuses a zone rather than dropping it. */
        CHECK(!ami_config_parse_ip6("fe80::1%eth0", got, NULL));

        /* Formatting, and the round trip through it. */
        ami_config_format_ip6_zone(ll, "eth0", wide, sizeof(wide));
        CHECK_STR(wide, "fe80::1%eth0");

        ami_config_format_ip6_zone(ll, "", wide, sizeof(wide));
        CHECK_STR(wide, "fe80::1");

        CHECK(ami_config_parse_ip6_zone(wide, got, NULL, zone, sizeof(zone)));
        CHECK(ip6_equal(got, ll));

        ami_config_format_ip6_zone(ll, "eth0", text, sizeof(text));
        CHECK_STR(text, "");
    }

    /* Round trip: everything the formatter writes, the parser must read. */
    {
        static const ULONG probe[4] =
            IP6(0x2001, 0x0db8, 0, 1, 0, 0, 0, 1);
        ULONG back[4];

        ami_config_format_ip6(probe, text, sizeof(text));
        CHECK(ami_config_parse_ip6(text, back, NULL));
        CHECK(ip6_equal(back, probe));
    }

    ami_config_format_ip6(addr, text, 8);
    CHECK_STR(text, "");
}

/* ---- CONFIGURE6 / ADDRESS6 / GATEWAY6 in an interface file ------------- */
/* Still IPv6-only: config_parse.c reads these keys in an AMINETXDUO_IPV6
   build alone. */

#ifdef AMINETXDUO_IPV6

static const char dual_stack_net[] =
    "device     = a2065.device\n"
    "unit       = 0\n"
    "configure  = dhcp\n"
    "configure6 = static\n"
    "address6   = 2001:db8::10/48\n"
    "gateway6   = fe80::1\n";

static void test_interface_ipv6(void)
{
    AmiIfConfig cfg;
    char        buf[512];

    printf("interface file: dual stack\n");

    strcpy(buf, dual_stack_net);
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.iptype == AMI_IPTYPE_DHCP);
    CHECK(cfg.ip6type == AMI_IP6TYPE_STATIC);
    CHECK(cfg.address6_count == 1);
    CHECK(cfg.address6[0].prefix == 48);
    CHECK(cfg.address6[0].addr[0] == 0x20010db8UL &&
          cfg.address6[0].addr[3] == 0x10UL);
    CHECK(cfg.have_gateway6);
    CHECK(cfg.gateway6[0] == 0xfe800000UL && cfg.gateway6[3] == 1UL);

    /* No IPv6 keyword at all: AUTO, no address, no router. */
    printf("interface file: ipv6 defaults\n");
    strcpy(buf, "device=a2065.device\nconfigure=dhcp\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_AUTO);
    CHECK(cfg.address6_count == 0);
    CHECK(!cfg.have_gateway6);

    /* TWO ADDRESS6 lines: a ULA and a global at once, which is what RFC 6724
       rule 6 needs a node to hold and what one line per interface could not
       describe.  Order is the file order. */
    printf("interface file: two address6 lines\n");
    strcpy(buf, "device=a2065.device\nconfigure6=static\n"
                "address6=fd00:6724:1::10/64\n"
                "address6=2001:db8:6724:1::10/64\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_STATIC);
    CHECK(cfg.address6_count == 2);
    CHECK(cfg.address6[0].addr[0] == 0xfd006724UL);
    CHECK(cfg.address6[0].prefix == 64);
    CHECK(cfg.address6[1].addr[0] == 0x20010db8UL);
    CHECK(cfg.address6[1].prefix == 64);

    /* Each line carries its own prefix length. */
    printf("interface file: per-line prefix length\n");
    strcpy(buf, "device=a2065.device\n"
                "address6=2001:0:1::10/16\n"
                "address6=2001:db8::10\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.address6_count == 2);
    CHECK(cfg.address6[0].prefix == 16);
    CHECK(cfg.address6[1].prefix == 64);

    /* One line past the ceiling is refused and the ones already taken stay:
       an interface with its first address is more use than one with none. */
    printf("interface file: address6 ceiling\n");
    strcpy(buf, "device=a2065.device\n"
                "address6=2001:db8::1\n"
                "address6=2001:db8::2\n"
                "address6=2001:db8::3\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.address6_count == AMI_CFG_MAX_ADDRESS6);
    CHECK(cfg.address6[0].addr[3] == 1UL);
    CHECK(cfg.address6[1].addr[3] == 2UL);

    /* ADDRESS6 with no CONFIGURE6 implies STATIC, as ADDRESS implies a
       static IPv4 interface. */
    printf("interface file: address6 implies static\n");
    strcpy(buf, "device=a2065.device\naddress6=2001:db8::5\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_STATIC);
    CHECK(cfg.address6_count == 1);
    CHECK(cfg.address6[0].prefix == 64);

    /* CONFIGURE6 wins over the implication, whichever order they appear in. */
    printf("interface file: configure6 wins\n");
    strcpy(buf, "device=a2065.device\nconfigure6=auto\naddress6=2001:db8::5\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_AUTO);

    /* OFF means off. */
    printf("interface file: configure6 off\n");
    strcpy(buf, "device=a2065.device\nconfigure6=off\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_OFF);

    /* STATIC with no address degrades to link-local rather than failing. */
    printf("interface file: static6 with no address6\n");
    strcpy(buf, "device=a2065.device\nconfigure6=static\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_LINKLOCAL);

    /* An overflowing prefix is a bad ADDRESS6, not /0 after ULONG wrap. */
    strcpy(buf, "device=a2065.device\nconfigure6=static\n"
                "address6=2001:db8::5/42949672960\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_LINKLOCAL);
    CHECK(cfg.address6_count == 0);
}

#endif /* AMINETXDUO_IPV6 */

/* A real Roadshow interface file (BlitterStudio/zz9000-drivers). */
static const char zz9000_net[] =
    "# $VER: ZZ9000Net 1.0 (30.07.2019)\n"
    "device=ZZ9000Net.device\n"
    "unit=0\n"
    "#address=192.168.1.199\n"
    "#netmask=255.255.255.0\n"
    "configure=dhcp\n"
    "#configure=auto\n"
    "debug=yes\n"
    "#iprequests=32\n"
    "filter=ipandarp\n"
    "requiresinitdelay=no\n";

static void test_interface_roadshow(void)
{
    AmiIfConfig iface;
    char       *buf = dup_text(zz9000_net);

    printf("interface: Roadshow DHCP\n");

    CHECK(ami_cfg_parse_interface("ZZ9000Net", buf, &iface) == AMI_CFG_OK);
    CHECK_STR(iface.name, "ZZ9000Net");
    CHECK_STR(iface.device, "ZZ9000Net.device");
    CHECK(iface.unit == 0);
    CHECK(iface.iptype == AMI_IPTYPE_DHCP);
    CHECK(iface.address == 0);
    CHECK(iface.up == TRUE);
    CHECK(iface.configured == TRUE);
    free(buf);
}

static void test_interface_static(void)
{
    AmiIfConfig iface;
    char *buf = dup_text(
        "; DEVS:NetInterfaces/eth0\r\n"
        "\r\n"
        "\tDevice = ariadne.device   \r\n"
        "UNIT\t0\r\n"
        "ADDRESS=192.168.1.100\r\n"
        "netmask = 255.255.255.0\r\n"
        "GATEWAY=192.168.1.1\r\n"
        "mtu=1500\r\n"
        "iptype=2048\r\n"
        "hardwareaddress=00:60:30:00:11:22\r\n"
        "id = a1200\r\n"
        "state=down\r\n"
        "# end\r\n");

    printf("interface: static, messy formatting\n");

    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK_STR(iface.name, "eth0");
    CHECK_STR(iface.id, "a1200");
    CHECK_STR(iface.device, "ariadne.device");
    CHECK(iface.unit == 0);
    CHECK(iface.iptype == AMI_IPTYPE_STATIC);
    CHECK_IP(iface.address, 192, 168, 1, 100);
    CHECK_IP(iface.netmask, 255, 255, 255, 0);
    CHECK_IP(iface.gateway, 192, 168, 1, 1);
    CHECK(iface.mtu == 1500);
    CHECK(iface.up == FALSE);
    free(buf);
}

static void test_interface_amitcp_flavour(void)
{
    AmiIfConfig iface;
    char *buf = dup_text(
        "DEVICE=mister_eth.device\n"
        "UNIT=1\n"
        "IPTYPE=DHCP\n"
        "MTU=1500\n");

    printf("interface: AmiTCP-flavoured IPTYPE\n");

    CHECK(ami_cfg_parse_interface("mister0", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.iptype == AMI_IPTYPE_DHCP);
    CHECK(iface.unit == 1);
    CHECK(iface.mtu == 1500);
    free(buf);

    buf = dup_text("device=x.device\naddress=dhcp\nnetmask=dhcp\n");
    CHECK(ami_cfg_parse_interface("x", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.iptype == AMI_IPTYPE_DHCP);
    free(buf);

    buf = dup_text("device=x.device\nconfigure=fastauto\n");
    CHECK(ami_cfg_parse_interface("x", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.iptype == AMI_IPTYPE_LINKLOCAL);
    free(buf);
}

static void test_interface_errors(void)
{
    AmiIfConfig iface;
    char       *buf;

    printf("interface: error handling\n");

    /* DEVICE is the only mandatory keyword. */
    buf = dup_text("unit=0\naddress=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("bad", buf, &iface) == AMI_CFG_ERR_SYNTAX);
    free(buf);

    /* Junk values are warned about, not fatal, and never overrun a buffer. */
    buf = dup_text(
        "device=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "address=999.1.2.3\n"
        "mtu=big\n"
        "wibble=1\n");
    CHECK(ami_cfg_parse_interface("longnamethatisdefinitelytoolong", buf, &iface)
          == AMI_CFG_OK);
    CHECK(strlen(iface.device) == AMI_CFG_PATH_LEN - 1);
    /* Roadshow truncates the interface name to 15 characters. */
    CHECK_STR(iface.name, "longnamethatisd");
    CHECK(iface.address == 0);
    CHECK(iface.mtu == 0);
    free(buf);

    /* An empty file is not a crash. */
    buf = dup_text("");
    CHECK(ami_cfg_parse_interface("empty", buf, &iface) == AMI_CFG_ERR_SYNTAX);
    free(buf);
}

/* ---------------------------------------------------------- the reporter */

#define MAX_SEEN    8

static struct
{
    ULONG line;
    UWORD severity;
    char  text[160];
    char  hint[240];
} seen[MAX_SEEN];

static UWORD seen_count;

static VOID collect(const AmiCfgProblem *problem, APTR user)
{
    (void)user;

    if (seen_count >= MAX_SEEN)
        return;

    seen[seen_count].line     = problem->line;
    seen[seen_count].severity = problem->severity;
    strncpy(seen[seen_count].text, problem->text,
            sizeof(seen[0].text) - 1);
    seen[seen_count].hint[0] = '\0';
    {
        const char *advice = (problem->hint_text != NULL)
                                 ? problem->hint_text
                                 : ami_cfg_advice(problem->hint);

        if (advice != NULL)
            strncpy(seen[seen_count].hint, advice, sizeof(seen[0].hint) - 1);
    }

    if (stub_verbose)
        printf("    line %lu: %s\n", (unsigned long)problem->line,
               problem->text);

    seen_count++;
}

/* Does any reported problem mention `needle`? */
static int seen_mentions(const char *needle)
{
    UWORD i;

    for (i = 0; i < seen_count; i++)
    {
        if (strstr(seen[i].text, needle) != NULL ||
            strstr(seen[i].hint, needle) != NULL)
            return 1;
    }

    return 0;
}

static void test_problem_reporter(void)
{
    AmiIfConfig iface;
    char       *buf;

    printf("interface: problems, with line numbers\n");

    seen_count = 0;
    ami_config_set_reporter(collect, NULL);
    ami_cfg_problem_file("DEVS:NetInterfaces/eth0");

    buf = dup_text("# a comment\n"          /* line 1 */
                   "devcie = a2065.device\n"/* line 2: typo   */
                   "unit = zero\n"          /* line 3: not a number */
                   "\n"                     /* line 4 */
                   "address = 10.0.0.300\n" /* line 5: not an address */
                   "configure = dhcpp\n");  /* line 6: not a mode */

    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_ERR_SYNTAX);
    free(buf);

    ami_config_set_reporter(NULL, NULL);

    /* One per bad line, plus the "no DEVICE at all" verdict for the file. */
    CHECK(seen_count == 5);

    CHECK(seen[0].line == 2);
    CHECK(seen_mentions("devcie"));
    /* The suggestion is the whole point of the typo case. */
    CHECK(seen_mentions("DEVICE"));

    CHECK(seen[1].line == 3);
    CHECK(seen_mentions("UNIT"));

    CHECK(seen[2].line == 5);
    CHECK(seen_mentions("ADDRESS"));

    CHECK(seen[3].line == 6);
    CHECK(seen_mentions("CONFIGURE"));

    /* A verdict about the file as a whole carries line 0. */
    CHECK(seen[4].line == 0);
    CHECK(seen[4].severity == AMI_CFG_PROBLEM_ERROR);

    /* With no reporter installed nothing is collected and nothing crashes. */
    seen_count = 0;
    buf = dup_text("wibble=1\ndevice=a2065.device\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    free(buf);
    CHECK(seen_count == 0);
}

static void test_inert_keywords_are_notes(void)
{
    AmiIfConfig iface;
    char       *buf;
    UWORD       i;

    printf("interface: a keyword ignored by design is a note, not a problem\n");

    seen_count = 0;
    ami_config_set_reporter(collect, NULL);
    ami_cfg_problem_file("DEVS:NetInterfaces/genet");

    /* The user's own file, and there is nothing wrong with it. */
    buf = dup_text("device = a2065.device\n"    /* line 1 */
                   "unit = 0\n"                 /* line 2 */
                   "configure = dhcp\n"         /* line 3 */
                   "iprequests = 32\n"          /* line 4 */
                   "writerequests = 32\n"       /* line 5 */
                   "copymode = 1\n"             /* line 6 */
                   "multicast = yes\n");        /* line 7 */

    CHECK(ami_cfg_parse_interface("genet", buf, &iface) == AMI_CFG_OK);
    free(buf);
    ami_config_set_reporter(NULL, NULL);

    CHECK(seen_count == 4);
    CHECK(seen_mentions("iprequests"));
    CHECK(seen_mentions("writerequests"));
    CHECK(seen_mentions("copymode"));
    CHECK(seen_mentions("multicast"));
    CHECK(seen[0].line == 4);
    CHECK(seen[3].line == 7);

    /* And not one of them may reach an ordinary command. */
    for (i = 0; i < seen_count; i++)
        CHECK(seen[i].severity == AMI_CFG_PROBLEM_NOTE);

    seen_count = 0;
    ami_config_set_reporter(collect, NULL);
    ami_cfg_problem_file("DEVS:NetInterfaces/broken");

    buf = dup_text("device = a2065.device\n"    /* line 1 */
                   "multicast = yes\n"          /* line 2: inert, a note */
                   "configure = static\n"       /* line 3 */
                   "address = 10.0.0.300\n"     /* line 4: not an address */
                   "devcie = a2065.device\n");  /* line 5: a typo */

    CHECK(ami_cfg_parse_interface("broken", buf, &iface) == AMI_CFG_OK);
    free(buf);
    ami_config_set_reporter(NULL, NULL);

    CHECK(seen_count == 4);

    CHECK(seen[0].line == 2);
    CHECK(seen[0].severity == AMI_CFG_PROBLEM_NOTE);

    CHECK(seen[1].line == 4);
    CHECK(seen[1].severity == AMI_CFG_PROBLEM_ERROR);
    CHECK(seen_mentions("ADDRESS"));

    CHECK(seen[2].line == 5);
    CHECK(seen[2].severity == AMI_CFG_PROBLEM_WARN);
    CHECK(seen_mentions("devcie"));

    CHECK(seen[3].line == 0);
    CHECK(seen[3].severity == AMI_CFG_PROBLEM_ERROR);

    /* A missing DEVICE line is the whole-file verdict, and stays an ERROR. */
    seen_count = 0;
    ami_config_set_reporter(collect, NULL);
    ami_cfg_problem_file("DEVS:NetInterfaces/nodevice");

    buf = dup_text("unit = 0\ncopymode = 1\nconfigure = dhcp\n");
    CHECK(ami_cfg_parse_interface("nodevice", buf, &iface) != AMI_CFG_OK);
    free(buf);
    ami_config_set_reporter(NULL, NULL);

    CHECK(seen_count == 2);
    CHECK(seen[0].severity == AMI_CFG_PROBLEM_NOTE);      /* copymode */
    CHECK(seen[1].line == 0);
    CHECK(seen[1].severity == AMI_CFG_PROBLEM_ERROR);     /* no DEVICE */
}

/* CARD= names come from include/aminetxduo/anxnet.h; an unknown name must
   refuse the interface, not fall back to UNIT. */

static void test_interface_ipv6_only(void)
{
    AmiIfConfig iface;
    char       *buf;

    printf("interface: IPv6-only\n");

    /* CONFIGURE=NONE is "no IPv4", not "static with no address". */
    buf = dup_text("device=a2065.device\nconfigure=none\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.iptype == AMI_IPTYPE_NONE);
    CHECK(iface.configured == TRUE);
    CHECK(!ami_config_iface_wants_ipv4(&iface));
    free(buf);

    /* And its three spellings. */
    buf = dup_text("device=a2065.device\nconfigure=off\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.iptype == AMI_IPTYPE_NONE);
    free(buf);

    /* A static IPv4 address means IPv4 is expected; DHCP and link-local too. */
    buf = dup_text("device=a2065.device\naddress=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(ami_config_iface_wants_ipv4(&iface));
    free(buf);

    buf = dup_text("device=a2065.device\nconfigure=dhcp\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(ami_config_iface_wants_ipv4(&iface));
    free(buf);

    buf = dup_text("device=a2065.device\nconfigure=linklocal\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(ami_config_iface_wants_ipv4(&iface));
    free(buf);

    /* STATIC with no ADDRESS expects nothing: there is no source left. */
    buf = dup_text("device=a2065.device\nconfigure6=auto\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.iptype == AMI_IPTYPE_STATIC);
    CHECK(!ami_config_iface_wants_ipv4(&iface));
    free(buf);
}

static void test_ipv6_only_no_error(void)
{
    AmiIfConfig iface;
    char       *buf;

    printf("interface: IPv6-only raises no problem\n");

    ami_config_set_reporter(collect, NULL);
    ami_cfg_problem_file("DEVS:NetInterfaces/eth0");

    /* An explicit CONFIGURE6 is a statement that IPv6 carries the interface. */
    seen_count = 0;
    buf = dup_text("device=a2065.device\nconfigure6=auto\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 0);
    free(buf);

    /* So is an ADDRESS6. */
    seen_count = 0;
    buf = dup_text("device=a2065.device\naddress6=2001:db8::10/64\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 0);
    CHECK(iface.ip6type == AMI_IP6TYPE_STATIC);
    free(buf);

    /* So is a GATEWAY6 on its own. */
    seen_count = 0;
    buf = dup_text("device=a2065.device\ngateway6=fe80::1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 0);
    free(buf);

    /* And so is CONFIGURE=NONE, which says which family carries it. */
    seen_count = 0;
    buf = dup_text("device=a2065.device\nconfigure=none\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 0);
    free(buf);

    /* CONFIGURE6=DHCP is the new mode and is spelled like CONFIGURE=DHCP. */
    seen_count = 0;
    buf = dup_text("device=a2065.device\nconfigure6=dhcp\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 0);
    CHECK(iface.ip6type == AMI_IP6TYPE_DHCP);
    CHECK(ami_config_iface_wants_ipv6(&iface));
    free(buf);

    seen_count = 0;
    buf = dup_text("device=a2065.device\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 1);
    CHECK(seen[0].severity == AMI_CFG_PROBLEM_ERROR);
    CHECK(seen_mentions("no address"));
    CHECK(seen_mentions("CONFIGURE6"));
    free(buf);

    /* And so must one that switches both families off. */
    seen_count = 0;
    buf = dup_text("device=a2065.device\nconfigure=none\nconfigure6=off\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(seen_count == 1);
    CHECK(seen[0].severity == AMI_CFG_PROBLEM_ERROR);
    CHECK(!ami_config_iface_wants_ipv4(&iface));
    CHECK(!ami_config_iface_wants_ipv6(&iface));
    free(buf);

    ami_config_set_reporter(NULL, NULL);
}

static void test_interface_card(void)
{
    AmiIfConfig iface;
    char       *buf;

    printf("interface: CARD\n");

    buf = dup_text("device=anxnet.device\nunit=0\ncard=xsurf100\n"
                   "address=192.168.1.10\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK_STR(iface.card, "xsurf100");
    free(buf);

    /* Case-insensitive like every other keyword's value that names a thing. */
    buf = dup_text("device=anxnet.device\nCARD = Ariadne2\naddress=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK_STR(iface.card, "Ariadne2");
    free(buf);

    /* No CARD at all is the ordinary case and leaves the field empty. */
    buf = dup_text("device=a2065.device\naddress=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK(iface.card[0] == '\0');
    free(buf);

    /* A name no card has, and an empty one, refuse the interface. */
    buf = dup_text("device=anxnet.device\ncard=nonsense\naddress=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_ERR_SYNTAX);
    free(buf);

    buf = dup_text("device=anxnet.device\ncard=\naddress=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_ERR_SYNTAX);
    free(buf);

    /* And the report names the keyword, the value and the choices. */
    seen_count = 0;
    ami_config_set_reporter(collect, NULL);
    buf = dup_text("device=anxnet.device\n"
                   "card=xsurf1000\n"       /* line 2 */
                   "address=10.0.0.1\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_ERR_SYNTAX);
    free(buf);
    ami_config_set_reporter(NULL, NULL);

    CHECK(seen_count == 1);
    CHECK(seen[0].line == 2);
    CHECK(seen[0].severity == AMI_CFG_PROBLEM_ERROR);
    CHECK(seen_mentions("CARD"));
    CHECK(seen_mentions("xsurf1000"));
    CHECK(seen_mentions("XSURF100"));
    CHECK(seen_mentions("ARIADNE2"));
}

/* Host-name precedence, strongest first: name_resolution, DHCP option 12,
   ENV:HOSTNAME, an interface file's ID=. */
static void test_hostname_syntax(void)
{
    printf("host name: RFC 1123 syntax\n");

    CHECK(ami_config_hostname_valid("a1200"));
    CHECK(ami_config_hostname_valid("a1200.intra.example.de"));
    CHECK(ami_config_hostname_valid("3com"));          /* RFC 1123 leading digit */
    CHECK(ami_config_hostname_valid("my-amiga"));

    CHECK(!ami_config_hostname_valid(NULL));
    CHECK(!ami_config_hostname_valid(""));
    CHECK(!ami_config_hostname_valid("my amiga"));     /* space   */
    CHECK(!ami_config_hostname_valid("my_amiga"));     /* underscore */
    CHECK(!ami_config_hostname_valid("-amiga"));
    CHECK(!ami_config_hostname_valid("amiga-"));
    CHECK(!ami_config_hostname_valid(".amiga"));       /* empty first label */
    CHECK(!ami_config_hostname_valid("amiga."));       /* empty last label  */
    CHECK(!ami_config_hostname_valid("a..b"));

    /* Longer than the store, so there would be nothing to keep it in. */
    {
        char big[AMI_CFG_NAME_LEN + 8];

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK(!ami_config_hostname_valid(big));
    }
}

static void test_hostname_precedence(void)
{
    AmiConfig cfg;

    /* cfg_reset() frees the previous list before it builds the next,
       so the struct has to be valid before the first one. */
    memset(&cfg, 0, sizeof(cfg));

    printf("host name: which source wins\n");

    cfg_reset(&cfg, 1U);
    strcpy(cfg.interfaces[0].id, "a1200");
    ami_cfg_hostname_from_files(&cfg, NULL);
    CHECK_STR(cfg.hostname, "a1200");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_INTERFACE);

    cfg_reset(&cfg, 1U);
    strcpy(cfg.interfaces[0].id, "a1200");
    {
        char *env = dup_text("a3000\n");

        ami_cfg_hostname_from_files(&cfg, env);
        free(env);
    }
    CHECK_STR(cfg.hostname, "a3000");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_ENV);

    cfg_reset(&cfg, 1U);
    strcpy(cfg.interfaces[0].id, "Ethernet");
    {
        char *env = dup_text("myamiga\n");

        ami_cfg_hostname_from_files(&cfg, env);
        free(env);
    }
    CHECK_STR(cfg.hostname, "myamiga");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_ENV);

    /* No ID=: the environment still answers, as it always did. */
    cfg_reset(&cfg, 1U);
    {
        char *env = dup_text("a3000\r\nignored second line\n");

        ami_cfg_hostname_from_files(&cfg, env);
        free(env);
    }
    CHECK_STR(cfg.hostname, "a3000");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_ENV);

    /* An ID that is not a host name falls through rather than being adopted. */
    cfg_reset(&cfg, 1U);
    strcpy(cfg.interfaces[0].id, "Ariadne in the study");
    {
        char *env = dup_text("a3000\n");

        ami_cfg_hostname_from_files(&cfg, env);
        free(env);
    }
    CHECK_STR(cfg.hostname, "a3000");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_ENV);

    /* Two interfaces, the first with an unusable ID: the second answers. */
    cfg_reset(&cfg, 2U);
    strcpy(cfg.interfaces[0].id, "the *good* one");
    strcpy(cfg.interfaces[1].id, "a4000");
    ami_cfg_hostname_from_files(&cfg, NULL);
    CHECK_STR(cfg.hostname, "a4000");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_INTERFACE);

    /* name_resolution has already run and outranks everything below it. */
    cfg_reset(&cfg, 1U);
    strcpy(cfg.interfaces[0].id, "a1200");
    strcpy(cfg.hostname, "workshop");
    cfg.hostname_source = AMI_HOSTNAME_NAMERES;
    {
        char *env = dup_text("a3000\n");

        ami_cfg_hostname_from_files(&cfg, env);
        free(env);
    }
    CHECK_STR(cfg.hostname, "workshop");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_NAMERES);

    /* Nothing named it at all. */
    cfg_reset(&cfg, 0U);
    ami_cfg_hostname_from_files(&cfg, NULL);
    CHECK_STR(cfg.hostname, "");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_NONE);
    ami_config_free(&cfg);
}

static void test_hostname_from_hwaddr(void)
{
    static const UBYTE a2065[6] = { 0x00, 0x80, 0x10, 0x49, 0x00, 0x07 };
    static const UBYTE tail0[6] = { 0x00, 0x80, 0x10, 0x00, 0x00, 0x00 };
    static const UBYTE hexy[6]  = { 0x02, 0x11, 0x22, 0xAB, 0xCD, 0xEF };
    static const UBYTE none[6]  = { 0, 0, 0, 0, 0, 0 };
    char      out[AMI_CFG_NAME_LEN];
    AmiConfig cfg;

    /* cfg_reset() frees the previous list before it builds the next,
       so the struct has to be valid before the first one. */
    memset(&cfg, 0, sizeof(cfg));
    UWORD     rank;

    printf("host name: derived from the hardware address\n");

    memset(out, '?', sizeof(out));
    CHECK(ami_config_hostname_from_hwaddr(a2065, sizeof(a2065), out,
                                          sizeof(out)));
    CHECK_STR(out, "amiga-490007");
    CHECK(ami_config_hostname_valid(out));

    /* Lower case, and both nibbles of every octet. Upper case would claim the
       same mDNS name (RFC 6762 16) and read back as a different string. */
    CHECK(ami_config_hostname_from_hwaddr(hexy, sizeof(hexy), out,
                                          sizeof(out)));
    CHECK_STR(out, "amiga-abcdef");
    CHECK(ami_config_hostname_valid(out));

    /* Three zero octets are still three octets: this card has an address. */
    CHECK(ami_config_hostname_from_hwaddr(tail0, sizeof(tail0), out,
                                          sizeof(out)));
    CHECK_STR(out, "amiga-000000");

    /* The same card twice: the name does not move between boots. */
    {
        char again[AMI_CFG_NAME_LEN];

        CHECK(ami_config_hostname_from_hwaddr(a2065, sizeof(a2065), again,
                                              sizeof(again)));
        CHECK_STR(again, "amiga-490007");
    }

    /* Two cards, two names; the whole point of the change. */
    {
        static const UBYTE other[6] = { 0x00, 0x80, 0x10, 0x49, 0x00, 0x08 };
        char               b[AMI_CFG_NAME_LEN];

        CHECK(ami_config_hostname_from_hwaddr(a2065, sizeof(a2065), out,
                                              sizeof(out)));
        CHECK(ami_config_hostname_from_hwaddr(other, sizeof(other), b,
                                              sizeof(b)));
        CHECK(strcmp(out, b) != 0);
    }

    out[0] = '\0';
    CHECK(!ami_config_hostname_from_hwaddr(none, sizeof(none), out,
                                           sizeof(out)));
    CHECK_STR(out, "");
    CHECK(!ami_config_hostname_from_hwaddr(NULL, 6, out, sizeof(out)));
    CHECK(!ami_config_hostname_from_hwaddr(a2065, 2, out, sizeof(out)));
    CHECK(!ami_config_hostname_from_hwaddr(a2065, sizeof(a2065), NULL,
                                           sizeof(out)));
    CHECK(!ami_config_hostname_from_hwaddr(a2065, sizeof(a2065), out, 12));
    CHECK(ami_config_hostname_from_hwaddr(a2065, sizeof(a2065), out, 13));

    for (rank = (UWORD)AMI_HOSTNAME_INTERFACE;
         rank <= (UWORD)AMI_HOSTNAME_NAMERES; rank++)
    {
        cfg_reset(&cfg, 0U);

        CHECK(ami_config_hostname_from_hwaddr(a2065, sizeof(a2065),
                                              cfg.hostname,
                                              sizeof(cfg.hostname)));
        CHECK(cfg.hostname_source == AMI_HOSTNAME_NONE);

        CHECK(ami_config_hostname_offer(&cfg, rank, "workshop"));
        CHECK_STR(cfg.hostname, "workshop");
        CHECK(cfg.hostname_source == rank);
    }

    cfg_reset(&cfg, 1U);
    strcpy(cfg.interfaces[0].id, "a1200");
    ami_cfg_hostname_from_files(&cfg, NULL);
    CHECK_STR(cfg.hostname, "a1200");
    CHECK(cfg.hostname[0] != '\0');     /* the test the stack makes */
    ami_config_free(&cfg);
}

static void test_hostname_offer(void)
{
    AmiConfig cfg;

    /* cfg_reset() frees the previous list before it builds the next,
       so the struct has to be valid before the first one. */
    memset(&cfg, 0, sizeof(cfg));

    printf("host name: offers and ranks\n");

    CHECK(AMI_HOSTNAME_NONE      < AMI_HOSTNAME_INTERFACE);
    CHECK(AMI_HOSTNAME_INTERFACE < AMI_HOSTNAME_ENV);
    CHECK(AMI_HOSTNAME_ENV       < AMI_HOSTNAME_DHCP);
    CHECK(AMI_HOSTNAME_DHCP      < AMI_HOSTNAME_NAMERES);

    cfg_reset(&cfg, 0U);
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_INTERFACE, "a1200"));
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_ENV, "a3000"));
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_DHCP, "leased"));
    CHECK_STR(cfg.hostname, "leased");
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_ENV, "a3000"));
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_INTERFACE, "a1200"));
    CHECK_STR(cfg.hostname, "leased");

    /* A renewal from the same source replaces the name it set. */
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_DHCP, "released"));
    CHECK_STR(cfg.hostname, "released");

    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_NAMERES, "workshop"));
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_DHCP, "leased"));
    CHECK_STR(cfg.hostname, "workshop");

    /* Off the network, so it is held to the syntax and the old name stands. */
    cfg_reset(&cfg, 0U);
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_ENV, "a3000"));
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_DHCP, "not a name"));
    CHECK_STR(cfg.hostname, "a3000");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_ENV);

    cfg_reset(&cfg, 0U);
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_ENV, "my_amiga"));
    CHECK_STR(cfg.hostname, "my_amiga");
    CHECK(ami_config_hostname_offer(&cfg, AMI_HOSTNAME_NAMERES, "my_amiga"));

    /* Refusals that must not change anything. */
    cfg_reset(&cfg, 0U);
    CHECK(!ami_config_hostname_offer(NULL, AMI_HOSTNAME_ENV, "a1200"));
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_ENV, NULL));
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_ENV, ""));
    CHECK(!ami_config_hostname_offer(&cfg, AMI_HOSTNAME_NONE, "a1200"));
    CHECK_STR(cfg.hostname, "");
    CHECK(cfg.hostname_source == AMI_HOSTNAME_NONE);

    /* Every source names itself; AMI_HOSTNAME_NONE is not a source. */
    CHECK_STR(ami_config_hostname_source_text(AMI_HOSTNAME_NAMERES),
              "name_resolution");
    CHECK_STR(ami_config_hostname_source_text(AMI_HOSTNAME_DHCP), "DHCP");
    CHECK_STR(ami_config_hostname_source_text(AMI_HOSTNAME_INTERFACE),
              "interface ID");
    CHECK_STR(ami_config_hostname_source_text(AMI_HOSTNAME_ENV),
              "ENV:HOSTNAME");
    CHECK(ami_config_hostname_source_text(AMI_HOSTNAME_NONE) == NULL);
    ami_config_free(&cfg);
}

static void test_resolver(void)
{
    AmiResolverConfig res;
    char              hostname[AMI_CFG_NAME_LEN];
    char             *buf;

    printf("name_resolution\n");

    memset(&res, 0, sizeof(res));
    hostname[0] = '\0';

    buf = dup_text(
        "# DEVS:Internet/name_resolution\n"
        "nameserver 8.8.8.8\n"
        "nameserver\t8.8.4.4\r\n"
        "domain local\n"
        "search local example.com  test.invalid\n"
        "prefer static\n");
    ami_cfg_parse_resolver(buf, &res, hostname, sizeof(hostname));
    free(buf);

    CHECK(res.nameserver_count == 2);
    CHECK_IP(res.nameserver[0], 8, 8, 8, 8);
    CHECK_IP(res.nameserver[1], 8, 8, 4, 4);
    CHECK(res.nameserver_use[0] == -1);
    CHECK(res.nameserver_use[1] == -1);
    CHECK_STR(res.domain, "local");
    CHECK(res.search_count == 3);
    CHECK_STR(res.search[0], "local");
    CHECK_STR(res.search[1], "example.com");
    CHECK_STR(res.search[2], "test.invalid");

    /* '=' form, and the AmiTCP netdb-myhost shape in the same routine. */
    memset(&res, 0, sizeof(res));
    hostname[0] = '\0';

    buf = dup_text(
        "; AmiTCP:db/netdb-myhost\n"
        "HOST 127.0.0.1 localhost\n"
        "HOST 192.168.1.42 amiga1200 a1200\n"
        "NAMESERVER=192.168.1.1\n"
        "DOMAIN=home.lan\n");
    ami_cfg_parse_resolver(buf, &res, hostname, sizeof(hostname));
    free(buf);

    CHECK(res.nameserver_count == 1);
    CHECK_IP(res.nameserver[0], 192, 168, 1, 1);
    CHECK_STR(res.domain, "home.lan");
    CHECK_STR(hostname, "amiga1200");

    /* More name servers than we have slots for: keep the first ones, no overrun. */
    memset(&res, 0, sizeof(res));
    buf = dup_text(
        "nameserver 1.1.1.1\nnameserver 2.2.2.2\nnameserver 3.3.3.3\n"
        "nameserver 4.4.4.4\nnameserver 5.5.5.5\nnameserver 6.6.6.6\n"
        "search a b c d e f g h\n");
    ami_cfg_parse_resolver(buf, &res, NULL, 0);
    free(buf);

    CHECK(res.nameserver_count == AMI_CFG_MAX_NAMESERVERS);
    CHECK_IP(res.nameserver[AMI_CFG_MAX_NAMESERVERS - 1], 4, 4, 4, 4);
    CHECK(res.search_count == AMI_CFG_MAX_SEARCH);

    {
        char line[AMI_CFG_DOMAIN_LEN + 16];
        char expect[201];
        int  i;

        for (i = 0; i < 200; i++)
            expect[i] = (i % 10 == 9) ? '.' : 'a';
        expect[199] = 'z';
        expect[200] = '\0';

        snprintf(line, sizeof(line), "domain %s\n", expect);

        memset(&res, 0, sizeof(res));
        buf = dup_text(line);
        ami_cfg_parse_resolver(buf, &res, NULL, 0);
        free(buf);

        CHECK_STR(res.domain, expect);
    }
}

/* Clears the array first, so an assertion about entry N of a shorter list
   reports "(null)" rather than reading whatever was in the slot. */
static UWORD search_of(const AmiResolverConfig *res, const char *out[])
{
    UWORD i;

    for (i = 0; i < (UWORD)AMI_CFG_SEARCH_LIST_MAX; i++)
        out[i] = NULL;

    return ami_config_search_list(res, out, (UWORD)AMI_CFG_SEARCH_LIST_MAX);
}

static void test_search_domains(void)
{
    AmiResolverConfig res;
    const char       *list[AMI_CFG_SEARCH_LIST_MAX];
    char             *buf;
    UWORD             n;

    printf("search domains\n");

    /* ---- nothing configured: no suffix, so a short name is asked once. */
    memset(&res, 0, sizeof(res));
    CHECK(search_of(&res, list) == 0);

    /* ---- DOMAIN alone is the one suffix. */
    memset(&res, 0, sizeof(res));
    buf = dup_text("domain localdomain\n");
    ami_cfg_parse_resolver(buf, &res, NULL, 0);
    free(buf);

    CHECK(res.search_static == 0);
    n = search_of(&res, list);
    CHECK(n == 1);
    CHECK_STR(list[0], "localdomain");

    CHECK(ami_config_search_offer(&res, "local.tinic.net") == TRUE);
    CHECK(res.search_static == 0);
    CHECK(res.search_count == 1);

    n = search_of(&res, list);
    CHECK(n == 2);
    CHECK_STR(list[0], "localdomain");
    CHECK_STR(list[1], "local.tinic.net");

    CHECK_STR(res.domain, "localdomain");

    memset(&res, 0, sizeof(res));
    buf = dup_text("domain unused.test\nsearch one.test two.test\n");
    ami_cfg_parse_resolver(buf, &res, NULL, 0);
    free(buf);

    CHECK(res.search_static == 2);
    CHECK(ami_config_search_offer(&res, "three.test") == TRUE);

    n = search_of(&res, list);
    CHECK(n == 3);
    CHECK_STR(list[0], "one.test");
    CHECK_STR(list[1], "two.test");
    CHECK_STR(list[2], "three.test");

    memset(&res, 0, sizeof(res));
    buf = dup_text("domain Home.Lan\n");
    ami_cfg_parse_resolver(buf, &res, NULL, 0);
    free(buf);

    CHECK(ami_config_search_offer(&res, "home.lan") == TRUE);
    n = search_of(&res, list);
    CHECK(n == 1);
    CHECK_STR(list[0], "Home.Lan");

    /* ---- Twice from the lease is once in the list. */
    CHECK(ami_config_search_offer(&res, "other.test") == TRUE);
    CHECK(ami_config_search_offer(&res, "OTHER.TEST") == FALSE);
    CHECK(res.search_count == 2);

    /* ---- Off the network and not a domain name: refused, not queried. */
    memset(&res, 0, sizeof(res));
    CHECK(ami_config_search_offer(&res, "not a domain") == FALSE);
    CHECK(ami_config_search_offer(&res, "-leading.hyphen") == FALSE);
    CHECK(ami_config_search_offer(&res, "double..dot") == FALSE);
    CHECK(ami_config_search_offer(&res, "") == FALSE);
    CHECK(res.search_count == 0);

    /* ---- More than the list holds: the first ones stand, no overrun. */
    memset(&res, 0, sizeof(res));
    {
        char name[16];
        int  i;

        for (i = 0; i < AMI_CFG_MAX_SEARCH + 3; i++)
        {
            snprintf(name, sizeof(name), "d%d.test", i);
            (void)ami_config_search_offer(&res, name);
        }
    }
    CHECK(res.search_count == AMI_CFG_MAX_SEARCH);
    CHECK_STR(res.search[0], "d0.test");
    n = search_of(&res, list);
    CHECK(n == AMI_CFG_MAX_SEARCH);
}

/* DHCP option 119, RFC 3397: a run of RFC 1035 4.1.4 names off the network. */
static void test_dhcp_search_option(void)
{
    AmiResolverConfig res;
    const char       *list[AMI_CFG_SEARCH_LIST_MAX];

    printf("DHCP option 119\n");

    /* "eng.example.com", then "sales.example.com" written with a compression
       pointer back to "example.com" -- RFC 3397 2's own example. */
    {
        static const UBYTE wire[] = {
            3, 'e', 'n', 'g', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
            3, 'c', 'o', 'm', 0,
            5, 's', 'a', 'l', 'e', 's', 0xC0, 4
        };

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_from_rfc3397(&res, wire, sizeof(wire)) == 2);
        CHECK(res.search_count == 2);
        CHECK_STR(res.search[0], "eng.example.com");
        CHECK_STR(res.search[1], "sales.example.com");

        CHECK(search_of(&res, list) == 2);
        CHECK_STR(list[0], "eng.example.com");
    }

    /* The file's SEARCH line still goes first. */
    {
        static const UBYTE wire[] = { 5, 'l', 'e', 'a', 's', 'e', 0 };
        char              *buf;
        UWORD              n;

        memset(&res, 0, sizeof(res));
        buf = dup_text("search file.test\n");
        ami_cfg_parse_resolver(buf, &res, NULL, 0);
        free(buf);

        CHECK(ami_config_search_from_rfc3397(&res, wire, sizeof(wire)) == 1);
        n = search_of(&res, list);
        CHECK(n == 2);
        CHECK_STR(list[0], "file.test");
        CHECK_STR(list[1], "lease");
    }

    /* A pointer to itself, and one that points forwards: RFC 1035 4.1.4 allows
       neither, and either would be a loop. Nothing is stored and nothing
       hangs -- reaching the next line is the assertion. */
    {
        static const UBYTE self[]    = { 0xC0, 0 };
        static const UBYTE forward[] = { 0xC0, 4, 0, 0, 2, 'h', 'i', 0 };

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_from_rfc3397(&res, self, sizeof(self)) == 0);
        CHECK(ami_config_search_from_rfc3397(&res, forward,
                                             sizeof(forward)) == 0);
        CHECK(res.search_count == 0);
    }

    {
        static const UBYTE overrun[] = { 9, 'a', 'b', 'c' };
        static const UBYTE unterminated[] = { 2, 'h', 'i' };
        static const UBYTE trailing[] = { 2, 'o', 'k', 0, 9, 'a' };

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_from_rfc3397(&res, overrun,
                                             sizeof(overrun)) == 0);
        CHECK(ami_config_search_from_rfc3397(&res, unterminated,
                                             sizeof(unterminated)) == 0);
        CHECK(ami_config_search_from_rfc3397(&res, trailing,
                                             sizeof(trailing)) == 1);
        CHECK(res.search_count == 1);
        CHECK_STR(res.search[0], "ok");
    }

    /* An empty option, and a lone root label, name nothing. */
    {
        static const UBYTE root[] = { 0 };

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_from_rfc3397(&res, root, sizeof(root)) == 0);
        CHECK(ami_config_search_from_rfc3397(&res, root, 0) == 0);
        CHECK(res.search_count == 0);
    }

    {
        UBYTE big[AMI_CFG_NAME_LEN + 8];
        ULONG i;

        big[0] = (UBYTE)(AMI_CFG_NAME_LEN + 2);
        for (i = 1; i <= (ULONG)AMI_CFG_NAME_LEN + 2; i++)
            big[i] = (UBYTE)'a';
        big[AMI_CFG_NAME_LEN + 3] = 0;

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_from_rfc3397(&res, big,
                                             AMI_CFG_NAME_LEN + 4) == 0);
        CHECK(res.search_count == 0);
    }
}

/* RFC 8106 5.2, the router advertisement's search list: same encoding as
   DHCP option 119, plus zero-lifetime withdrawal and 8-byte-unit padding. */
static void test_ra_search_option(void)
{
    AmiResolverConfig res;
    const char       *list[AMI_CFG_SEARCH_LIST_MAX];
    char             *buf;

    printf("RFC 8106 search list\n");

    {
        static const UBYTE padded[] = {
            5, 'l', 'o', 'c', 'a', 'l', 5, 't', 'i', 'n', 'i', 'c',
            3, 'n', 'e', 't', 0,
            0, 0, 0, 0, 0, 0, 0
        };

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_from_rfc3397(&res, padded,
                                             sizeof(padded)) == 1);
        CHECK(res.search_count == 1);
        CHECK_STR(res.search[0], "local.tinic.net");

        /* And it is the suffix a name with no dot is tried under. */
        CHECK(search_of(&res, list) == 1);
        CHECK_STR(list[0], "local.tinic.net");

        /* The zero lifetime takes it back out. */
        CHECK(ami_config_search_withdraw_rfc3397(&res, padded,
                                                 sizeof(padded)) == 1);
        CHECK(res.search_count == 0);
        CHECK(search_of(&res, list) == 0);

        /* And a second withdrawal of the same name changes nothing. */
        CHECK(ami_config_search_withdraw_rfc3397(&res, padded,
                                                 sizeof(padded)) == 0);
        CHECK(res.search_count == 0);
    }

    {
        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_offer(&res, "one.test") == TRUE);
        CHECK(ami_config_search_offer(&res, "two.test") == TRUE);
        CHECK(ami_config_search_offer(&res, "three.test") == TRUE);

        CHECK(ami_config_search_withdraw(&res, "two.test") == TRUE);
        CHECK(res.search_count == 2);
        CHECK_STR(res.search[0], "one.test");
        CHECK_STR(res.search[1], "three.test");

        /* Case-insensitively, as every other comparison here is (RFC 4343). */
        CHECK(ami_config_search_withdraw(&res, "ONE.TEST") == TRUE);
        CHECK(res.search_count == 1);
        CHECK_STR(res.search[0], "three.test");
    }

    {
        memset(&res, 0, sizeof(res));
        buf = dup_text("search file.test\n");
        ami_cfg_parse_resolver(buf, &res, NULL, 0);
        free(buf);

        CHECK(res.search_static == 1);
        CHECK(ami_config_search_withdraw(&res, "file.test") == FALSE);
        CHECK(res.search_count == 1);
        CHECK_STR(res.search[0], "file.test");

        /* The network's own entry above it still goes. */
        CHECK(ami_config_search_offer(&res, "ra.test") == TRUE);
        CHECK(ami_config_search_withdraw(&res, "ra.test") == TRUE);
        CHECK(res.search_count == 1);
        CHECK(res.search_static == 1);
    }

    /* Nothing, and a name that was never there. */
    {
        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_withdraw(&res, "absent.test") == FALSE);
        CHECK(ami_config_search_withdraw(&res, "") == FALSE);
        CHECK(ami_config_search_withdraw(&res, NULL) == FALSE);
        CHECK(ami_config_search_withdraw(NULL, "x.test") == FALSE);
    }

    {
        memset(&res, 0, sizeof(res));
        CHECK(ami_config_search_offer(&res, "shared.test") == TRUE);
        CHECK(res.search_use[0] == 1);
        CHECK(ami_config_search_reference_add(&res, "SHARED.TEST") == TRUE);
        CHECK(res.search_count == 1 && res.search_use[0] == 2);
        CHECK(ami_config_search_reference_remove(&res, "shared.test") == TRUE);
        CHECK(res.search_count == 1 && res.search_use[0] == 1);
        CHECK(ami_config_search_reference_remove(&res, "shared.test") == TRUE);
        CHECK(res.search_count == 0);
    }

    {
        memset(&res, 0, sizeof(res));
        buf = dup_text("search file.test\n");
        ami_cfg_parse_resolver(buf, &res, NULL, 0);
        free(buf);

        CHECK(ami_config_search_reference_add(&res, "FILE.TEST") == TRUE);
        CHECK(ami_config_search_reference_remove(&res, "file.test") == TRUE);
        CHECK(res.search_count == 1 && res.search_static == 1);
        CHECK_STR(res.search[0], "file.test");
    }
}

/* RFC 8106 5.1, the advertised servers.  A router naming more than the list
   holds must not evict servers that are already answering. */
static void test_ra_nameserver6(void)
{
    AmiResolverConfig res;
    ULONG             a[4] = { 0x2607F598UL, 0xE1A84C00UL, 0xE63A6EFFUL,
                               0xFE03D5BAUL };
    ULONG             b[4] = { 0x20010DB8UL, 0, 0, 0x00000053UL };
    ULONG             s[6][4];
    int               i;

    printf("RFC 8106 name servers\n");

    memset(&res, 0, sizeof(res));

    CHECK(ami_config_nameserver6_offer(&res, a) == TRUE);
    CHECK(res.nameserver6_count == 1);
    CHECK(res.nameserver6[0][0] == 0x2607F598UL);
    CHECK(res.nameserver6[0][3] == 0xFE03D5BAUL);

    /* Every advertisement repeats the option: the second is not a change. */
    CHECK(ami_config_nameserver6_offer(&res, a) == FALSE);
    CHECK(res.nameserver6_count == 1);

    /* nameserver6_use[] is ObtainDomainNameServerList()'s dnsn_UseCount for
       the IPv6 half: one owner, acquired at run time, so positive and never
       zero.  It reported a constant 1 before RemoveDomainNameServer() could
       take an IPv6 server away at all. */
    CHECK(res.nameserver6_use[0] == 1);

    CHECK(ami_config_nameserver6_offer(&res, b) == TRUE);
    CHECK(res.nameserver6_count == 2);
    CHECK(res.nameserver6_use[1] == 1);

    /* A count belongs to its address and has to travel with it when the list
       is compacted, not stay at the index it was written to. */
    res.nameserver6_use[1] = 3;

    /* Withdrawing the first leaves the second, and its order. */
    CHECK(ami_config_nameserver6_withdraw(&res, a) == TRUE);
    CHECK(res.nameserver6_count == 1);
    CHECK(res.nameserver6[0][0] == 0x20010DB8UL);
    CHECK(res.nameserver6[0][3] == 0x00000053UL);
    CHECK(res.nameserver6_use[0] == 3);
    CHECK(res.nameserver6_use[1] == 0);

    /* And a second withdrawal of the same address changes nothing. */
    CHECK(ami_config_nameserver6_withdraw(&res, a) == FALSE);
    CHECK(res.nameserver6_count == 1);

    memset(&res, 0, sizeof(res));
    for (i = 0; i < 6; i++)
    {
        s[i][0] = 0x20010DB8UL;
        s[i][1] = 0UL;
        s[i][2] = 0UL;
        s[i][3] = (ULONG)(i + 1);
    }

    for (i = 0; i < AMI_CFG_MAX_NAMESERVERS; i++)
        CHECK(ami_config_nameserver6_offer(&res, s[i]) == TRUE);

    CHECK(res.nameserver6_count == AMI_CFG_MAX_NAMESERVERS);
    CHECK(ami_config_nameserver6_offer(&res, s[AMI_CFG_MAX_NAMESERVERS])
              == FALSE);
    CHECK(res.nameserver6_count == AMI_CFG_MAX_NAMESERVERS);

    for (i = 0; i < AMI_CFG_MAX_NAMESERVERS; i++)
        CHECK(res.nameserver6[i][3] == (ULONG)(i + 1));

    /* The refused one is not in the list under any reading. */
    CHECK(ami_config_nameserver6_withdraw(&res, s[AMI_CFG_MAX_NAMESERVERS])
              == FALSE);
    CHECK(res.nameserver6_count == AMI_CFG_MAX_NAMESERVERS);

    CHECK(ami_config_nameserver6_withdraw(&res, s[1]) == TRUE);
    CHECK(res.nameserver6_count == (UWORD)(AMI_CFG_MAX_NAMESERVERS - 1));
    CHECK(res.nameserver6[0][3] == 1);
    CHECK(res.nameserver6[1][3] == 3);
    CHECK(res.nameserver6[2][3] == 4);
    CHECK(res.nameserver6[AMI_CFG_MAX_NAMESERVERS - 1][3] == 0);

    /* :: is not a name server, and neither argument may be NULL. */
    {
        ULONG zero[4] = { 0, 0, 0, 0 };

        memset(&res, 0, sizeof(res));
        CHECK(ami_config_nameserver6_offer(&res, zero) == FALSE);
        CHECK(ami_config_nameserver6_offer(&res, NULL) == FALSE);
        CHECK(ami_config_nameserver6_offer(NULL, a) == FALSE);
        CHECK(ami_config_nameserver6_withdraw(&res, NULL) == FALSE);
        CHECK(ami_config_nameserver6_withdraw(NULL, a) == FALSE);
        CHECK(res.nameserver6_count == 0);
    }
}

static void test_gateway(void)
{
    ULONG gw;
    char *buf;

    printf("default_gateway / routes\n");

    gw  = 0;
    buf = dup_text(
        "; DEVS:Internet/default_gateway\n"
        "DEVICE=ariadne.device\n"
        "UNIT=0\n"
        "GATEWAY=192.168.1.1\n");
    ami_cfg_parse_gateway(buf, &gw);
    free(buf);
    CHECK_IP(gw, 192, 168, 1, 1);

    /* Roadshow's routes file: a specific route must not be taken as default. */
    gw  = 0;
    buf = dup_text(
        "# DEVS:Internet/routes\n"
        "netdst=10.0.0.0 via=192.168.1.9\n"
        "default=192.168.1.254\n");
    ami_cfg_parse_gateway(buf, &gw);
    free(buf);
    CHECK_IP(gw, 192, 168, 1, 254);

    /* Only specific routes: no default gateway at all. */
    gw  = 0;
    buf = dup_text("hostdst 10.1.2.3 via 10.0.0.1\n");
    ami_cfg_parse_gateway(buf, &gw);
    free(buf);
    CHECK(gw == 0);
}

static void test_tcp_handler(void)
{
    BOOL  on;
    char *buf;

    printf("tcp_handler\n");

    on  = TRUE;
    buf = dup_text("; DEVS:Internet/tcp_handler\nTCPHANDLER=OFF\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == FALSE);

    /* One setting, so the file may be one word. */
    on  = TRUE;
    buf = dup_text("off\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == FALSE);

    /* Back on again, spelled the other three ways. */
    on  = FALSE;
    buf = dup_text("TCP YES\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == TRUE);

    on  = FALSE;
    buf = dup_text("tcphandler = on\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == TRUE);

    on  = FALSE;
    buf = dup_text("1\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == TRUE);

    on  = TRUE;
    buf = dup_text("# TCPHANDLER=OFF\n\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == TRUE);

    on  = TRUE;
    buf = dup_text("TCPHANDLER=maybe\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == TRUE);

    on  = TRUE;
    buf = dup_text("NAMESERVER=192.168.1.1\n");
    ami_cfg_parse_tcp_handler(buf, &on);
    free(buf);
    CHECK(on == TRUE);

    ami_cfg_parse_tcp_handler(NULL, &on);
    CHECK(on == TRUE);
}

static void test_netdb(void)
{
    const AmiNetdbEntry *e;

    printf("netdb\n");

    clear_fixtures();
    set_fixture(AMI_CFG_FILE_HOSTS,
        "# DEVS:Internet/hosts\n"
        "127.0.0.1\tlocalhost\tloopback lb\n"
        "192.168.1.42   amiga1200   a1200 workbench   # my machine\n"
        "HOST 192.168.1.1 router gateway\n"
        "NAMESERVER 192.168.1.1\n"
        "\n");
    set_fixture(AMI_CFG_FILE_NETWORKS,
        "loopback 127\n"
        "home 192.168.1 lan\n");
    set_fixture(AMI_CFG_FILE_PROTOCOLS,
        "ip 0 IP # internet protocol\n"
        "icmp\t1\tICMP\n"
        "tcp 6 TCP\n"
        "udp 17 UDP\n");
    set_fixture(AMI_CFG_FILE_SERVICES,
        "ftp\t\t21/tcp\n"
        "telnet\t\t23/tcp\n"
        "domain\t\t53/udp\tnameserver\n"
        "domain\t\t53/tcp\tnameserver\n"
        "http\t\t80/tcp\twww www-http\n");

    CHECK(ami_netdb_load() == AMI_CFG_OK);

    e = ami_netdb_host_by_name("localhost");
    CHECK(e != NULL);
    if (e) { CHECK_IP(e->value, 127, 0, 0, 1); CHECK_STR(e->name, "localhost"); }

    /* Alias lookup, and case-insensitivity. */
    e = ami_netdb_host_by_name("WORKBENCH");
    CHECK(e != NULL);
    if (e) CHECK_IP(e->value, 192, 168, 1, 42);

    /* The AmiTCP "HOST <addr> <name>" line in a standard hosts file. */
    e = ami_netdb_host_by_name("router");
    CHECK(e != NULL);
    if (e) {
        CHECK_IP(e->value, 192, 168, 1, 1);
        CHECK(e->aliases != NULL && e->aliases[0] != NULL);
        if (e->aliases && e->aliases[0]) CHECK_STR(e->aliases[0], "gateway");
        CHECK(e->aliases[1] == NULL);
    }

    /* A NAMESERVER line in the hosts file is not a netdb entry. */
    CHECK(ami_netdb_host_by_name("NAMESERVER") == NULL);

    e = ami_netdb_host_by_addr(0xC0A8012AUL);
    CHECK(e != NULL);
    if (e) CHECK_STR(e->name, "amiga1200");

    /* A mid-line '#' comment must not become an alias. */
    e = ami_netdb_host_by_name("amiga1200");
    CHECK(e != NULL);
    if (e) {
        CHECK_STR(e->aliases[0], "a1200");
        CHECK_STR(e->aliases[1], "workbench");
        CHECK(e->aliases[2] == NULL);
    }

    e = ami_netdb_net_by_name("lan");
    CHECK(e != NULL);
    if (e) { CHECK(e->value == 0x00C0A801UL); CHECK_STR(e->name, "home"); }
    e = ami_netdb_net_by_addr(127);
    CHECK(e != NULL);
    if (e) CHECK_STR(e->name, "loopback");

    e = ami_netdb_proto_by_name("TCP");
    CHECK(e != NULL);
    if (e) CHECK(e->value == 6);
    e = ami_netdb_proto_by_number(17);
    CHECK(e != NULL);
    if (e) CHECK_STR(e->name, "udp");
    CHECK(ami_netdb_proto_by_name("sctp") == NULL);

    e = ami_netdb_serv_by_name("domain", "tcp");
    CHECK(e != NULL);
    if (e) { CHECK(e->value == 53); CHECK_STR(e->proto, "tcp"); }
    e = ami_netdb_serv_by_name("nameserver", "udp");     /* by alias */
    CHECK(e != NULL);
    if (e) CHECK_STR(e->proto, "udp");
    e = ami_netdb_serv_by_name("http", NULL);            /* any protocol */
    CHECK(e != NULL);
    if (e) CHECK(e->value == 80);
    e = ami_netdb_serv_by_port(21, "tcp");
    CHECK(e != NULL);
    if (e) CHECK_STR(e->name, "ftp");
    CHECK(ami_netdb_serv_by_port(21, "udp") == NULL);

    /* get*ent() iteration walks the file in order and stops with NULL. */
    {
        ULONG i;
        ULONG count = 0;

        for (i = 0; ami_netdb_proto_entry(i) != NULL; i++)
            count++;
        CHECK(count == 4);
        CHECK_STR(ami_netdb_proto_entry(0)->name, "ip");
        CHECK_STR(ami_netdb_proto_entry(3)->name, "udp");

        count = 0;
        for (i = 0; ami_netdb_serv_entry(i) != NULL; i++)
            count++;
        CHECK(count == 5);

        count = 0;
        for (i = 0; ami_netdb_net_entry(i) != NULL; i++)
            count++;
        CHECK(count == 2);
    }

    ami_netdb_free();
    CHECK(ami_alloc_count() == 0);
}

static void test_netdb_missing_files(void)
{
    const AmiNetdbEntry *e;

    printf("netdb: built-in fallback\n");

    clear_fixtures();            /* every file absent */
    CHECK(ami_netdb_load() == AMI_CFG_OK);

    e = ami_netdb_host_by_name("localhost");
    CHECK(e != NULL);
    if (e) CHECK_IP(e->value, 127, 0, 0, 1);

    e = ami_netdb_proto_by_name("tcp");
    CHECK(e != NULL);
    if (e) CHECK(e->value == 6);

    e = ami_netdb_serv_by_name("http", "tcp");
    CHECK(e != NULL);
    if (e) CHECK(e->value == 80);

    e = ami_netdb_serv_by_name("smtp", "tcp");
    CHECK(e != NULL);
    if (e) { CHECK(e->value == 25); CHECK_STR(e->aliases[0], "mail"); }

    ami_netdb_free();
    CHECK(ami_alloc_count() == 0);
}

static void test_netdb_garbage(void)
{
    printf("netdb: malformed input\n");

    clear_fixtures();
    set_fixture(AMI_CFG_FILE_HOSTS,
        "\n\n   \n"
        "#only a comment\n"
        "notanaddress name\n"
        "1.2.3\n"
        "10.0.0.1\n"                 /* address with no name */
        "10.0.0.2 ok\n");
    set_fixture(AMI_CFG_FILE_SERVICES,
        "noport\n"
        "weird 80\n"                 /* no /proto */
        "empty 81/\n"                /* empty protocol */
        "bad xx/tcp\n"
        "wrapped 4294967297/tcp\n"
        "narrowed 65557/tcp\n"
        "maximum 65535/tcp\n"
        "good 90/tcp\n");
    set_fixture(AMI_CFG_FILE_PROTOCOLS,
        "maximum 255 MAXIMUM\n"
        "oversized 256 OVERSIZED\n"
        "negative-looking 4294967295 NEGATIVE-LOOKING\n");

    CHECK(ami_netdb_load() == AMI_CFG_OK);
    CHECK(ami_netdb_host_by_name("ok") != NULL);
    CHECK(ami_netdb_host_by_name("notanaddress") == NULL);
    CHECK(ami_netdb_serv_by_name("bad", NULL) == NULL);
    CHECK(ami_netdb_serv_by_name("wrapped", NULL) == NULL);
    CHECK(ami_netdb_serv_by_name("narrowed", NULL) == NULL);
    CHECK(ami_netdb_serv_by_name("maximum", "tcp") != NULL);
    CHECK(ami_netdb_serv_by_name("weird", NULL) == NULL);
    CHECK(ami_netdb_serv_by_name("empty", NULL) == NULL);
    CHECK(ami_netdb_serv_by_name("good", "tcp") != NULL);
    CHECK(ami_netdb_proto_by_name("maximum") != NULL);
    CHECK(ami_netdb_proto_by_name("oversized") == NULL);
    CHECK(ami_netdb_proto_by_name("negative-looking") == NULL);

    ami_netdb_free();
    CHECK(ami_alloc_count() == 0);
}

static void test_service_discovery(void)
{
    AmiSdService svc[AMI_CFG_MAX_SD_SERVICES];
    UWORD        count;
    char        *buf;

    printf("service_discovery\n");

    memset(svc, 0, sizeof(svc));
    count = 0;

    buf = dup_text(
        "# DEVS:Internet/service_discovery\n"
        "; a leading semicolon is a comment too\n"
        "\n"
        "_ftp._tcp\t21\n"
        "  _http._tcp   80    Amiga web server\n"
        "_smb._tcp 139 \"Quoted Name\"\n"
        "_http._udp 8080 txt=path=/;u=guest\n"
        "_ssh._tcp 22 Shell txt=v=2\n"
        "_daap._tcp 3689 # a comment after the line\n");
    ami_cfg_parse_dnssd(buf, svc, AMI_CFG_MAX_SD_SERVICES, &count);
    free(buf);

    CHECK(count == 6);

    CHECK_STR(svc[0].type, "_ftp._tcp");
    CHECK(svc[0].port == 21);
    CHECK_STR(svc[0].name, "");         /* empty: the host name is used */
    CHECK_STR(svc[0].txt, "");

    /* An unquoted instance name runs to the end of the line, spaces and all. */
    CHECK_STR(svc[1].type, "_http._tcp");
    CHECK(svc[1].port == 80);
    CHECK_STR(svc[1].name, "Amiga web server");

    CHECK_STR(svc[2].name, "Quoted Name");

    /* txt= takes the rest of the line, ';' included, it is the separator. */
    CHECK_STR(svc[3].type, "_http._udp");
    CHECK(svc[3].port == 8080);
    CHECK_STR(svc[3].name, "");
    CHECK_STR(svc[3].txt, "path=/;u=guest");

    CHECK_STR(svc[4].name, "Shell");
    CHECK_STR(svc[4].txt, "v=2");

    CHECK_STR(svc[5].type, "_daap._tcp");
    CHECK(svc[5].port == 3689);
    CHECK_STR(svc[5].name, "");

    /* Every kind of malformed line: reported, skipped, never fatal. */
    memset(svc, 0, sizeof(svc));
    count = 0;

    buf = dup_text(
        "ftp._tcp 21\n"              /* no leading underscore              */
        "_ftp 21\n"                  /* no transport                       */
        "_ftp._sctp 21\n"            /* not a transport we know            */
        "_f*tp._tcp 21\n"            /* illegal character in the type      */
        "_thisnameiswaytoolong._tcp 21\n"
        "__._tcp 21\n"               /* empty service label                */
        "_ftp._tcp\n"                /* no port                            */
        "_ftp._tcp 0\n"
        "_ftp._tcp 65536\n"
        "_ftp._tcp 4294967297\n"    /* must not wrap to port 1             */
        "_ftp._tcp notanumber\n"
        "_ftp._tcp 21 My.Server\n"   /* a dot would become a label break   */
        "_good._tcp 22\n");
    ami_cfg_parse_dnssd(buf, svc, AMI_CFG_MAX_SD_SERVICES, &count);
    free(buf);

    CHECK(count == 1);
    CHECK_STR(svc[0].type, "_good._tcp");
    CHECK(svc[0].port == 22);

    /* More lines than slots: the first ones are kept and nothing overruns. */
    memset(svc, 0, sizeof(svc));
    count = 0;

    buf = dup_text(
        "_a._tcp 1\n_b._tcp 2\n_c._tcp 3\n_d._tcp 4\n_e._tcp 5\n"
        "_f._tcp 6\n_g._tcp 7\n_h._tcp 8\n_i._tcp 9\n_j._tcp 10\n");
    ami_cfg_parse_dnssd(buf, svc, AMI_CFG_MAX_SD_SERVICES, &count);
    free(buf);

    CHECK(count == AMI_CFG_MAX_SD_SERVICES);
    CHECK_STR(svc[AMI_CFG_MAX_SD_SERVICES - 1].type, "_h._tcp");

    /* A max below the array size, and a count that does not start at zero. */
    memset(svc, 0, sizeof(svc));
    count = 2;

    buf = dup_text("_a._tcp 1\n_b._tcp 2\n_c._tcp 3\n_d._tcp 4\n");
    ami_cfg_parse_dnssd(buf, svc, 3, &count);
    free(buf);

    CHECK(count == 3);
    CHECK_STR(svc[0].type, "");
    CHECK_STR(svc[2].type, "_a._tcp");
    CHECK_STR(svc[3].type, "");

    /* A name that leaves no room for the module's " (2)" rename suffix. */
    {
        char  line[AMI_CFG_NAME_LEN + 32];
        int   i;

        strcpy(line, "_ftp._tcp 21 ");
        for (i = 0; i < AMI_CFG_NAME_LEN; i++)
            strcat(line, "x");
        strcat(line, "\n");

        memset(svc, 0, sizeof(svc));
        count = 0;
        buf = dup_text(line);
        ami_cfg_parse_dnssd(buf, svc, AMI_CFG_MAX_SD_SERVICES, &count);
        free(buf);

        CHECK(count == 0);
    }

    /* Nothing here allocates, so nothing here can leak. */
    CHECK(ami_alloc_count() == 0);
}

static void test_interface_drawer(void)
{
    AmiConfig cfg;
    ULONG     base = ami_alloc_count();
    UWORD     i;

    printf("interface drawer: every definition survives\n");

    memset(&cfg, 0, sizeof(cfg));
    clear_fixtures();
    clear_drawer();

    /* Staged out of order, so the sort has something to do. */
    stage_interface("eth2",    2);
    stage_interface("wifi0",   0);
    stage_interface("eth0",    0);
    stage_interface("slip0",   0);
    stage_interface("eth1",    1);
    stage_interface("ppp0",    0);
    stage_interface("arcnet0", 0);
    stage_interface("eth3",    3);
    stage_interface("zorro0",  0);

    ami_config_load_interfaces(&cfg);

    /* NINE.  Not two, and not any other number this tree picked. */
    CHECK(cfg.interface_count == 9);

    CHECK_STR(cfg.interfaces[0].name, "arcnet0");
    CHECK_STR(cfg.interfaces[1].name, "eth0");
    CHECK_STR(cfg.interfaces[2].name, "eth1");
    CHECK_STR(cfg.interfaces[3].name, "eth2");
    CHECK_STR(cfg.interfaces[4].name, "eth3");
    CHECK_STR(cfg.interfaces[5].name, "ppp0");
    CHECK_STR(cfg.interfaces[6].name, "slip0");
    CHECK_STR(cfg.interfaces[7].name, "wifi0");
    CHECK_STR(cfg.interfaces[8].name, "zorro0");

    CHECK(cfg.interfaces[4].unit == 3);
    for (i = 0; i < cfg.interface_count; i++)
        CHECK(cfg.interfaces[i].iptype == AMI_IPTYPE_DHCP);

    /* The list grew to hold them rather than stopping at the floor. */
    CHECK(cfg.interface_capacity >= 9);

    ami_config_free(&cfg);

    /* AmigaOS reclaims nothing a Process did not free, so the census must
       come back to exactly where it started. */
    CHECK(cfg.interfaces == NULL);
    CHECK(cfg.interface_count == 0);
    CHECK(cfg.interface_capacity == 0);
    CHECK(ami_alloc_count() == base);

    ami_config_free(&cfg);
    CHECK(ami_alloc_count() == base);

    clear_fixtures();
    clear_drawer();
}

/* The growth itself: what it keeps, what it costs, and what it refuses. */
static void test_interface_reserve(void)
{
    AmiConfig cfg;
    ULONG     base = ami_alloc_count();
    UWORD     i;

    printf("interface list: growth keeps what it held\n");

    memset(&cfg, 0, sizeof(cfg));

    /* The floor, as ami_config_load() takes it up front. */
    CHECK(ami_config_reserve(&cfg, (UWORD)AMI_CFG_IFACE_FLOOR));
    CHECK(cfg.interfaces != NULL);
    CHECK(cfg.interface_capacity >= (UWORD)AMI_CFG_IFACE_FLOOR);

    for (i = 0; i < (UWORD)AMI_CFG_IFACE_FLOOR; i++)
    {
        (void)snprintf(cfg.interfaces[i].name,
                       sizeof(cfg.interfaces[i].name),
                       "if%u", (unsigned)i);
        cfg.interface_count++;
    }

    /* Far past any ceiling this tree ever had. */
    CHECK(ami_config_reserve(&cfg, 300U));
    CHECK(cfg.interface_capacity >= 300U);

    /* Carried over, not lost; and everything past the old end is zeroed. */
    for (i = 0; i < (UWORD)AMI_CFG_IFACE_FLOOR; i++)
    {
        char want[16];

        (void)snprintf(want, sizeof(want), "if%u", (unsigned)i);
        CHECK_STR(cfg.interfaces[i].name, want);
    }
    CHECK(cfg.interfaces[299].name[0] == '\0');

    {
        ULONG held = ami_alloc_count();

        CHECK(ami_config_reserve(&cfg, 10U));
        CHECK(ami_alloc_count() == held);
    }

    ami_config_free(&cfg);
    CHECK(ami_alloc_count() == base);

    /* NULL is not a crash, and neither is a config nothing ever loaded. */
    CHECK(!ami_config_reserve(NULL, 1U));
    ami_config_free(NULL);
    memset(&cfg, 0, sizeof(cfg));
    ami_config_free(&cfg);
    CHECK(ami_alloc_count() == base);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        stub_verbose = 1;

    test_text_helpers();
    test_ip();
    test_ip6();
#ifdef AMINETXDUO_IPV6
    test_interface_ipv6();
#endif
    test_interface_roadshow();
    test_interface_static();
    test_interface_amitcp_flavour();
    test_interface_errors();
    test_problem_reporter();
    test_inert_keywords_are_notes();
    test_interface_ipv6_only();
#ifdef AMINETXDUO_IPV6
    test_ipv6_only_no_error();
#endif
    test_interface_card();
    test_interface_drawer();
    test_interface_reserve();
    test_hostname_syntax();
    test_hostname_precedence();
    test_hostname_from_hwaddr();
    test_hostname_offer();
    test_resolver();
    test_search_domains();
    test_dhcp_search_option();
    test_ra_search_option();
    test_ra_nameserver6();
    test_gateway();
    test_tcp_handler();
    test_netdb();
    test_netdb_missing_files();
    test_netdb_garbage();
    test_service_discovery();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
