/*
 * The AmigaDOS Command Reference contract, host side.
 *
 *   quiet      tool_quiet_rc() maps as the reference says, and every command
 *              whose reference QUIET is Roadshow's routes its exit through it.
 *   reference  every source carrying REFERENCE_TEMPLATE accepts that template:
 *              each reference item is one of ours with all its aliases and the
 *              same modifiers, and our positional items start with the
 *              reference's, in its order.  Extras are allowed.
 *
 * Sources are parsed, not compiled: they reach proto/dos.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toolquiet.h"
#include "addif_wb.h"

#ifndef AMINETXDUO_SOURCE_DIR
#define AMINETXDUO_SOURCE_DIR "."
#endif

static int failures;
static int checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  n;
    char *buf;

    if (fp == NULL)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = malloc((size_t)n + 1);
    if (buf != NULL && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf != NULL)
        buf[n] = '\0';
    fclose(fp);
    return buf;
}

static char *source(const char *name)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/src/tools/%s", AMINETXDUO_SOURCE_DIR,
             name);
    return slurp(path);
}

/* ---------------------------------------------------------------- quiet --- */

/* The reference documents Roadshow's QUIET for AddNetInterface, AddNetRoute,
   DeleteNetRoute (both addnetroute.c) and ConfigureNetInterface.
   RemoveNetInterface is their sibling; its reference page is empty. */
static const char *const quiet_commands[] = {
    "addnetinterface.c", "addnetroute.c", "configurenetinterface.c",
    "removenetinterface.c",
};

static void check_quiet(void)
{
    size_t i;

    CHECK(tool_quiet_rc(0, 0) == 0, "no QUIET, 0");
    CHECK(tool_quiet_rc(0, 10) == 10, "no QUIET, 10 kept");
    CHECK(tool_quiet_rc(0, 20) == 20, "no QUIET, 20 kept");
    CHECK(tool_quiet_rc(1, 0) == 0, "QUIET, success stays 0");
    CHECK(tool_quiet_rc(1, 5) == 5, "QUIET, WARN stays 5");
    CHECK(tool_quiet_rc(1, 10) == 5, "QUIET, ERROR becomes 5");
    CHECK(tool_quiet_rc(1, 20) == 5, "QUIET, FAIL becomes 5");

    for (i = 0; i < sizeof(quiet_commands) / sizeof(quiet_commands[0]); i++)
    {
        char *src = source(quiet_commands[i]);

        CHECK(src != NULL, "%s: cannot read", quiet_commands[i]);
        if (src == NULL)
            continue;
        CHECK(strstr(src, "tool_quiet_begin();") != NULL,
              "%s: QUIET does not call tool_quiet_begin()", quiet_commands[i]);
        CHECK(strstr(src, "tool_quiet_end(") != NULL,
              "%s: main() does not return through tool_quiet_end()",
              quiet_commands[i]);
        free(src);
    }
}

/* ------------------------------------------------------------ templates --- */

#define MAX_ITEMS   40
#define MAX_ALIAS   4

typedef struct Item
{
    char alias[MAX_ALIAS][32];
    int  naliases;
    char mods[8];       /* sorted, uppercase */
} Item;

/* `#define NAME "a," "b"` over continued lines, quoted runs joined. */
static int read_define(const char *src, const char *name, int nth, char *out,
                       size_t outlen)
{
    char        needle[64];
    const char *p = src;
    size_t      n = 0;

    snprintf(needle, sizeof(needle), "#define %s ", name);
    while ((p = strstr(p, needle)) != NULL && nth-- > 0)
        p += strlen(needle);
    if (p == NULL)
        return 0;

    for (;;)
    {
        const char *eol = strchr(p, '\n');
        const char *q = p;
        const char *b;

        if (eol == NULL)
            eol = p + strlen(p);
        while ((q = memchr(q, '"', (size_t)(eol - q))) != NULL)
        {
            const char *end = ++q;

            while (end < eol && *end != '"')
                end++;
            if (end >= eol)
                break;
            while (q < end && n + 1 < outlen)
                out[n++] = *q++;
            q = end + 1;
        }
        b = eol;
        while (b > p && (b[-1] == ' ' || b[-1] == '\t'))
            b--;
        if (*eol == '\0' || b == p || b[-1] != '\\')
            break;
        p = eol + 1;
    }
    out[n] = '\0';
    return n > 0;
}

static int cmpc(const void *a, const void *b)
{
    return *(const char *)a - *(const char *)b;
}

static int parse(const char *tmpl, Item *out)
{
    char *copy = strdup(tmpl);
    char *save;
    char *it;
    int   n = 0;

    for (it = strtok_r(copy, ",", &save); it != NULL && n < MAX_ITEMS;
         it = strtok_r(NULL, ",", &save))
    {
        Item *x = &out[n++];
        char *slash = strchr(it, '/');
        char *asave;
        char *a;
        size_t m = 0;

        memset(x, 0, sizeof(*x));
        if (slash != NULL)
        {
            char *q;

            for (q = slash; *q != '\0' && m + 1 < sizeof(x->mods); q++)
                if (*q != '/')
                    x->mods[m++] = (char)toupper((unsigned char)*q);
            *slash = '\0';
            qsort(x->mods, m, 1, cmpc);
        }
        for (a = strtok_r(it, "=", &asave); a != NULL && x->naliases < MAX_ALIAS;
             a = strtok_r(NULL, "=", &asave))
        {
            char *c;

            for (c = a; *c != '\0'; c++)
                *c = (char)toupper((unsigned char)*c);
            snprintf(x->alias[x->naliases++], sizeof(x->alias[0]), "%s", a);
        }
    }
    free(copy);
    return n;
}

static int has_alias(const Item *x, const char *a)
{
    int i;

    for (i = 0; i < x->naliases; i++)
        if (strcmp(x->alias[i], a) == 0)
            return 1;
    return 0;
}

/* Filled by position: anything that is not /K, /S or /T. */
static int positional(const Item *x)
{
    return strchr(x->mods, 'K') == NULL && strchr(x->mods, 'S') == NULL &&
           strchr(x->mods, 'T') == NULL;
}

static void compare(const char *file, int pair, const char *ref,
                    const char *ours)
{
    Item r[MAX_ITEMS];
    Item o[MAX_ITEMS];
    int  nr = parse(ref, r);
    int  no = parse(ours, o);
    int  i;
    int  j;
    int  rp = 0;
    int  op = 0;

    for (i = 0; i < nr; i++)
    {
        int found = -1;
        int a;

        for (j = 0; j < no && found < 0; j++)
            if (has_alias(&o[j], r[i].alias[0]))
                found = j;

        CHECK(found >= 0, "%s[%d]: reference keyword %s is missing", file,
              pair, r[i].alias[0]);
        if (found < 0)
            continue;

        for (a = 1; a < r[i].naliases; a++)
            CHECK(has_alias(&o[found], r[i].alias[a]),
                  "%s[%d]: %s lacks the reference alias %s", file, pair,
                  r[i].alias[0], r[i].alias[a]);

        CHECK(strcmp(r[i].mods, o[found].mods) == 0,
              "%s[%d]: %s is /%s, the reference has /%s", file, pair,
              r[i].alias[0], o[found].mods, r[i].mods);
    }

    /* The reference's positional items lead ours, in its order. */
    for (i = 0; i < nr; i++)
    {
        if (!positional(&r[i]))
            continue;
        while (op < no && !positional(&o[op]))
            op++;
        CHECK(op < no && has_alias(&o[op], r[i].alias[0]),
              "%s[%d]: positional item %d is %s, the reference has %s", file,
              pair, rp, (op < no) ? o[op].alias[0] : "nothing",
              r[i].alias[0]);
        rp++;
        op++;
    }
}

static int carried;

static void check_file(const char *file)
{
    char *src = source(file);
    char  ref[1024];
    char  ours[1024];
    int   nth;

    if (src == NULL)
        return;

    for (nth = 0; read_define(src, "REFERENCE_TEMPLATE", nth, ref,
                              sizeof(ref)); nth++)
    {
        /* The n-th reference goes with the n-th TEMPLATE: addnetroute.c is
           two commands. */
        CHECK(read_define(src, "TEMPLATE", nth, ours, sizeof(ours)),
              "%s[%d]: REFERENCE_TEMPLATE with no TEMPLATE", file, nth);
        compare(file, nth, ref, ours);
        carried++;
    }

    free(src);
}

static void check_reference(void)
{
    char           dir[512];
    DIR           *d;
    struct dirent *e;

    snprintf(dir, sizeof(dir), "%s/src/tools", AMINETXDUO_SOURCE_DIR);
    d = opendir(dir);
    CHECK(d != NULL, "cannot open %s", dir);
    if (d == NULL)
        return;

    while ((e = readdir(d)) != NULL)
    {
        size_t n = strlen(e->d_name);

        if (n > 2 && strcmp(e->d_name + n - 2, ".c") == 0)
            check_file(e->d_name);
    }
    closedir(d);

    /* Never fewer than this change introduced. */
    CHECK(carried >= 3, "%d reference templates found", carried);
}

/* ---------------------------------------------------------------- addif --- */

static void check_addif(void)
{
    long v = 99;
    char a[32];
    char b[32];

    CHECK(addif_seconds("60", &v) && v == 60, "TIMEOUT=60 reads 60");
    CHECK(addif_seconds("5", &v) && v == 5,
          "TIMEOUT=5 reads 5; the floor of 10 is the caller's, as for the Shell");
    CHECK(addif_seconds("-1", &v) && v == -1, "a negative value is read, to be refused");
    CHECK(!addif_seconds("", &v), "an empty TIMEOUT is refused");
    CHECK(!addif_seconds("10s", &v), "TIMEOUT=10s is refused, not read as 10");
    CHECK(!addif_seconds("abc", &v), "TIMEOUT=abc is refused");
    CHECK(!addif_seconds("-", &v), "a lone sign is refused");
    CHECK(!addif_seconds("99999999999", &v), "past a LONG is refused");

    addif_log_name(a, sizeof(a), 0x0012a4c0UL);
    addif_log_name(b, sizeof(b), 0x0012a5c0UL);
    CHECK(strcmp(a, "T:AddNetInterface.0012a4c0") == 0, "the log is named by the process: %s", a);
    CHECK(strcmp(a, b) != 0, "and two launches do not share one");
}

int main(int argc, char **argv)
{
    const char *group = (argc > 1) ? argv[1] : NULL;

    if (group == NULL || strcmp(group, "quiet") == 0)
        check_quiet();
    if (group == NULL || strcmp(group, "reference") == 0)
        check_reference();
    if (group == NULL || strcmp(group, "addif") == 0)
        check_addif();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
