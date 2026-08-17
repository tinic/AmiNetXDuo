/*
 * Every command's ReadArgs template against the enum that indexes it.
 *
 * A command reaches its arguments by position.  ReadArgs() fills an array in
 * template order, and the code says args[ARG_TIMEOUT] to get one out.  The two
 * agree only because somebody kept them in step, and nothing checks it.
 *
 * A keyword inserted in the middle of a template without moving the enum
 * shifts every argument after it by one.  Nothing warns: the types are all
 * LONG, the array is long enough, and the command runs.  QUIET starts reading
 * the number that TIMEOUT was given, and an /S switch reads a pointer and is
 * true whenever the option before it was present.  On a machine with no memory
 * protection, an /N read as a pointer is how a command takes the machine down
 * instead of printing usage.
 *
 * Thirty commands were rewritten wholesale in the 2026-08-04 diagnostics pass
 * with nothing on the host able to catch a mistake in any of them, and the
 * cross build only proves they compile.  Two of them have templates in two
 * halves and one has three aliases on a single keyword.
 *
 * This runs host-side and parses the sources rather than including them, for
 * the reason tests/sockopt/host/test_optnum_host.c gives: they need
 * <exec/types.h> and proto/dos.h, and a table restated here would only ever
 * agree with itself.
 *
 * Four groups, one ctest case each, named by argv[1].  No argument runs all of
 * them.
 *
 *   enums     For each command source holding a TEMPLATE and an ARG_ enum:
 *
 *               1. the template has as many comma-separated items as the enum
 *                  has entries before its count sentinel,
 *               2. item N names the same thing as ARG_ entry N,
 *               3. the -4/-6 pair is spelled one way in every command that
 *                  has it.
 *
 *   contract  What the template promises the code will do with the slot:
 *             every slot cleared before ReadArgs fills it, every /N read
 *             through a dereference and never as a number, no /S ever
 *             dereferenced, no option parsed and then ignored, and the
 *             modifiers themselves well formed.
 *
 *   usage     Every option named in a tool_usage() synopsis exists in the
 *             template it is a synopsis of.
 *
 *   docs      The template as restated in the file's own header comment and
 *             in docs/user/AmiNetXDuo.guide.  Both were stale when this group
 *             was written.
 *
 * A template item is a set of `=`-separated aliases and then modifiers, and
 * ReadArgs accepts any of them: `-c=COUNT` and `DNS=DOMAINNAMESERVERS` and
 * `DEBUG=-d` all appear in this tree, with the enum naming sometimes the first
 * and sometimes the second.  So entry N has to match any alias of item N, not
 * a fixed one.
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

/*
 * The tree to read.  CMake bakes in the real one.  The environment overrides
 * it, so tests/tools/argtemplates-verdict-selftest.sh can point the same
 * binary at a copy it has broken on purpose.
 */
static const char *source_dir(void)
{
    const char *env = getenv("AMINETXDUO_SOURCE_DIR");

    return (env != NULL && env[0] != '\0') ? env : AMINETXDUO_SOURCE_DIR;
}

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
 * Named rather than globbed.  A command that grows a template is added here
 * deliberately, and a file that disappears fails the test rather than quietly
 * reducing what is checked.
 */
static const char *const commands[] = {
    "addnetinterface", "addnetroute", "arp", "checknetconfig",
    "checknetdevice",
    "configurenetinterface", "fetch",
    "getnetstatus", "host", "hostname", "httpd", "iperf", "nc", "netsetup",
    "netshutdown",
    "netcapture",
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
 * then the remainder is split on '='.  A leading '-' is kept.  `DEBUG=-d` has
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
 * no list works.  nslookup, onoff and whois size theirs with ARG_COUNT, and
 * ping has ARG_COUNT as a real argument for -c=COUNT with ARG_ARGCOUNT as its
 * sentinel.  How the code uses the name is the only thing that tells them
 * apart.
 */
static int sentinel_of(const char *src, char *out, size_t outlen)
{
    const char *p = src;

    /*
     * The declaration, not a use.  addnetroute reads args[ARG_DST] hundreds of
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
 * arguments and AddNetRoute with six -- and each carries its own pair.  A check
 * of only the first left the six-argument half unchecked.
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
             source_dir(), name);

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

/* --------------------------------------------------------- the -4/-6 pair --- */

/*
 * Every command that resolves a name and then opens a socket carries
 * `IPV4=-4/S,IPV6=-6/S`, spelled the same way in all of them, and calls
 * tool_arg_family() to read it.  Listed rather than inferred, for the reason
 * `commands` above is listed: a new client command is added here deliberately.
 *
 * httpd and ShowNetServices are deliberately absent.  Neither resolves a name
 * to connect to -- for httpd the flag would say which family to bind, for
 * ShowNetServices which family to report a discovered service in -- so the
 * pair would mean a third and fourth thing under the same spelling.  arp is
 * IPv4 by definition.
 */
static const char *const family_commands[] = {
    "fetch", "host", "iperf", "nc", "ping", "sntp", "telnet", "tftp",
    "traceroute", "whois",
};

#define FAMILY_COUNT ((int)(sizeof(family_commands) / \
                            sizeof(family_commands[0])))

static void check_family(const char *name)
{
    char  path[512];
    char *src;
    char  tmpl[1024];
    const char *p;

    snprintf(path, sizeof(path), "%s/src/tools/%s.c",
             source_dir(), name);

    src = slurp(path);
    CHECK(src != NULL, "%s: cannot read %s", name, path);
    if (src == NULL)
        return;

    p = strstr(src, "#define TEMPLATE");
    tmpl[0] = '\0';
    if (p != NULL)
        (void)read_template(p, tmpl, sizeof(tmpl));

    /*
     * The exact spelling, not a -4 somewhere in the template.  nc and iperf
     * set it and the rest match them.  A command that wrote `-4=IPV4` would
     * parse the same and read differently in every `?` line and every piece of
     * documentation.
     */
    CHECK(strstr(tmpl, "IPV4=-4/S,IPV6=-6/S") != NULL,
          "%s: template has no IPV4=-4/S,IPV6=-6/S: \"%s\"", name, tmpl);

    CHECK(strstr(src, "ARG_IPV4") != NULL && strstr(src, "ARG_IPV6") != NULL,
          "%s: template carries the pair but the enum does not", name);

    /* One refusal for both flags at once, worded in toolsock.c and nowhere
       else. */
    CHECK(strstr(src, "tool_arg_family(") != NULL,
          "%s: does not read the pair through tool_arg_family()", name);

    free(src);
}

/* ------------------------------------------------------- template items --- */

#define MAX_ITEMS       32
#define MAX_ALIASES     4

typedef struct TmplItem
{
    char alias[MAX_ALIASES][64];    /* uppercased, '-' kept                  */
    int  naliases;
    char mods[8];                   /* "KN", "S", "MA", uppercased           */
} TmplItem;

static int parse_items(const char *tmpl, TmplItem *out, int max)
{
    char *copy = strdup(tmpl);
    char *save;
    char *item;
    int   n = 0;

    if (copy == NULL)
        return 0;

    for (item = strtok_r(copy, ",", &save); item != NULL && n < max;
         item = strtok_r(NULL, ",", &save))
    {
        TmplItem   *it = &out[n];
        const char *p = item;
        char        head[128];
        size_t      h = 0;
        size_t      m = 0;
        char       *asave;
        char       *alias;

        memset(it, 0, sizeof(*it));

        while (*p != '\0' && *p != '/' && h + 1 < sizeof(head))
            head[h++] = (char)toupper((unsigned char)*p++);
        head[h] = '\0';

        while (*p != '\0' && m + 1 < sizeof(it->mods))
        {
            if (*p != '/')
                it->mods[m++] = (char)toupper((unsigned char)*p);
            p++;
        }
        it->mods[m] = '\0';

        for (alias = strtok_r(head, "=", &asave);
             alias != NULL && it->naliases < MAX_ALIASES;
             alias = strtok_r(NULL, "=", &asave))
        {
            snprintf(it->alias[it->naliases], sizeof(it->alias[0]), "%s",
                     alias);
            it->naliases++;
        }

        n++;
    }

    free(copy);
    return n;
}

static int has_mod(const TmplItem *it, char m)
{
    return strchr(it->mods, m) != NULL;
}

/* ------------------------------------------------------------- scanning --- */

/*
 * Occurrences of a whole `ARG_NAME` token, so ARG_COUNT does not also count
 * ARG_ARGCOUNT.
 */
static int token_uses(const char *src, const char *token)
{
    const char *p = src;
    size_t      len = strlen(token);
    int         n = 0;

    while ((p = strstr(p, token)) != NULL)
    {
        char before = (p == src) ? ' ' : p[-1];
        char after  = p[len];

        if (!isalnum((unsigned char)before) && before != '_' &&
            !isalnum((unsigned char)after) && after != '_')
            n++;

        p += len;
    }

    return n;
}

/*
 * How one `args[ARG_X]` is used.  ReadArgs leaves a /N slot holding a LONG *
 * and a /S slot holding a flag, so the two have opposite correct spellings and
 * each spelling crashes on the other kind of slot.
 */
typedef enum ArgUse
{
    USE_DEREF,      /* *(LONG *)args[X]        */
    USE_TEST,       /* args[X] != 0            */
    USE_STORE,      /* args[X] = 0             */
    USE_BARE        /* anything else           */
} ArgUse;

static ArgUse classify_use(const char *src, const char *at, size_t len)
{
    const char *b = at;
    const char *a = at + len;

    while (b > src && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n'))
        b--;

    if (b - src >= 2 && b[-1] == ')')
    {
        const char *c = b - 1;

        while (c > src && c[-1] != '(')
            c--;
        /* the cast body ends in a star: (LONG *) / (const ULONG *) */
        {
            const char *e = b - 1;

            while (e > c && (e[-1] == ' ' || e[-1] == '\t'))
                e--;
            if (e > c && e[-1] == '*')
                return USE_DEREF;
        }
    }

    while (*a == ' ' || *a == '\t' || *a == '\n')
        a++;

    if (a[0] == '=' && a[1] == '=')
        return USE_TEST;
    if (a[0] == '!' && a[1] == '=')
        return USE_TEST;
    if (a[0] == '=')
        return USE_STORE;

    return USE_BARE;
}

/* ------------------------------------------------------------- contract --- */

/*
 * An option that parses and is then never read.  One exception, named in the
 * file's own header.
 */
static int deliberately_ignored(const char *cmd, const char *arg)
{
    /* host: the timeout is the resolver's, set in DEVS:Internet/resolv.conf. */
    return strcmp(cmd, "host") == 0 && strcmp(arg, "TIMEOUT") == 0;
}

static void contract_pair(const char *name, int pair, const char *src,
                          const char *from)
{
    char      tmpl[1024];
    char      args[64][64];
    TmplItem  items[MAX_ITEMS];
    int       nargs;
    int       nitems;
    int       bulk;
    int       nm = 0;
    int       i;
    int       j;

    if (!read_template(from, tmpl, sizeof(tmpl)))
        return;

    nargs  = read_args(src, from, args, 64);
    nitems = parse_items(tmpl, items, MAX_ITEMS);

    CHECK(nitems > 0 && nitems <= MAX_ITEMS,
          "%s[%d]: %d template items", name, pair, nitems);

    /* Cleared before ReadArgs: it fills what it matched and nothing else. */
    bulk = (strstr(src, "args[i] = 0") != NULL) ||
           (strstr(src, "memset(args") != NULL);

    for (i = 0; i < nitems; i++)
    {
        const TmplItem *it = &items[i];
        const char     *m;

        for (m = it->mods; *m != '\0'; m++)
            CHECK(strchr("AKNSTFM", *m) != NULL,
                  "%s[%d]: %s has modifier /%c, which ReadArgs has no such "
                  "letter for", name, pair, it->alias[0], *m);

        CHECK(!(has_mod(it, 'S') && has_mod(it, 'N')),
              "%s[%d]: %s is both a switch and a number",
              name, pair, it->alias[0]);
        CHECK(!(has_mod(it, 'S') && has_mod(it, 'A')),
              "%s[%d]: %s is a switch marked required, which can never be "
              "unsatisfied", name, pair, it->alias[0]);
        CHECK(!has_mod(it, 'F'),
              "%s[%d]: %s is /F, which swallows the rest of the line and is "
              "not what any command here means", name, pair, it->alias[0]);

        if (has_mod(it, 'M'))
            nm++;

        /* Every alias distinct.  ReadArgs takes the first match, and the
           second spelling then silently never fires. */
        for (j = i + 1; j < nitems; j++)
        {
            int a, b;

            for (a = 0; a < it->naliases; a++)
                for (b = 0; b < items[j].naliases; b++)
                    CHECK(strcmp(it->alias[a], items[j].alias[b]) != 0,
                          "%s[%d]: %s is an alias of both item %d and item %d",
                          name, pair, it->alias[a], i, j);
        }

        if (i >= nargs)
            continue;

        {
            char        token[80];
            char        expr[96];
            const char *p;
            int         zeroed = 0;
            int         reads = 0;

            snprintf(token, sizeof(token), "ARG_%s", args[i]);
            snprintf(expr, sizeof(expr), "args[ARG_%s]", args[i]);

            p = src;
            while ((p = strstr(p, expr)) != NULL)
            {
                ArgUse u = classify_use(src, p, strlen(expr));

                if (u == USE_STORE)
                    zeroed++;
                else
                    reads++;

                if (has_mod(it, 'N'))
                    CHECK(u != USE_BARE,
                          "%s[%d]: %s is /N, so args[ARG_%s] holds a LONG *, "
                          "and this use reads it as the number itself: %.60s",
                          name, pair, it->alias[0], args[i], p);

                if (has_mod(it, 'S') || has_mod(it, 'T'))
                    CHECK(u != USE_DEREF,
                          "%s[%d]: %s is a switch, so args[ARG_%s] is a flag "
                          "and not a pointer, and this dereferences it: %.60s",
                          name, pair, it->alias[0], args[i], p);

                p += strlen(expr);
            }

            CHECK(bulk || zeroed > 0,
                  "%s[%d]: nothing clears args[ARG_%s] before ReadArgs, so an "
                  "option left out is read from whatever was on the stack",
                  name, pair, args[i]);

            /* Read somewhere.  The enum declaration and the clearing do not
               count, and a helper taking the index (arg_or) does. */
            if (reads == 0 && !deliberately_ignored(name, args[i]))
                CHECK(token_uses(src, token) - 1 - zeroed > 0,
                      "%s[%d]: %s is in the template and ARG_%s is never "
                      "read, so the option parses and does nothing",
                      name, pair, it->alias[0], args[i]);
        }
    }

    CHECK(nm <= 1,
          "%s[%d]: %d items are /M, and only the first can ever collect "
          "anything", name, pair, nm);
}

static void check_contract(const char *name)
{
    char        path[512];
    char       *src;
    const char *p;
    int         pair = 0;
    int         nverstag = 0;
    int         nname = 0;

    snprintf(path, sizeof(path), "%s/src/tools/%s.c",
             source_dir(), name);

    src = slurp(path);
    CHECK(src != NULL, "%s: cannot read %s", name, path);
    if (src == NULL)
        return;

    p = src;
    while ((p = strstr(p, "#define TEMPLATE")) != NULL)
    {
        contract_pair(name, pair, src, p);
        pair++;
        p += strlen("#define TEMPLATE");
    }

    CHECK(strstr(src, "ReadArgs((CONST_STRPTR)TEMPLATE") != NULL,
          "%s: does not hand TEMPLATE to ReadArgs, so \"?\" prints something "
          "other than what this test checked", name);
    CHECK(strstr(src, "FreeArgs(") != NULL,
          "%s: ReadArgs with no FreeArgs", name);

    /*
     * The name in the version tag is what `Version` reports and what a user
     * types.  tool_name prefixes every diagnostic.  One command per pair, and
     * addnetroute.c and onoff.c are two commands each.
     */
    for (p = src; (p = strstr(p, "TOOL_VERSTAG(\"")) != NULL; p += 14)
    {
        char        want[128];
        const char *q = p + strlen("TOOL_VERSTAG(\"");
        size_t      n = 0;

        while (*q != '\0' && *q != '"' && n + 1 < sizeof(want))
            want[n++] = *q++;
        want[n] = '\0';
        nverstag++;

        {
            char needle[160];

            snprintf(needle, sizeof(needle), "tool_name = \"%s\"", want);
            CHECK(strstr(src, needle) != NULL,
                  "%s: TOOL_VERSTAG(\"%s\") with no matching tool_name, so "
                  "Version and the diagnostics disagree about what this "
                  "command is called", name, want);
        }
    }

    for (p = src; (p = strstr(p, "tool_name = \"")) != NULL;
         p += strlen("tool_name = \""))
        nname++;

    CHECK(nverstag == nname,
          "%s: %d version tags against %d tool_name definitions",
          name, nverstag, nname);

    free(src);
}

/* ---------------------------------------------------------------- usage --- */

/*
 * The first argument of tool_usage(), which is the synopsis a user sees when
 * the arguments were wrong.  A synopsis naming an option the template does not
 * have advises the user to type something that fails the same way again.
 */
static int usage_synopsis(const char *from, char *out, size_t outlen)
{
    const char *p = from;
    size_t      n = 0;
    int         instr = 0;

    for (; *p != '\0'; p++)
    {
        if (instr)
        {
            if (*p == '\\' && p[1] != '\0')
            {
                p++;
                continue;
            }
            if (*p == '"')
            {
                instr = 0;
                continue;
            }
            if (n + 1 < outlen)
                out[n++] = *p;
            continue;
        }

        if (*p == '"')
        {
            instr = 1;
            continue;
        }
        if (*p == ',' || *p == ')')
            break;
    }

    out[n] = '\0';
    return n > 0;
}

/*
 * An uppercase word is an option where an option can start: at the beginning,
 * after '[', or after a '>' or ']' that closed a placeholder.  After a letter
 * or a '|' it is a value -- `[CONFIGURE DHCP]`, `[MDNS YES|NO]`.
 */
static int is_option_position(const char *text, const char *at)
{
    const char *b = at;

    while (b > text && (b[-1] == ' ' || b[-1] == '\t'))
        b--;

    if (b == text)
        return 1;
    return b[-1] == '[' || b[-1] == '>' || b[-1] == ']';
}

static void check_usage(const char *name)
{
    char        path[512];
    char       *src;
    const char *p;
    TmplItem    items[MAX_ITEMS * 2];
    int         nitems = 0;

    snprintf(path, sizeof(path), "%s/src/tools/%s.c",
             source_dir(), name);

    src = slurp(path);
    CHECK(src != NULL, "%s: cannot read %s", name, path);
    if (src == NULL)
        return;

    for (p = src; (p = strstr(p, "#define TEMPLATE")) != NULL;
         p += strlen("#define TEMPLATE"))
    {
        char tmpl[1024];

        if (read_template(p, tmpl, sizeof(tmpl)) &&
            nitems < (int)(sizeof(items) / sizeof(items[0])))
            nitems += parse_items(tmpl, items + nitems,
                                  (int)(sizeof(items) / sizeof(items[0])) -
                                      nitems);
    }

    for (p = src; (p = strstr(p, "tool_usage(")) != NULL;
         p += strlen("tool_usage("))
    {
        char        text[512];
        const char *t;

        if (!usage_synopsis(p + strlen("tool_usage("), text, sizeof(text)))
            continue;

        for (t = text; *t != '\0'; t++)
        {
            char word[64];
            int  known = 0;
            int  i;
            int  a;

            if (*t == '-' && isalnum((unsigned char)t[1]) &&
                !isalnum((unsigned char)t[2]))
            {
                word[0] = '-';
                word[1] = (char)toupper((unsigned char)t[1]);
                word[2] = '\0';
                t++;
            }
            else if (isupper((unsigned char)*t) &&
                     isupper((unsigned char)t[1]) && is_option_position(text, t))
            {
                size_t n = 0;

                while (isalnum((unsigned char)*t) && n + 1 < sizeof(word))
                    word[n++] = *t++;
                word[n] = '\0';
                t--;
            }
            else
            {
                continue;
            }

            for (i = 0; i < nitems && !known; i++)
                for (a = 0; a < items[i].naliases; a++)
                    if (strcmp(items[i].alias[a], word) == 0)
                        known = 1;

            CHECK(known,
                  "%s: the usage line offers %s and no template has it: "
                  "\"%s\"", name, word, text);
        }
    }

    free(src);
}

/* ----------------------------------------------------------------- docs --- */

/*
 * Whitespace and comment furniture removed, so a template restated over three
 * wrapped lines compares equal to the one on a single #define.
 */
static char *flatten(const char *text, int strip_comment_stars)
{
    size_t  len = strlen(text);
    char   *out = malloc(len + 1);
    size_t  n = 0;
    const char *p = text;

    if (out == NULL)
        return NULL;

    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (strip_comment_stars && *p == '*')
            p++;

        while (*p != '\0' && *p != '\n')
        {
            if (*p != ' ' && *p != '\t' && *p != '\r')
                out[n++] = *p;
            p++;
        }
        if (*p == '\n')
            p++;
    }

    out[n] = '\0';
    return out;
}

/* The commands docs/user/AmiNetXDuo.guide prints a template for. */
static const char *const guide_commands[] = {
    "addnetinterface", "onoff", "shownetstatus", "netstat", "ping", "host",
    "checknetdevice",
};

#define GUIDE_COUNT ((int)(sizeof(guide_commands) / \
                           sizeof(guide_commands[0])))

static int in_guide_set(const char *name)
{
    int i;

    for (i = 0; i < GUIDE_COUNT; i++)
        if (strcmp(guide_commands[i], name) == 0)
            return 1;
    return 0;
}

static void check_docs(const char *name, const char *guide_flat)
{
    char        path[512];
    char       *src;
    char       *header_flat = NULL;
    const char *p;
    int         pair = 0;

    snprintf(path, sizeof(path), "%s/src/tools/%s.c",
             source_dir(), name);

    src = slurp(path);
    CHECK(src != NULL, "%s: cannot read %s", name, path);
    if (src == NULL)
        return;

    /* The leading block comment, which is where every command restates its
       template for a reader of the source. */
    if (strncmp(src, "/*", 2) == 0)
    {
        char *end = strstr(src + 2, "*/");

        if (end != NULL)
        {
            char *head = malloc((size_t)(end - src) + 1);

            if (head != NULL)
            {
                memcpy(head, src + 2, (size_t)(end - src) - 2);
                head[(size_t)(end - src) - 2] = '\0';
                header_flat = flatten(head, 1);
                free(head);
            }
        }
    }

    CHECK(header_flat != NULL, "%s: no leading comment", name);

    for (p = src; (p = strstr(p, "#define TEMPLATE")) != NULL;
         p += strlen("#define TEMPLATE"))
    {
        char  tmpl[1024];
        char *flat;

        if (!read_template(p, tmpl, sizeof(tmpl)))
            continue;

        flat = flatten(tmpl, 0);
        if (flat == NULL)
            continue;

        if (header_flat != NULL)
            CHECK(strstr(header_flat, flat) != NULL,
                  "%s[%d]: the header comment does not restate the template: "
                  "\"%s\"", name, pair, tmpl);

        if (guide_flat != NULL && in_guide_set(name))
        {
            char *cut = strrchr(flat, ',');

            CHECK(strstr(guide_flat, flat) != NULL,
                  "%s[%d]: docs/user/AmiNetXDuo.guide prints a template for "
                  "this command and it is not this one: \"%s\"",
                  name, pair, tmpl);

            /*
             * The guide prints netstat's template twice, once as a section
             * heading and once as what `netstat ?` answers, and an edit to
             * one of the two leaves the other stale.  So every copy that gets
             * as far as the last option has to carry it: search on the
             * template without its last item and require the whole thing at
             * each hit.
             */
            if (cut != NULL)
            {
                size_t n = (size_t)(cut - flat);
                char  *prefix = malloc(n + 1);

                if (prefix != NULL)
                {
                    const char *q;

                    memcpy(prefix, flat, n);
                    prefix[n] = '\0';

                    for (q = guide_flat; (q = strstr(q, prefix)) != NULL;
                         q += n)
                        CHECK(strncmp(q, flat, strlen(flat)) == 0,
                              "%s[%d]: the guide prints this template more "
                              "than once and one copy stops early: \"%.*s\"",
                              name, pair, (int)n + 16, q);

                    free(prefix);
                }
            }
        }

        free(flat);
        pair++;
    }

    free(header_flat);
    free(src);
}

/* ------------------------------------------------------------------ main --- */

static int wanted(const char *group, const char *arg)
{
    return arg == NULL || strcmp(group, arg) == 0;
}

int main(int argc, char **argv)
{
    const char *group = (argc > 1) ? argv[1] : NULL;
    char       *guide = NULL;
    char       *guide_flat = NULL;
    int         i;

    if (group != NULL && strcmp(group, "enums") != 0 &&
        strcmp(group, "contract") != 0 && strcmp(group, "usage") != 0 &&
        strcmp(group, "docs") != 0)
    {
        printf("usage: test_argtemplates [enums|contract|usage|docs]\n");
        return 2;
    }

    printf("ReadArgs templates%s%s\n\n",
           (group != NULL) ? ": " : "", (group != NULL) ? group : "");

    if (wanted("enums", group))
    {
        for (i = 0; i < COMMAND_COUNT; i++)
            check_command(commands[i]);

        for (i = 0; i < FAMILY_COUNT; i++)
            check_family(family_commands[i]);
    }

    if (wanted("contract", group))
        for (i = 0; i < COMMAND_COUNT; i++)
            check_contract(commands[i]);

    if (wanted("usage", group))
        for (i = 0; i < COMMAND_COUNT; i++)
            check_usage(commands[i]);

    if (wanted("docs", group))
    {
        char path[512];

        snprintf(path, sizeof(path), "%s/docs/user/AmiNetXDuo.guide",
                 source_dir());
        guide = slurp(path);
        CHECK(guide != NULL, "cannot read %s", path);
        if (guide != NULL)
            guide_flat = flatten(guide, 0);

        for (i = 0; i < COMMAND_COUNT; i++)
            check_docs(commands[i], guide_flat);

        free(guide_flat);
        free(guide);
    }

    printf("\n%d commands, %d checks, %d failure(s)\n",
           COMMAND_COUNT, checks, failures);

    return failures == 0 ? 0 : 1;
}
