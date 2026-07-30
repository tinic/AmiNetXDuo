/*
 * AmiNetXDuo -- host-side test for the configuration and netdb parsers.
 *
 * Builds and runs on the development host (cc -std=c99), not on the Amiga:
 * config_text.c, config_parse.c and netdb.c contain no AmigaDOS calls, so all
 * they need is the tiny <exec/types.h> shim in test/shim and the three stubs
 * below (ami_alloc/ami_free/ami_log and the ami_cfg_read_file hook, which is
 * answered here from an in-memory fixture table rather than from disk).
 *
 * config_file.c -- the dos.library Open/Read/Close and the Examine/ExNext scan
 * of DEVS:NetInterfaces -- is therefore not covered here; it is verified by
 * compilation and needs an on-Amiga run to be tested properly.
 *
 *   cc -std=c99 -Wall -Wextra -I../../../include -Ishim \
 *      test_config.c ../config_text.c ../config_parse.c ../netdb.c -o test_config
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
fixtures[8];

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
                   __FILE__, __LINE__, (want), (got) ? (got) : "(null)");    \
        }                                                                    \
    } while (0)

#define CHECK_IP(got, a, b, c, d)                                            \
    CHECK((got) == (((ULONG)(a) << 24) | ((ULONG)(b) << 16) |                \
                    ((ULONG)(c) << 8)  |  (ULONG)(d)))

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


/* The text conversions are compiled either way (src/config/config_text6.c),
   so they are tested either way. */

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
        };
        size_t i;

        for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        {
            ami_config_format_ip6(cases[i], text, sizeof(text));
            CHECK_STR(text, want[i]);
        }
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

    /* A buffer that cannot hold the longest form yields "", never a
       truncated address that would parse back as something else. */
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
    CHECK(cfg.prefix6 == 48);
    CHECK(cfg.address6[0] == 0x20010db8UL && cfg.address6[3] == 0x10UL);
    CHECK(cfg.have_gateway6);
    CHECK(cfg.gateway6[0] == 0xfe800000UL && cfg.gateway6[3] == 1UL);

    /* No IPv6 keyword at all: AUTO, prefix 64, no address, no router. */
    printf("interface file: ipv6 defaults\n");
    strcpy(buf, "device=a2065.device\nconfigure=dhcp\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_AUTO);
    CHECK(cfg.prefix6 == 64);
    CHECK(!cfg.have_gateway6);

    /* ADDRESS6 with no CONFIGURE6 implies STATIC, as ADDRESS implies a
       static IPv4 interface. */
    printf("interface file: address6 implies static\n");
    strcpy(buf, "device=a2065.device\naddress6=2001:db8::5\n");
    CHECK(ami_cfg_parse_interface("eth0", buf, &cfg) == AMI_CFG_OK);
    CHECK(cfg.ip6type == AMI_IP6TYPE_STATIC);
    CHECK(cfg.prefix6 == 64);

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
    /* Tabs, CRLF, trailing blanks, spaces around '=', both comment styles,
     * mixed case keywords, an unrecognised-but-real Roadshow keyword and a
     * blank line: the shapes hand-edited files actually arrive in. */
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
        "state=down\r\n"
        "# end\r\n");

    printf("interface: static, messy formatting\n");

    CHECK(ami_cfg_parse_interface("eth0", buf, &iface) == AMI_CFG_OK);
    CHECK_STR(iface.name, "eth0");
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
    /* The IPTYPE=DHCP spelling from the AmiTCP/Genesis-era documentation, and
     * the 'address=dhcp' spelling Roadshow really uses. */
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

/*
 * The problem reporter puts a file name, a line number and a suggestion on the
 * user's screen (src/tools/tool_diag.c installs one). The line numbers are
 * what these tests pin down: they make the message actionable and are easy to
 * break by adding a `continue`.
 */
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
    if (problem->hint != NULL)
        strncpy(seen[seen_count].hint, problem->hint, sizeof(seen[0].hint) - 1);

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
        "bad xx/tcp\n"
        "good 90/tcp\n");

    CHECK(ami_netdb_load() == AMI_CFG_OK);
    CHECK(ami_netdb_host_by_name("ok") != NULL);
    CHECK(ami_netdb_host_by_name("notanaddress") == NULL);
    CHECK(ami_netdb_serv_by_name("bad", NULL) == NULL);
    CHECK(ami_netdb_serv_by_name("weird", NULL) != NULL);
    CHECK(ami_netdb_serv_by_name("good", "tcp") != NULL);

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

    /* txt= takes the rest of the line, ';' included -- it is the separator. */
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
    test_resolver();
    test_gateway();
    test_netdb();
    test_netdb_missing_files();
    test_netdb_garbage();
    test_service_discovery();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
