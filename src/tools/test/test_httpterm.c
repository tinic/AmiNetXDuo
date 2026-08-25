/*
 * THE CONSOLE GATE, ON THE SOURCE.
 *
 * SetMode(), WaitForChar() and Open("*") carry no fh_Arg1, so httpterm.c gates
 * them on term_packet_current() instead.  That predicate answered FALSE while
 * neither term_break_port nor term_shell_task was known, and the only thing
 * that set term_shell_task to a task sat AFTER the switch those three packets
 * leave through -- so the first one of a session was refused ERROR_INVALID_LOCK.
 *
 * The invariant here is the one that catches that WITHOUT naming a fix: a task
 * has to be written into term_shell_task somewhere ABOVE the packet the gate
 * protects.  Learning it in the predicate satisfies it; so does moving the
 * assignment above the switch.
 *
 * Parsed rather than compiled, for the reason test_argtemplates.c gives: the
 * file reaches proto/dos.h.  What a live session then does is
 * tests/tools/run-wsterm.sh's, and stays there.
 *
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *source_dir(void)
{
    const char *env = getenv("AMINETXDUO_SOURCE_DIR");

    return (env != NULL && env[0] != '\0') ? env : AMINETXDUO_SOURCE_DIR;
}

/* Comments go first: the paragraph that explains the gate names every symbol
   asserted below, and would answer every search on its own. */
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

    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    buf = malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }

    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    strip_comments(buf);
    return buf;
}

/* The offset of the first `<name> =` that is not `== `, `!=` or `= NULL`, or
   -1.  A session start clearing the pointer is not a session naming an owner. */
static long assigns_a_task(const char *text, const char *name)
{
    size_t      n = strlen(name);
    const char *p;

    for (p = strstr(text, name); p != NULL; p = strstr(p + 1, name)) {
        const char *q = p + n;

        if (p != text && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
            continue;

        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '=' || q[1] == '=')
            continue;

        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n')
            q++;
        if (strncmp(q, "NULL", 4) == 0)
            continue;

        return (long)(p - text);
    }

    return -1;
}

static long offset_of(const char *text, const char *what)
{
    const char *p = strstr(text, what);

    return (p != NULL) ? (long)(p - text) : -1;
}

/* The arm for `label` that is GATED, not the one in the packet-name table. */
static long offset_of_gated(const char *text, const char *label)
{
    const char *p;

    for (p = strstr(text, label); p != NULL; p = strstr(p + 1, label)) {
        char window[256];
        size_t n = strlen(p) < sizeof(window) - 1 ? strlen(p)
                                                  : sizeof(window) - 1;

        memcpy(window, p, n);
        window[n] = '\0';

        if (strstr(window, "term_packet_current") != NULL)
            return (long)(p - text);
    }

    return -1;
}

int main(void)
{
    char *text = slurp("src/tools/httpterm.c");
    long  learned;
    long  gate;
    long  screen_mode;
    long  wait_char;

    printf("the console gate\n");

    if (text == NULL) {
        printf("\n%d checks, %d failure(s)\n", checks, failures + 1);
        return 1;
    }

    gate        = offset_of(text, "term_packet_current(const struct DosPacket");
    screen_mode = offset_of_gated(text, "case ACTION_SCREEN_MODE:");
    wait_char   = offset_of_gated(text, "case ACTION_WAIT_CHAR:");
    learned     = assigns_a_task(text, "term_shell_task");

    CHECK(gate >= 0, "httpterm.c: term_packet_current() is gone");
    CHECK(screen_mode > 0 && wait_char > 0,
          "httpterm.c: ACTION_SCREEN_MODE and ACTION_WAIT_CHAR are no longer"
          " answered in the packet switch");
    CHECK(learned >= 0, "httpterm.c: nothing ever names the session's task");

    /* The row this file was written for. */
    CHECK(learned >= 0 && screen_mode > 0 && learned < screen_mode,
          "httpterm.c: term_shell_task is first given a task at %ld, past the"
          " ACTION_SCREEN_MODE arm at %ld that term_packet_current() gates on"
          " it, so the first SetMode() of a session is refused",
          learned, screen_mode);
    CHECK(learned >= 0 && wait_char > 0 && learned < wait_char,
          "httpterm.c: term_shell_task is first given a task at %ld, past the"
          " ACTION_WAIT_CHAR arm at %ld, so the first WaitForChar() of a"
          " session is refused", learned, wait_char);

    /* And the isolation it must not have cost: dos.library's own answer still
       outranks the inference, and a dead session refuses everything. */
    CHECK(strstr(text, "term_break_port != NULL") != NULL,
          "httpterm.c: ACTION_CHANGE_SIGNAL's port no longer outranks the"
          " task the gate inferred");
    CHECK(strstr(text, "!term_active") != NULL,
          "httpterm.c: the gate no longer refuses a packet for a session that"
          " is over");

    free(text);

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
