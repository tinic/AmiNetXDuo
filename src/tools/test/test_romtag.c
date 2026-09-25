/*
 * romtag_name() on built load files, and on any real ones named on the
 * command line as path=expected pairs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "romtag.h"

static int failures;

static void check(const char *what, BOOL got, BOOL want, const char *name,
                  const char *expect)
{
    if (got != want || (want && strcmp(name, expect) != 0))
    {
        printf("FAIL %s: got %d \"%s\", want %d \"%s\"\n", what, (int)got,
               name, (int)want, want ? expect : "");
        failures++;
    }
    else
        printf("ok   %s\n", what);
}

static size_t put_long(unsigned char *b, size_t at, unsigned long v)
{
    b[at]     = (unsigned char)(v >> 24);
    b[at + 1] = (unsigned char)(v >> 16);
    b[at + 2] = (unsigned char)(v >> 8);
    b[at + 3] = (unsigned char)v;
    return at + 4;
}

/*
 * One code hunk holding a RomTag at `tag` and the name at `name_off`, in the
 * code hunk or, when `name_in_data`, at the same offset in a second data hunk.
 * `short_relocs` writes HUNK_RELOC32SHORT instead of HUNK_RELOC32.
 */
static size_t build(unsigned char *b, unsigned long tag, unsigned long name_off,
                    int name_in_data, int short_relocs, int with_symbols)
{
    const char   *name  = "x-surf-100.device";
    unsigned long longs = 32;               /* 128-byte hunks */
    unsigned long hunks = name_in_data ? 2 : 1;
    unsigned char code[128];
    unsigned char data[128];
    size_t        at = 0;

    memset(code, 0, sizeof(code));
    memset(data, 0, sizeof(data));

    code[tag]     = 0x4A;
    code[tag + 1] = 0xFC;
    put_long(code, tag + 2, tag);           /* rt_MatchTag, self */
    put_long(code, tag + 6, 128);           /* rt_EndSkip */
    put_long(code, tag + 14, name_off);     /* rt_Name */
    memcpy(name_in_data ? data + name_off : code + name_off, name,
           strlen(name) + 1);

    at = put_long(b, at, 0x3F3);
    at = put_long(b, at, 0);
    at = put_long(b, at, hunks);
    at = put_long(b, at, 0);
    at = put_long(b, at, hunks - 1);
    at = put_long(b, at, longs);
    if (name_in_data)
        at = put_long(b, at, longs | 0x40000000UL);    /* MEMF_CHIP bit */

    at = put_long(b, at, 0x3E9);
    at = put_long(b, at, longs);
    memcpy(b + at, code, 128);
    at += 128;

    if (short_relocs)
    {
        at = put_long(b, at, 0x3FC);
        b[at++] = 0; b[at++] = 1;           /* one offset */
        b[at++] = 0; b[at++] = 0;           /* into hunk 0 */
        b[at++] = 0; b[at++] = (unsigned char)(tag + 2);
        b[at++] = 0; b[at++] = 1;
        b[at++] = 0; b[at++] = (unsigned char)(name_in_data ? 1 : 0);
        b[at++] = 0; b[at++] = (unsigned char)(tag + 14);
        b[at++] = 0; b[at++] = 0;           /* end */
        while (at & 3)
            b[at++] = 0;
    }
    else
    {
        at = put_long(b, at, 0x3EC);
        at = put_long(b, at, 1);
        at = put_long(b, at, 0);
        at = put_long(b, at, tag + 2);
        at = put_long(b, at, 1);
        at = put_long(b, at, name_in_data ? 1 : 0);
        at = put_long(b, at, tag + 14);
        at = put_long(b, at, 0);
    }

    if (with_symbols)
    {
        at = put_long(b, at, 0x3F0);
        at = put_long(b, at, 1);            /* one-long name */
        memcpy(b + at, "_ini", 4);
        at += 4;
        at = put_long(b, at, 0);            /* value */
        at = put_long(b, at, 0);
        at = put_long(b, at, 0x3F1);
        at = put_long(b, at, 1);
        at = put_long(b, at, 0xDEADBEEF);
    }
    at = put_long(b, at, 0x3F2);

    if (name_in_data)
    {
        at = put_long(b, at, 0x3EA);
        at = put_long(b, at, longs);
        memcpy(b + at, data, 128);
        at += 128;
        at = put_long(b, at, 0x3F2);
    }

    return at;
}

static unsigned char *slurp(const char *path, unsigned long *len)
{
    FILE          *f = fopen(path, "rb");
    unsigned char *buf;
    long           n;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (unsigned long)n;
    return buf;
}

int main(int argc, char **argv)
{
    unsigned char b[1024];
    char          name[64];
    size_t        n;
    int           a;

    n = build(b, 4, 40, 0, 0, 0);
    check("code hunk, RELOC32", romtag_name(b, n, name, sizeof(name)), TRUE,
          name, "x-surf-100.device");

    n = build(b, 4, 40, 1, 0, 1);
    check("name in data hunk, symbols and debug",
          romtag_name(b, n, name, sizeof(name)), TRUE, name,
          "x-surf-100.device");

    n = build(b, 8, 60, 0, 1, 0);
    check("RELOC32SHORT", romtag_name(b, n, name, sizeof(name)), TRUE, name,
          "x-surf-100.device");

    n = build(b, 4, 40, 0, 0, 0);
    check("cut to the buffer", romtag_name(b, n, name, 7), TRUE, name,
          "x-surf");

    n = build(b, 4, 40, 0, 0, 0);
    b[20 + 4 + 8 + 4 + 2] ^= 0xFF;          /* break rt_MatchTag */
    check("no self-matching tag", romtag_name(b, n, name, sizeof(name)), FALSE,
          name, "");

    n = build(b, 4, 40, 0, 0, 0);
    check("truncated file", romtag_name(b, n - 60, name, sizeof(name)), FALSE,
          name, "");

    n = build(b, 4, 40, 0, 0, 0);
    b[3] = 0xF4;                            /* not HUNK_HEADER */
    check("not a load file", romtag_name(b, n, name, sizeof(name)), FALSE,
          name, "");

    /* path=expected, for real drivers on the machine running the test. */
    for (a = 1; a < argc; a++)
    {
        char          *eq = strchr(argv[a], '=');
        unsigned char *file;
        unsigned long  len = 0;

        if (eq == NULL)
            continue;
        *eq  = '\0';
        file = slurp(argv[a], &len);
        if (file == NULL)
        {
            printf("FAIL cannot read %s\n", argv[a]);
            failures++;
            continue;
        }
        check(argv[a], romtag_name(file, len, name, sizeof(name)), TRUE, name,
              eq + 1);
        free(file);
    }

    printf("romtag failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
