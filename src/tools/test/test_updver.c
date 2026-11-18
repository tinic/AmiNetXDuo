/*
 * The tests for src/tools/updver.c: which version strings are accepted, and
 * their order.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updver.h"

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

static int parse(const char *s, UpdVer *v)
{
    return upd_ver_parse(s, (unsigned long)strlen(s), v);
}

static void test_accepts(void)
{
    static const char *good[] = {
        "0.0.0", "1.0.0", "1.0.0-rc4", "1.0.0-beta8", "1.0.0-rc10",
        "10.20.30", "99999.0.0", "1.0.0-0", "1.0.0-rc0", "1.0.0-rc.1",
        "1.0.0-RC1", "1.0.0-rc100", "1.0.0-a.b.c", "1.0.0-123456789012345",
        NULL
    };
    UpdVer v;
    char   back[UPD_VER_MAX];
    int    i;

    for (i = 0; good[i] != NULL; i++)
    {
        int ok = parse(good[i], &v);

        CHECK(ok);
        if (!ok)
        {
            printf("    refused \"%s\"\n", good[i]);
            continue;
        }

        upd_ver_format(&v, back);
        CHECK(strcmp(back, good[i]) == 0);
    }

    CHECK(parse("1.0.0-rc4", &v) && v.major == 1 && v.minor == 0 &&
          v.patch == 0 && strcmp(v.pre, "rc4") == 0);
}

static void test_refuses(void)
{
    static const char *bad[] = {
        "", "1", "1.0", "1.0.", "1.0.0.", "1.0.0.0", ".1.0.0", "1..0",
        "01.0.0", "1.00.0", "1.0.01", "100000.0.0", "1.0.0-", "1.0.0-rc01",
        "1.0.0-00", "1.0.0-rc.", "1.0.0-.rc", "1.0.0-rc..1", "1.0.0-rc_1",
        "1.0.0-rc 1", "1.0.0+nx6", "1.0.0-rc4+nx6.5.1", "v1.0.0", " 1.0.0",
        "1.0.0 ", "1.0.0-1234567890123456", "-1.0.0", "1.-1.0", "a.b.c",
        NULL
    };
    UpdVer v;
    int    i;

    for (i = 0; bad[i] != NULL; i++)
    {
        int ok = parse(bad[i], &v);

        CHECK(!ok);
        if (ok)
            printf("    accepted \"%s\"\n", bad[i]);
    }

    /* `len` is the bound, not a NUL. */
    CHECK(upd_ver_parse("1.0.0-rc4", 5, &v) && strcmp(v.pre, "") == 0 &&
          v.patch == 0);
    CHECK(!upd_ver_parse("1.0.0-rc4", 6, &v));
}

/* Each strictly older than the next. */
static void test_order(void)
{
    static const char *chain[] = {
        "0.9.0", "0.10.0", "1.0.0-0", "1.0.0-9", "1.0.0-10", "1.0.0-alpha",
        "1.0.0-beta", "1.0.0-beta1", "1.0.0-beta8", "1.0.0-beta9",
        "1.0.0-beta10", "1.0.0-rc", "1.0.0-rc1", "1.0.0-rc1.1",
        "1.0.0-rc4", "1.0.0-rc9", "1.0.0-rc10", "1.0.0-rc11", "1.0.0-rc99",
        "1.0.0-rc100", "1.0.0", "1.0.1-rc1", "1.0.1", "1.0.10", "1.1.0",
        "2.0.0-beta1", "2.0.0", "10.0.0",
        NULL
    };
    UpdVer v[32];
    int    n, i, j;

    for (n = 0; chain[n] != NULL; n++)
        CHECK(parse(chain[n], &v[n]));

    for (i = 0; i < n; i++)
    {
        for (j = 0; j < n; j++)
        {
            int c    = upd_ver_cmp(&v[i], &v[j]);
            int want = (i < j) ? -1 : (i > j) ? 1 : 0;

            if ((c < 0 ? -1 : c > 0 ? 1 : 0) != want)
            {
                failures++;
                printf("  FAIL %s vs %s gave %d\n", chain[i], chain[j], c);
            }
            checks++;
        }
    }
}

int main(void)
{
    test_accepts();
    test_refuses();
    test_order();

    printf("test_updver: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
