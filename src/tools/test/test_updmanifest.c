/*
 * The tests for src/tools/updmanifest.c: one good manifest, and each way of
 * refusing one, by its own result code.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updmanifest.h"

#include <stdio.h>
#include <string.h>

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

#define H1 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define H2 "fedcba9876543210FEDCBA9876543210fedcba9876543210fedcba9876543210"

#define L_FORMAT    "format=1\n"
#define L_PRODUCT   "product=AmiNetXDuo\n"
#define L_VERSION   "version=1.0.0-rc5\n"
#define L_CHANNEL   "channel=prerelease\n"
#define L_CERTS     "certificates.prev=" H2 "\n"
#define L_CFULL     "container=full 123456 " H1 \
                    " AmiNetXDuo-1.0.0-rc5-full.update.lha\n"
#define L_CMICRO    "container=micro 65432 " H2 \
                    " AmiNetXDuo-1.0.0-rc5-micro.update.lha\n"
#define L_F1        "file=full LIBS 98765 " H1 " bsdsocket.library\n"
#define L_F2        "file=full DEVS/Networks 4321 " H2 " anxa2065.device\n"
#define L_F3        "file=full C 0 " H1 " AmiNetXDuo Tools/fetch\n"
#define L_F4        "file=full certificates 200000 " H2 " certificates\n"
#define L_FM        "file=micro LIBS 50000 " H1 " bsdsocket.library\n"

#define HEAD        L_FORMAT "# AmiNetXDuo 1.0.0-rc5\n" L_PRODUCT L_VERSION \
                    L_CHANNEL L_CERTS
#define GOOD        HEAD L_CFULL L_CMICRO L_F1 L_F2 L_F3 L_F4 L_FM

static char    buf[8192];
static UpdFile files[8];

/* `body` with end= appended over its own line count. */
static const char *with_end(const char *body)
{
    unsigned long lines = 0;
    const char   *p;

    for (p = body; *p != '\0'; p++)
        lines += (*p == '\n');

    snprintf(buf, sizeof(buf), "%send=%lu\n", body, lines);
    return buf;
}

static UpdWant want_rc4(void)
{
    UpdWant w;

    upd_ver_parse("1.0.0-rc4", 9, &w.installed);
    w.channel = UPD_CHANNEL_PRERELEASE;
    w.flavour = UPD_FLAVOUR_FULL;
    return w;
}

static UpdResult parse_raw(const char *text, unsigned long len,
                           const UpdWant *w, UpdManifest *m)
{
    return upd_manifest_parse(text, len, w, m, files, 8);
}

static UpdResult parse_w(const char *text, const UpdWant *w)
{
    UpdManifest m;

    return parse_raw(text, (unsigned long)strlen(text), w, &m);
}

static UpdResult parse(const char *text)
{
    UpdWant w = want_rc4();

    return parse_w(text, &w);
}

/* `body` plus end=, refused with `want`. */
#define REFUSE(body, want)                                                   \
    do {                                                                     \
        UpdResult r_ = parse(with_end(body));                                \
        checks++;                                                            \
        if (r_ != (want)) {                                                  \
            failures++;                                                      \
            printf("  FAIL %s:%d: got %s, wanted %s\n", __FILE__, __LINE__, \
                   upd_result_text(r_), upd_result_text(want));             \
        }                                                                    \
    } while (0)

static int same(const char *p, unsigned long n, const char *want)
{
    return n == strlen(want) && memcmp(p, want, n) == 0;
}

static void test_good(void)
{
    UpdWant     w = want_rc4();
    UpdManifest m;
    const char *t = with_end(GOOD);

    CHECK(parse_raw(t, (unsigned long)strlen(t), &w, &m) == UPD_OK);
    CHECK(strcmp(m.version.pre, "rc5") == 0);
    CHECK(m.channel == UPD_CHANNEL_PRERELEASE);
    CHECK(m.has_certs_prev && m.certs_prev[0] == 0xfe &&
          m.certs_prev[31] == 0x10);
    CHECK(same(m.container, m.container_len,
               "AmiNetXDuo-1.0.0-rc5-full.update.lha"));
    CHECK(m.container_size == 123456UL);
    CHECK(m.container_sha256[0] == 0x01 && m.container_sha256[31] == 0xef);

    CHECK(m.nfiles == 4);
    CHECK(files[0].cls == UPD_CLASS_LIBS && files[0].size == 98765UL &&
          same(files[0].path, files[0].path_len, "bsdsocket.library"));
    CHECK(files[1].cls == UPD_CLASS_NETWORKS && files[1].sha256[0] == 0xfe);
    CHECK(files[2].cls == UPD_CLASS_C && files[2].size == 0 &&
          same(files[2].path, files[2].path_len, "AmiNetXDuo Tools/fetch"));
    CHECK(files[3].cls == UPD_CLASS_CERTIFICATES);

    /* The micro flavour sees only its own. */
    w.flavour = UPD_FLAVOUR_MICRO;
    CHECK(parse_raw(t, (unsigned long)strlen(t), &w, &m) == UPD_OK);
    CHECK(m.nfiles == 1 && m.container_size == 65432UL);

    /* A release from a prerelease, and a key from a later format. */
    w = want_rc4();
    REFUSE(HEAD "x.later=anything at all\n" L_CFULL L_F1, UPD_OK);
    CHECK(parse_w(with_end(L_FORMAT L_PRODUCT "version=1.0.0\n" L_CHANNEL
                           L_CFULL), &w) == UPD_OK);

    /* Every exact-length limit is accepted. */
    {
        char line[UPD_LINE_MAX + 2];
        char body[2048];

        memset(line, 'a', sizeof(line));
        memcpy(line, "x.pad=", 6);
        line[UPD_LINE_MAX] = '\n';
        line[UPD_LINE_MAX + 1] = '\0';
        snprintf(body, sizeof(body), "%s%s%s", HEAD, line, L_CFULL);
        REFUSE(body, UPD_OK);
    }
}

static void test_truncated(void)
{
    const char   *t = with_end(GOOD);
    unsigned long n = (unsigned long)strlen(t);
    UpdWant       w = want_rc4();
    UpdManifest   m;
    unsigned long cut;
    int           bad = 0;

    CHECK(parse_raw(t, n - 1, &w, &m) == UPD_E_TRUNCATED);  /* no LF      */
    CHECK(parse_raw(t, 0, &w, &m) == UPD_E_TRUNCATED);
    CHECK(parse(GOOD) == UPD_E_TRUNCATED);                  /* no end=    */

    /* Every cut short of the whole is refused, whatever it is refused as. */
    for (cut = 0; cut < n; cut++)
    {
        if (parse_raw(t, cut, &w, &m) == UPD_OK)
            bad++;
    }
    CHECK(bad == 0);
}

static void test_lines(void)
{
    char line[UPD_LINE_MAX + 3];
    char body[2048];

    memset(line, 'a', sizeof(line));
    memcpy(line, "x.pad=", 6);
    line[UPD_LINE_MAX + 1] = '\n';
    line[UPD_LINE_MAX + 2] = '\0';
    snprintf(body, sizeof(body), "%s%s%s", HEAD, line, L_CFULL);
    REFUSE(body, UPD_E_LINE_LONG);

    /* Long and never ending is too long, not truncated. */
    line[UPD_LINE_MAX + 1] = 'a';
    snprintf(body, sizeof(body), "%s%s", HEAD, line);
    CHECK(parse(body) == UPD_E_LINE_LONG);

    REFUSE(HEAD "version\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "Product=x\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "x.a=b\r\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "x.a=b\tc\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "x.a=\xe9\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "file=full LIBS 12 " H1 "\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "file=full  LIBS 12 " H1 " x\n" L_CFULL, UPD_E_SYNTAX);
    REFUSE(HEAD "container=full 12 " H1 "\n", UPD_E_SYNTAX);
    /* Nothing after end=. */
    with_end(GOOD);
    strcat(buf, "x.a=b\n");
    CHECK(parse(buf) == UPD_E_SYNTAX);
}

static void test_records(void)
{
    /* format */
    REFUSE(L_PRODUCT L_FORMAT L_VERSION L_CHANNEL L_CFULL, UPD_E_FORMAT);
    REFUSE("format=2\n" L_PRODUCT L_VERSION L_CHANNEL L_CFULL, UPD_E_FORMAT);
    REFUSE("# lead\n" L_FORMAT L_PRODUCT L_VERSION L_CHANNEL L_CFULL,
           UPD_OK);
    CHECK(parse("end=0\n") == UPD_E_FORMAT);

    /* duplicates */
    REFUSE(HEAD L_FORMAT L_CFULL, UPD_E_DUPLICATE);
    REFUSE(HEAD L_PRODUCT L_CFULL, UPD_E_DUPLICATE);
    REFUSE(HEAD L_VERSION L_CFULL, UPD_E_DUPLICATE);
    REFUSE(HEAD L_CHANNEL L_CFULL, UPD_E_DUPLICATE);
    REFUSE(HEAD L_CERTS L_CFULL, UPD_E_DUPLICATE);
    REFUSE(HEAD L_CFULL L_CFULL, UPD_E_DUPLICATE);
    REFUSE(HEAD L_CFULL L_F1 L_F1, UPD_E_DUPLICATE);
    REFUSE(HEAD L_CFULL L_F1 "file=full LIBS 1 " H2 " BSDSocket.Library\n",
           UPD_E_DUPLICATE);
    /* The same name under another destination is another file. */
    REFUSE(HEAD L_CFULL L_F1 "file=full C 1 " H2 " bsdsocket.library\n",
           UPD_OK);

    /* missing */
    REFUSE(L_FORMAT L_PRODUCT L_CHANNEL L_CFULL, UPD_E_MISSING);
    REFUSE(L_FORMAT L_VERSION L_CHANNEL L_CFULL, UPD_E_MISSING);
    REFUSE(L_FORMAT L_PRODUCT L_VERSION L_CFULL, UPD_E_MISSING);
    REFUSE(L_FORMAT L_PRODUCT L_VERSION L_CHANNEL L_CFULL L_F4,
           UPD_E_MISSING);
    REFUSE(HEAD L_CFULL "file=fat C 1 " H1 " ping\n", UPD_E_MISSING);

    /* end= */
    {
        char t[4096];

        snprintf(t, sizeof(t), "%send=99\n", HEAD L_CFULL);
        CHECK(parse(t) == UPD_E_COUNT);
        snprintf(t, sizeof(t), "%send=x\n", HEAD L_CFULL);
        CHECK(parse(t) == UPD_E_NUMBER);
    }

    /* flavours and classes */
    /* Flavours this updater does not know are skipped, whatever follows. */
    REFUSE(HEAD L_CFULL "container=nano 1 " H1 " a.lha\n", UPD_OK);
    REFUSE(HEAD L_CFULL "file=Full LIBS 1 " H1 " a\n", UPD_OK);
    REFUSE(HEAD L_CFULL "file=nano\n", UPD_OK);
    REFUSE(HEAD L_CFULL "container=nano x y z ../:\n", UPD_OK);
    REFUSE(HEAD L_CFULL "file=full DEVS 1 " H1 " a\n", UPD_E_CLASS);
    REFUSE(HEAD L_CFULL "file=full libs 1 " H1 " a\n", UPD_E_CLASS);
    REFUSE(HEAD L_CFULL "file=full Networks 1 " H1 " a\n", UPD_E_CLASS);

    /* hex */
    REFUSE(HEAD L_CFULL "file=full C 1 " H1 "0 a\n", UPD_E_HEX);
    REFUSE(HEAD L_CFULL "file=full C 1 "
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde"
           " a\n", UPD_E_HEX);
    REFUSE(HEAD L_CFULL "file=full C 1 "
           "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
           " a\n", UPD_E_HEX);
    REFUSE(L_FORMAT L_PRODUCT L_VERSION L_CHANNEL "certificates.prev=00\n"
           L_CFULL, UPD_E_HEX);
    REFUSE(HEAD "container=full 1 xyz a.lha\n", UPD_E_HEX);

    /* numbers */
    REFUSE(HEAD L_CFULL "file=full C 012 " H1 " a\n", UPD_E_NUMBER);
    REFUSE(HEAD L_CFULL "file=full C 4294967296 " H1 " a\n", UPD_E_NUMBER);
    REFUSE(HEAD L_CFULL "file=full C 4294967295 " H1 " a\n", UPD_OK);
    REFUSE(HEAD L_CFULL "file=full C -1 " H1 " a\n", UPD_E_NUMBER);
    REFUSE(HEAD "container=full 1k " H1 " a.lha\n", UPD_E_NUMBER);

    /* versions */
    REFUSE(L_FORMAT L_PRODUCT "version=1.0\n" L_CHANNEL L_CFULL,
           UPD_E_VERSION);
    REFUSE(L_FORMAT L_PRODUCT "version=1.0.0-rc05\n" L_CHANNEL L_CFULL,
           UPD_E_VERSION);
    REFUSE(L_FORMAT L_PRODUCT "version=\n" L_CHANNEL L_CFULL, UPD_E_VERSION);
}

/* A later release: a flavour and a class this updater does not know. */
#define FUTURE                                                               \
    HEAD L_CFULL L_CMICRO                                                    \
    "container=minimal 2 " H1 " min.lha\n"                                   \
    "container=fat 3 " H1 " fat.lha\n"                                       \
    "container=future 4 " H1 " future.lha\n"                                 \
    "file=future LIBS 1 " H1 " bsdsocket.library\n"                          \
    "file=future SYS 1 " H1 " x\n"                                           \
    "file=future some later shape\n"                                         \
    L_F1 L_FM

static void test_future(void)
{
    UpdWant      w = want_rc4();
    unsigned int f;

    for (f = 0; f < UPD_FLAVOUR_COUNT; f++)
    {
        w.flavour = (unsigned char)f;
        if (parse_w(with_end(FUTURE), &w) != UPD_OK)
        {
            failures++;
            printf("  FAIL future manifest refused for %s\n",
                   upd_flavour_name(f));
        }
        checks++;
    }

    /* An unknown class: skipped in another flavour, refused in ours. */
    w = want_rc4();
    CHECK(parse_w(with_end(HEAD L_CFULL L_CMICRO
                           "file=micro SYS 1 " H1 " x\n"), &w) == UPD_OK);
    w.flavour = UPD_FLAVOUR_MICRO;
    CHECK(parse_w(with_end(HEAD L_CFULL L_CMICRO
                           "file=micro SYS 1 " H1 " x\n"), &w) ==
          UPD_E_CLASS);
}

static void test_paths(void)
{
    static const char *bad[] = {
        "../x", "a/../b", "a/..", "..", ".", "./a", "/a", "a/", "a//b",
        "DEVS:x", "x:", ":x", " a", "a ",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        NULL
    };
    static const char *good[] = {
        "a", "a/b", "a..b", "...", ".a", "a b/c", "Libs/x.library",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        NULL
    };
    static const char *badname[] = {
        "a/b.lha", "a b.lha", "x:y.lha", "..", "/a.lha",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        NULL
    };
    char body[2048];
    int  i;

    for (i = 0; bad[i] != NULL; i++)
    {
        snprintf(body, sizeof(body), "%sfile=full C 1 %s %s\n",
                 HEAD L_CFULL, H1, bad[i]);
        if (parse(with_end(body)) != UPD_E_PATH)
        {
            failures++;
            printf("  FAIL path \"%s\" not refused\n", bad[i]);
        }
        checks++;
    }

    for (i = 0; good[i] != NULL; i++)
    {
        snprintf(body, sizeof(body), "%sfile=full C 1 %s %s\n",
                 HEAD L_CFULL, H1, good[i]);
        if (parse(with_end(body)) != UPD_OK)
        {
            failures++;
            printf("  FAIL path \"%s\" refused\n", good[i]);
        }
        checks++;
    }

    for (i = 0; badname[i] != NULL; i++)
    {
        snprintf(body, sizeof(body), "%scontainer=full 1 %s %s\n",
                 HEAD, H1, badname[i]);
        if (parse(with_end(body)) != UPD_E_PATH)
        {
            failures++;
            printf("  FAIL container name \"%s\" not refused\n", badname[i]);
        }
        checks++;
    }
}

static void test_refusals(void)
{
    UpdWant w = want_rc4();

    REFUSE(L_FORMAT "product=AmiTCP\n" L_VERSION L_CHANNEL L_CFULL,
           UPD_E_PRODUCT);
    REFUSE(L_FORMAT "product=amINETxduo\n" L_VERSION L_CHANNEL L_CFULL,
           UPD_E_PRODUCT);
    REFUSE(L_FORMAT L_PRODUCT L_VERSION "channel=release\n" L_CFULL,
           UPD_E_CHANNEL);
    REFUSE(L_FORMAT L_PRODUCT L_VERSION "channel=nightly\n" L_CFULL,
           UPD_E_CHANNEL);

    /* A release channel never carries a prerelease. */
    w.channel = UPD_CHANNEL_RELEASE;
    CHECK(parse_w(with_end(L_FORMAT L_PRODUCT L_VERSION "channel=release\n"
                           L_CFULL), &w) == UPD_E_CHANNEL);
    CHECK(parse_w(with_end(L_FORMAT L_PRODUCT "version=1.0.0\n"
                           "channel=release\n" L_CFULL), &w) == UPD_OK);

    /* The same version, and an older one. */
    w = want_rc4();
    upd_ver_parse("1.0.0-rc5", 9, &w.installed);
    CHECK(parse_w(with_end(GOOD), &w) == UPD_E_NOT_NEWER);
    upd_ver_parse("1.0.0", 5, &w.installed);
    CHECK(parse_w(with_end(GOOD), &w) == UPD_E_NOT_NEWER);
    upd_ver_parse("1.0.0-rc10", 10, &w.installed);
    CHECK(parse_w(with_end(GOOD), &w) == UPD_E_NOT_NEWER);

    /* Nothing for the installed flavour. */
    w = want_rc4();
    w.flavour = UPD_FLAVOUR_FAT;
    CHECK(parse_w(with_end(GOOD), &w) == UPD_E_NO_FLAVOUR);
    w.flavour = UPD_FLAVOUR_MINIMAL;
    CHECK(parse_w(with_end(GOOD), &w) == UPD_E_NO_FLAVOUR);

    /* Syntax first: a malformed manifest for another product is malformed. */
    REFUSE(L_FORMAT "product=AmiTCP\n" L_VERSION L_CHANNEL L_CFULL
           "file=full C 1 " H1 " ../x\n", UPD_E_PATH);
}

static void test_limits(void)
{
    UpdWant     w = want_rc4();
    UpdManifest m;
    const char *t = with_end(GOOD);
    unsigned long n = (unsigned long)strlen(t);

    CHECK(upd_manifest_parse(t, n, &w, &m, files, 3) == UPD_E_TOO_MANY);
    CHECK(upd_manifest_parse(t, n, &w, &m, files, 4) == UPD_OK);
    CHECK(upd_manifest_parse(t, n, &w, &m, NULL, 0) == UPD_E_TOO_MANY);
    CHECK(upd_manifest_parse(t, n, &w, &m, NULL, 4) == UPD_E_ARG);
    CHECK(upd_manifest_parse(NULL, 0, &w, &m, files, 4) == UPD_E_ARG);
    CHECK(upd_manifest_parse(t, n, NULL, &m, files, 4) == UPD_E_ARG);
    w.flavour = UPD_FLAVOUR_COUNT;
    CHECK(upd_manifest_parse(t, n, &w, &m, files, 4) == UPD_E_ARG);
}

/* ---------------------------------------------------- the signature file */

#define S128 H1 H2

static UpdResult sig(const char *t, UpdSigLine *l, unsigned int *n)
{
    return upd_sig_parse(t, (unsigned long)strlen(t), l, n);
}

static void test_sigfile(void)
{
    UpdSigLine   l[UPD_SIG_LINES];
    unsigned int n;

    CHECK(sig("key=a1 sig=" S128 "\n", l, &n) == UPD_OK && n == 1 &&
          strcmp(l[0].id, "a1") == 0 && l[0].sig[0] == 0x01 &&
          l[0].sig[63] == 0x10);
    CHECK(sig("key=a sig=" S128 "\nkey=b-2 sig=" S128 "\n", l, &n) ==
          UPD_OK && n == 2 && strcmp(l[1].id, "b-2") == 0);
    CHECK(sig("key=0123456789abcdef sig=" S128 "\n", l, &n) == UPD_OK);

    CHECK(sig("", l, &n) == UPD_E_TRUNCATED);
    CHECK(sig("key=a sig=" S128, l, &n) == UPD_E_TRUNCATED);
    CHECK(sig("key=a sig=" S128 "\n\n", l, &n) == UPD_E_SYNTAX);
    CHECK(sig("key= sig=" S128 "\n", l, &n) == UPD_E_SYNTAX);
    CHECK(sig("key=A sig=" S128 "\n", l, &n) == UPD_E_SYNTAX);
    CHECK(sig("key=0123456789abcdefg sig=" S128 "\n", l, &n) ==
          UPD_E_SYNTAX);
    CHECK(sig("key=a  sig=" S128 "\n", l, &n) == UPD_E_SYNTAX);
    CHECK(sig("sig=" S128 " key=a\n", l, &n) == UPD_E_SYNTAX);
    CHECK(sig("key=a sig=" S128 "\r\n", l, &n) == UPD_E_SYNTAX);
    CHECK(sig("key=a sig=" H1 "\n", l, &n) == UPD_E_HEX);
    CHECK(sig("key=a sig=" S128 "0\n", l, &n) == UPD_E_HEX);
    CHECK(sig("key=a sig=" S128 " \n", l, &n) == UPD_E_HEX);
    CHECK(sig("key=a sig=" S128 "\nkey=a sig=" S128 "\n", l, &n) ==
          UPD_E_DUPLICATE);
    CHECK(sig("key=a sig=" S128 "\nkey=b sig=" S128 "\nkey=c sig=" S128
              "\nkey=d sig=" S128 "\nkey=e sig=" S128 "\n", l, &n) ==
          UPD_E_TOO_MANY);
}

static void test_words(void)
{
    int r;

    for (r = 0; r < UPD_RESULT_COUNT; r++)
        CHECK(upd_result_text((UpdResult)r) != NULL);
    CHECK(upd_result_text(UPD_RESULT_COUNT) == NULL);
    CHECK(strcmp(upd_flavour_name(UPD_FLAVOUR_FAT), "fat") == 0);
    CHECK(strcmp(upd_class_name(UPD_CLASS_NETWORKS), "DEVS/Networks") == 0);
    CHECK(strcmp(upd_channel_name(UPD_CHANNEL_PRERELEASE), "prerelease") ==
          0);
    CHECK(upd_flavour_name(UPD_FLAVOUR_COUNT) == NULL);
}

int main(void)
{
    test_good();
    test_truncated();
    test_lines();
    test_records();
    test_future();
    test_paths();
    test_refusals();
    test_limits();
    test_sigfile();
    test_words();

    printf("test_updmanifest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
