/*
 * Every command's ReadArgs template against the enum that indexes it.
 *
 * WHY THIS IS A TEST AND NOT A REVIEW
 *
 *   A command reaches its arguments by position: ReadArgs() fills an array in
 *   template order, and the code says args[ARG_TIMEOUT] to get one out.  The
 *   two agree only because somebody kept them in step, and nothing checks it.
 *
 *   Insert a keyword in the middle of a template without moving the enum and
 *   every argument after it shifts by one.  Nothing warns: the types are all
 *   LONG, the array is long enough, and the command runs.  QUIET starts
 *   reading the number that TIMEOUT was given, an /S switch reads a pointer
 *   and is true whenever the option before it was present.  On a machine with
 *   no memory protection, an /N read as a pointer is how a command takes the
 *   machine down instead of printing usage.
 *
 *   Thirty commands were rewritten wholesale in the 2026-08-04 diagnostics
 *   pass with nothing on the host able to catch a mistake in any of them, and
 *   the cross build only proves they compile.  Two of them have templates in
 *   two halves and one has three aliases on a single keyword, which is exactly
 *   where a hand check stops being reliable.
 *
 *   Host-side, and it parses the sources rather than including them, for the
 *   reason tests/sockopt/host/test_optnum_host.c gives: they need
 *   <exec/types.h> and proto/dos.h, and a table restated here would only ever
 *   agree with itself.
 *
 * WHAT IT CHECKS
 *
 *   For each command source holding a TEMPLATE and an ARG_ enum:
 *
 *     1. the template has as many comma-separated items as the enum has
 *        entries before its count sentinel;
 *     2. item N names the same thing as ARG_ entry N.
 *
 *   A template item is a set of `=`-separated aliases and then modifiers, and
 *   ReadArgs accepts any of them: `-c=COUNT` and `DNS=DOMAINNAMESERVERS` and
 *   `DEBUG=-d` all appear in this tree, with the enum naming sometimes the
 *   first and sometimes the second.  So entry N has to match SOME alias of
 *   item N, not a fixed one.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

/* ------------------------------------------------------------ the files --- */

/*
 * Named rather than globbed: a command that grows a template should be added
 * here deliberately, and a file that disappears should fail rather than
 * quietly reduce what is checked.
 */
static const char *const commands[] = {
    "addnetinterface", "addnetroute", "arp", "checknetconfig",
    "configurenetinterface", "fetch",
    "getnetstatus", "host", "hostname", "httpd", "nc", "netsetup",
    "netshutdown",
    "netstat", "nettrace", "nslookup", "onoff", "ping", "removenetinterface",
    "shownetservices",
    "shownetstatus", "sntp", "telnet", "tftp", "traceroute", "whois",
};

#define COMMAND_COUNT ((int)(sizeof(commands) / sizeof(commands[0])))

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  n;
    char *buf;

    if (fp == NULL)
        return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    rewind(fp);

    buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(fp); return NULL; }

    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

/* ---------------------------------------------------------- the template --- */

/*
 * `#define TEMPLATE "a,b," \` continued over lines.  Collect every quoted run
 * from the define to the first line that does not end in a backslash.
 */
static int read_template(const char *from, char *out, size_t outlen)
{
    const char *p = from;
    size_t      n = 0;

    if (p == NULL)
        return 0;

    for (;;)
    {
        const char *q = p;
        const char *eol = strchr(p, '\n');
        int         continued = 0;

        if (eol == NULL)
            eol = p + strlen(p);

        /* every "..." on this line */
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

        /* a trailing backslash means the define continues */
        {
            const char *b = eol;
            while (b > p && (b[-1] == ' ' || b[-1] == '\t'))
                b--;
            if (b > p && b[-1] == '\\')
                continued = 1;
        }

        if (!continued || *eol == '\0')
            break;
        p = eol + 1;
    }

    out[n] = '\0';
    return n > 0;
}

/*
 * Does `name` match any alias of this template item?  Modifiers are cut first,
 * then the remainder is split on '='.  A leading '-' is kept: `DEBUG=-d` has
 * aliases DEBUG and -D, and the enum names the first.
 */
static int item_has_alias(const char *item, const char *name, char *first,
                          size_t firstlen)
{
    char        buf[128];
    size_t      n = 0;
    const char *p = item;
    char       *save;
    char       *alias;
    int         found = 0;
    int         seen = 0;

    while (*p != '\0' && *p != '/' && n + 1 < sizeof(buf))
        buf[n++] = (char)toupper((unsigned char)*p++);
    buf[n] = '\0';

    for (alias = strtok_r(buf, "=", &save); alias != NULL;
         alias = strtok_r(NULL, "=", &save))
    {
        if (!seen) {
            snprintf(first, firstlen, "%s", alias);
            seen = 1;
        }
        if (strcmp(alias, name) == 0)
            found = 1;
    }

    return found;
}

/* -------------------------------------------------------------- the enum --- */

/*
 * The name that sizes the argument array, out of `LONG args[ARG_XXX]`.  That
 * entry is a sentinel and is not an argument.
 *
 * Read from the declaration rather than from a list of likely names, because
 * no list works: nslookup, onoff and whois size theirs with ARG_COUNT, and
 * ping has ARG_COUNT as a real argument for -c=COUNT with ARG_ARGCOUNT as its
 * sentinel.  How the code uses the name is the only thing that tells them
 * apart.
 */
static int sentinel_of(const char *src, char *out, size_t outlen)
{
    const char *p = src;

    /*
     * The DECLARATION, not a use.  addnetroute reads args[ARG_DST] hundreds of
     * lines before anything declares the array, and taking the first
     * `args[ARG_` in the file made ARG_DST the sentinel and cut its enum to one
     * entry.  A declaration carries the type on the same line.
     */
    while ((p = strstr(p, "args[ARG_")) != NULL)
    {
        const char *bol = p;
        const char *q;
        size_t      n = 0;

        while (bol > src && bol[-1] != '\n')
            bol--;

        if (strstr(bol, "LONG") == NULL || strstr(bol, "LONG") > p) {
            p += 9;
            continue;
        }

        q = p + strlen("args[ARG_");
        while (*q != '\0' && *q != ']' && n + 1 < outlen)
            out[n++] = *q++;
        out[n] = '\0';

        return n > 0;
    }

    return 0;
}

/*
 * The ARG_ enum after the template, minus the sentinel above.
 */
static int read_args(const char *src, const char *from, char names[][64],
                     int max)
{
    const char *p = from;
    char        sentinel[64];
    int         n = 0;

    if (p == NULL)
        return 0;

    if (!sentinel_of(src, sentinel, sizeof(sentinel)))
        sentinel[0] = '\0';

    p = strstr(p, "enum");
    if (p == NULL)
        return 0;

    while (n < max)
    {
        const char *a = strstr(p, "ARG_");
        const char *q;
        size_t      len = 0;
        char        buf[64];

        if (a == NULL)
            break;

        q = a + 4;
        while (*q != '\0' && (isalnum((unsigned char)*q) || *q == '_') &&
               len + 1 < sizeof(buf))
            buf[len++] = *q++;
        buf[len] = '\0';

        /* the closing brace ends the enum */
        {
            const char *brace = strchr(p, '}');
            if (brace != NULL && a > brace)
                break;
        }

        if (sentinel[0] != '\0' && strcmp(buf, sentinel) == 0)
            break;

        snprintf(names[n], 64, "%s", buf);
        n++;
        p = q;
    }

    return n;
}

/* ------------------------------------------------------------------ test --- */

/*
 * Every template/enum pair in the file, not just the first.  addnetroute.c is
 * two commands behind #ifdef TOOL_DELETE -- DeleteNetRoute with three
 * arguments and AddNetRoute with six -- and each carries its own pair.
 * Checking only the first left the six-argument half unchecked.
 */
static void check_pair(const char *name, int pair, const char *src,
                       const char *from)
{
    char  tmpl[1024];
    char  args[64][64];
    int   nargs;
    int   nitems = 0;
    char *save;
    char *item;
    char *copy;

    CHECK(read_template(from, tmpl, sizeof(tmpl)),
          "%s[%d]: no TEMPLATE text", name, pair);
    if (tmpl[0] == '\0')
        return;

    nargs = read_args(src, from, args, 64);
    CHECK(nargs > 0, "%s[%d]: no ARG_ enum after the template", name, pair);
    if (nargs == 0)
        return;

    copy = strdup(tmpl);
    if (copy == NULL)
        return;

    for (item = strtok_r(copy, ",", &save); item != NULL;
         item = strtok_r(NULL, ",", &save))
    {
        char first[64];

        if (nitems < nargs)
        {
            CHECK(item_has_alias(item, args[nitems], first, sizeof(first)),
                  "%s[%d]: template item %d (%s) has no alias %s, "
                  "which is what ARG_%s indexes",
                  name, pair, nitems, first, args[nitems], args[nitems]);
        }
        nitems++;
    }

    CHECK(nitems == nargs,
          "%s[%d]: template has %d items, enum has %d entries",
          name, pair, nitems, nargs);

    free(copy);
}

static void check_command(const char *name)
{
    char        path[512];
    char       *src;
    const char *p;
    int         pair = 0;

    snprintf(path, sizeof(path), "%s/src/tools/%s.c",
             AMINETXDUO_SOURCE_DIR, name);

    src = slurp(path);
    CHECK(src != NULL, "%s: cannot read %s", name, path);
    if (src == NULL)
        return;

    p = src;
    while ((p = strstr(p, "#define TEMPLATE")) != NULL)
    {
        check_pair(name, pair, src, p);
        pair++;
        p += strlen("#define TEMPLATE");
    }

    CHECK(pair > 0, "%s: no TEMPLATE found", name);

    free(src);
}

int main(void)
{
    int i;

    printf("ReadArgs templates against their argument enums\n\n");

    for (i = 0; i < COMMAND_COUNT; i++)
        check_command(commands[i]);

    printf("\n%d commands, %d checks, %d failure(s)\n",
           COMMAND_COUNT, checks, failures);

    return failures == 0 ? 0 : 1;
}
