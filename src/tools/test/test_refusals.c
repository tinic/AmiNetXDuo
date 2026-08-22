/*
 * A refusal that does not say why, and the three shapes it takes here.
 *
 * WHAT WENT WRONG
 *
 *   4d9791be turned three-paragraph diagnostics into one line, which was
 *   right, and took 352 tool_advise() calls out to do it. Some of those calls
 *   were the whole of a message. What was left behind:
 *
 *     AddNetRoute   explain() and explain6() became switch statements whose
 *                   arms are `case ROUTE_ENOSYS: break;`. The command prints
 *                   "the route to fd00:9::/64 was not added" and stops. Which
 *                   of five refusals it was, and that IPv6 has no
 *                   prefix-to-next-hop table at all so no spelling of the
 *                   command would have worked, went with them.
 *     host          `host ::1` says the literal is an address and not that
 *                   nslookup is the command that asks the DNS about one.
 *     no IPv6       the IPv4-only build's refusals name IPv6 and stop, so a
 *                   user cannot tell a machine that is missing an address
 *                   from a library that has no IPv6 in it at all.
 *
 *   None of it is anything a compiler sees: an empty case arm is legal, and a
 *   shorter string is a shorter string. The emulator tier caught it in
 *   tests/ipv6/run-tools-amiberry.sh, which runs on a cron and a tag. This is
 *   the same claim in the tier a push runs.
 *
 *   It parses the sources rather than compiling them, for the reason
 *   test_argtemplates.c gives: they reach proto/dos.h, and what is asserted
 *   here is the shape of the source rather than anything a call could return.
 *
 * WHAT IT DOES NOT PROVE
 *
 *   That the sentences are true, or that the command reaches them. That is
 *   tests/ipv6/run-tools-amiberry.sh's, on a real segment, and stays there.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AMINETXDUO_SOURCE_DIR
#define AMINETXDUO_SOURCE_DIR "."
#endif

/* The tree to read. CMake bakes in the real one; the environment overrides it
   so a selftest can point the same binary at a copy it has broken. */
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

/*
 * Comments go first. Every phrase asserted below has to be in a string the
 * command prints: `nslookup` in the comment that explains why host refuses a
 * literal is exactly what made the message look present when it was not.
 */
static void strip_comments(char *s)
{
    char *r = s;
    char *w = s;

    while (*r != '\0') {
        if (r[0] == '/' && r[1] == '*') {
            r += 2;
            while (*r != '\0' && !(r[0] == '*' && r[1] == '/'))
                r++;
            if (*r != '\0')
                r += 2;
            *w++ = ' ';
            continue;
        }
        if (r[0] == '/' && r[1] == '/') {
            while (*r != '\0' && *r != '\n')
                r++;
            continue;
        }
        if (*r == '"') {                /* a literal is not a comment */
            *w++ = *r++;
            while (*r != '\0' && *r != '"') {
                if (*r == '\\' && r[1] != '\0')
                    *w++ = *r++;
                *w++ = *r++;
            }
            if (*r == '\0')
                break;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/*
 * A message wrapped over two source lines is two string literals, and the
 * checks below are about what the user reads. Joining them here is what lets
 * a phrase be searched for without the test caring where the line broke.
 */
static void join_literals(char *s)
{
    char *r = s;
    char *w = s;

    while (*r != '\0') {
        if (*r == '"') {
            char *look = r + 1;

            while (*look == ' ' || *look == '\t' || *look == '\n' ||
                   *look == '\r' || *look == '\\')
                look++;

            if (*look == '"') {
                r = look + 1;       /* drop both quotes and the gap */
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static char *slurp(const char *rel)
{
    char  path[512];
    FILE *fp;
    long  n;
    char *buf;

    snprintf(path, sizeof(path), "%s/%s", source_dir(), rel);

    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  FAIL cannot read %s\n", path);
        failures++;
        return NULL;
    }

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
    strip_comments(buf);
    join_literals(buf);
    return buf;
}

/* ------------------------------------------------------- the empty arms --- */

/*
 * Every `case X:` in a function must reach something that prints before its
 * `break`. `default:` is exempt: an error code nobody enumerated has no
 * sentence to write about it, and inventing one would be worse than the bare
 * refusal above it.
 *
 * The scan runs from the function's opening brace to the first line that is a
 * lone `}` in column one, which is this tree's brace style.
 */
static void arms_all_speak(const char *file, const char *text,
                           const char *func)
{
    const char *p = strstr(text, func);
    const char *end;
    const char *arm;
    int         arms = 0;

    if (p == NULL) {
        CHECK(0, "%s has no %s", file, func);
        return;
    }

    end = strstr(p, "\n}\n");
    if (end == NULL)
        end = p + strlen(p);

    for (arm = strstr(p, "case "); arm != NULL && arm < end;
         arm = strstr(arm + 1, "case ")) {
        const char *stop = strstr(arm, "break;");
        char        name[64];
        size_t      i;

        if (stop == NULL || stop > end)
            break;

        for (i = 0; i + 1 < sizeof(name) && arm[5 + i] != '\0' &&
                    arm[5 + i] != ':' && arm[5 + i] != '\n'; i++)
            name[i] = arm[5 + i];
        name[i] = '\0';

        arms++;

        /* Something between the label and the break has to reach the user.
           tool_printf() is what every arm here uses; tool_error() would do
           and is looked for so a rewrite is not gratuitously broken. */
        CHECK(memmem(arm, (size_t)(stop - arm), "tool_printf", 11) != NULL ||
              memmem(arm, (size_t)(stop - arm), "tool_error", 10) != NULL,
              "%s: %s case %s refuses and says nothing", file, func, name);
    }

    /* A function whose arms were all deleted would otherwise pass by having
       none left to check. */
    CHECK(arms >= 4, "%s: %s has %d case arms, expected at least 4",
          file, func, arms);
}

/* --------------------------------------------------------- the no-IPv6 -- */

/*
 * Every refusal that turns an IPv6 address away because the library has none
 * is followed by tool_no_ipv6_note(), which is where "it is a build option"
 * and "ShowNetStatus INTERFACES" live, once. Counted rather than matched to a
 * call site: what went wrong was deletion, and a count catches that without
 * the test having to parse C. The note itself is asserted in t_the_note();
 * an emptied helper would satisfy every caller.
 */
static void no_ipv6_sites_explain(const char *file, const char *text,
                                  int want)
{
    const char *p;
    int         notes = 0;

    for (p = strstr(text, "tool_no_ipv6_note("); p != NULL;
         p = strstr(p + 1, "tool_no_ipv6_note("))
        notes++;

    CHECK(strstr(text, "no IPv6") != NULL,
          "%s: the \"no IPv6\" refusal is gone", file);
    CHECK(notes >= want,
          "%s: %d of %d \"no IPv6\" refusals say it is a build option",
          file, notes, want);
}

/* ------------------------------------------------------------- the tests --- */

static void t_addnetroute(void)
{
    char *text = slurp("src/tools/addnetroute.c");

    if (text == NULL)
        return;

    arms_all_speak("addnetroute.c", text, "static VOID explain(LONG err");
    arms_all_speak("addnetroute.c", text, "static VOID explain6(LONG err");

    /* The one the backlog names: a prefix given a next hop. IPv6 has no table
       for it, and "the route was not added" does not say so. */
    CHECK(strstr(text, "no table that maps a prefix to a next hop") != NULL,
          "addnetroute.c: the ENOSYS arm does not say IPv6 has no such table");

    no_ipv6_sites_explain("addnetroute.c", text, 1);

    free(text);
}

static void t_host(void)
{
    char *text = slurp("src/tools/host.c");

    if (text == NULL)
        return;

    CHECK(strstr(text, "is an address, not a name") != NULL,
          "host.c: the literal refusal is gone");

    /* host cannot reverse an IPv6 address and nslookup can. A refusal that
       does not name it leaves the question unanswerable. */
    CHECK(strstr(text, "nslookup") != NULL,
          "host.c: the literal refusal does not name nslookup");

    no_ipv6_sites_explain("host.c", text, 1);

    free(text);
}

static void t_arp(void)
{
    char *text = slurp("src/tools/arp.c");

    if (text == NULL)
        return;

    /* The address is granted in the same breath as the refusal, or the answer
       reads as a verdict on what was typed. */
    CHECK(strstr(text, "well-formed IPv6 address") != NULL,
          "arp.c: the IPv6 refusal no longer grants the address is valid");

    no_ipv6_sites_explain("arp.c", text, 1);

    free(text);
}

static void t_toolsock(void)
{
    char *text = slurp("src/tools/toolsock.c");

    if (text == NULL)
        return;

    /* Eight commands share this one, so it is the highest-traffic refusal in
       the tree: ping, traceroute, nc, telnet, tftp, whois, sntp, fetch. */
    no_ipv6_sites_explain("toolsock.c", text, 2);

    free(text);
}

/* The note itself. An emptied helper would satisfy every call site above. */
static void t_the_note(void)
{
    char *text = slurp("src/tools/tool_util.c");

    if (text == NULL)
        return;

    CHECK(strstr(text, "VOID tool_no_ipv6_note(VOID)") != NULL,
          "tool_util.c: tool_no_ipv6_note() is gone");
    CHECK(strstr(text, "build option") != NULL,
          "tool_util.c: the note no longer says IPv6 is a build option");
    CHECK(strstr(text, "ShowNetStatus") != NULL,
          "tool_util.c: the note no longer points at ShowNetStatus");

    free(text);
}

int main(void)
{
    printf("refusals that say why\n");

    t_addnetroute();
    t_host();
    t_arp();
    t_toolsock();
    t_the_note();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
