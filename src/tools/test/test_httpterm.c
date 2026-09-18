/*
 * THE CONSOLE GATE.
 *
 * SetMode(), WaitForChar() and Open("*") carry no fh_Arg1, so httpterm.c gates
 * them on term_packet_current() instead.  That predicate answered FALSE while
 * neither term_break_port nor term_shell_task was known, and the only thing
 * that set term_shell_task to a task sat AFTER the switch those three packets
 * leave through -- so the first one of a session was refused ERROR_INVALID_LOCK.
 *
 * Two halves, because httpterm.c reaches proto/dos.h and compiles nowhere but
 * the target:
 *
 *   * the ORDER, parsed off the source, for the reason test_argtemplates.c
 *     gives -- a task has to be written into term_shell_task somewhere ABOVE
 *     the packet the gate protects;
 *   * the DECISION, compiled and run, out of src/tools/httpterm_owner.h, which
 *     is the predicate itself in plain pointers.  Three callers: the session's
 *     own Shell, a process that Shell spawned -- its own pr_MsgPort, matching
 *     neither the break port nor the Shell's task -- and an unrelated process,
 *     which is what stops the fix from being "admit everybody".
 *
 * What a live session then does is tests/tools/run-wsterm.sh's, and stays there.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpterm_owner.h"

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

/* Stand-ins for the only things the predicate ever does with a pointer, which
   is compare it.  Distinct objects, so no two addresses can coincide. */
static char handler_port, other_port, break_port;
static char shell_task, child_task, stranger_task, ghost_task;

static int admits(const TermOwner *s, const TermCallerId *c)
{
    const void *adopt = NULL;

    return term_owner_admits(s, c, &adopt);
}

static void session(TermOwner *s, const void *brk)
{
    s->to_Active    = 1;
    s->to_Port      = &handler_port;
    s->to_BreakPort = brk;
    s->to_ShellTask = &shell_task;
}

/* The Shell itself: dos.library named its port in ACTION_CHANGE_SIGNAL, and
   its pr_ConsoleTask is the handler it was started on. */
static void owner(TermCallerId *c)
{
    c->tc_Port    = &break_port;
    c->tc_Task    = &shell_task;
    c->tc_Console = &handler_port;
    c->tc_Stale   = 0;
}

/* A process the Shell spawned: its OWN pr_MsgPort, its own task, and the
   console it inherited. */
static void spawned(TermCallerId *c)
{
    c->tc_Port    = &other_port;
    c->tc_Task    = &child_task;
    c->tc_Console = &handler_port;
    c->tc_Stale   = 0;
}

/* Somebody else's process entirely: a console that is not this one. */
static void stranger(TermCallerId *c)
{
    c->tc_Port    = &other_port;
    c->tc_Task    = &stranger_task;
    c->tc_Console = &other_port;
    c->tc_Stale   = 0;
}

static void test_callers(void)
{
    TermOwner    s;
    TermCallerId c;
    const void  *adopt;

    printf("who may drive the session\n");

    /* The owner, before and after dos.library has named its port. */
    session(&s, NULL);
    owner(&c);
    CHECK(admits(&s, &c), "the session's own Shell is refused");

    session(&s, &break_port);
    owner(&c);
    CHECK(admits(&s, &c), "the session's own Shell is refused once its break"
          " port is known");

    /* THE ROW.  Both arms: the break port is what a real Shell sets early, so
       a fix that only works before it does is not one. */
    session(&s, NULL);
    spawned(&c);
    CHECK(admits(&s, &c), "a process the Shell spawned is refused: its own"
          " pr_MsgPort is neither the break port nor the Shell's task");

    session(&s, &break_port);
    spawned(&c);
    CHECK(admits(&s, &c), "a process the Shell spawned is refused once the"
          " break port is known");

    /* And the isolation that must not have cost anything. */
    session(&s, NULL);
    stranger(&c);
    CHECK(!admits(&s, &c), "an unrelated process may drive this console");

    session(&s, &break_port);
    stranger(&c);
    CHECK(!admits(&s, &c), "an unrelated process may drive this console with a"
          " break port set");

    /* A Shell whose session was let go of keeps this port as its console. */
    session(&s, &break_port);
    spawned(&c);
    c.tc_Task  = &ghost_task;
    c.tc_Stale = 1;
    CHECK(!admits(&s, &c), "an abandoned session's runner may drive the"
          " replacement");

    /* A caller with no pr_ConsoleTask at all still falls back, and a dead
       session still refuses everyone. */
    session(&s, NULL);
    s.to_ShellTask = NULL;
    c.tc_Port    = &other_port;
    c.tc_Task    = &shell_task;
    c.tc_Console = NULL;
    c.tc_Stale   = 0;
    adopt        = NULL;
    CHECK(term_owner_admits(&s, &c, &adopt) && adopt == &shell_task,
          "the first packet of a session no longer names its owner");

    s.to_Active = 0;
    owner(&c);
    CHECK(!admits(&s, &c), "a session that is over still answers packets");
}

/* One console under three handles: a read is keystrokes and a write is the
   screen whichever handle carries it.  The READ-on-Output row is 3.2's Dir
   asking the window's size; a read parked on the output ring never returns. */
static void test_rings(void)
{
    const int END = 1007;                       /* ACTION_END */

    printf("the rings\n");

    CHECK(term_ring_for(TERM_ID_OUT, TERM_PKT_READ) == TERM_RING_IN,
          "a Read() on Output() is not served from the keyboard");
    CHECK(term_ring_for(TERM_ID_CON, TERM_PKT_READ) == TERM_RING_IN,
          "a Read() on Open(\"*\") is not served from the keyboard");
    CHECK(term_ring_for(TERM_ID_IN, TERM_PKT_READ) == TERM_RING_IN,
          "a Read() on Input() is not served from the keyboard");
    CHECK(term_ring_for(TERM_ID_IN, TERM_PKT_WRITE) == TERM_RING_OUT,
          "a Write() on Input() does not reach the screen");
    CHECK(term_ring_for(TERM_ID_OUT, TERM_PKT_WRITE) == TERM_RING_OUT,
          "a Write() on Output() does not reach the screen");
    CHECK(term_ring_for(TERM_ID_CON, TERM_PKT_WRITE) == TERM_RING_OUT,
          "a Write() on Open(\"*\") does not reach the screen");
    CHECK(term_ring_for(TERM_ID_IN, END) == TERM_RING_IN,
          "closing Input() is not about the input ring");
    CHECK(term_ring_for(TERM_ID_OUT, END) == TERM_RING_OUT,
          "closing Output() is not about the output ring");
    CHECK(term_ring_for(TERM_ID_CON, END) == TERM_RING_OUT,
          "closing Open(\"*\") is not about the output ring");
    CHECK(term_ring_for(0, TERM_PKT_READ) == TERM_RING_NONE &&
          term_ring_for(4, TERM_PKT_WRITE) == TERM_RING_NONE,
          "a handle id that is not one of the three is answered");
}

/* How many times `needle` occurs in `text`. */
static int count_of(const char *text, const char *needle)
{
    int         n = 0;
    const char *p = text;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* Is there an `a` before a `b` in [text+from, text+to)?  Both must be present
   and in that order, or 0. */
static int a_before_b(const char *text, long from, long to,
                      const char *a, const char *b)
{
    long oa, ob;
    const char *pa, *pb;

    pa = strstr(text + from, a);
    pb = strstr(text + from, b);
    if (pa == NULL || pb == NULL)
        return 0;
    oa = (long)(pa - text);
    ob = (long)(pb - text);
    if (oa >= to || ob >= to)
        return 0;
    return oa < ob;
}

/*
 * THE WINDOW SIZE.
 *
 * A console client -- Dropbear's ssh (con_query_size), C:More -- asks the size
 * with ACTION_DISK_INFO, reads the returned IORequest's io_Unit as a ConUnit,
 * and takes cu_XMax/cu_YMax.  httpterm had left io_Unit at zero, so the read
 * was NULL and the client fell back to 80x25 (ssh) or nothing (More reported
 * zero rows).  term_sync_conunit() points io_Unit at a ConUnit whose
 * cu_XMax/cu_YMax are term_cols-1/term_rows-1, and is called on every size
 * change so a client that cached the unit pointer still sees a resize.
 *
 * The client adds one back (cu_XMax + 1); asserting httpterm's minus-one beside
 * the client's plus-one proves the round trip is the identity, off-A3000.
 */
static void test_winsize(void)
{
    char *term   = slurp("src/tools/httpterm.c");
    char *client = slurp("clients/dropbear/amiga_dropbear.c");
    long  disk, disk_end, resize, resize_end;

    printf("the window size\n");

    if (term == NULL || client == NULL) {
        failures++;
        free(term);
        free(client);
        return;
    }

    /* io_Unit is pointed at the ConUnit, not left NULL. */
    CHECK(strstr(term, "term_ioreq.io_Unit") != NULL &&
          strstr(term, "&term_conunit") != NULL,
          "httpterm.c: term_ioreq.io_Unit is not pointed at a ConUnit, so a"
          " client reads NULL and falls back to 80x25");

    /* The size the client will read is term_cols-1/term_rows-1. */
    CHECK(strstr(term, "cu_XMax = (WORD)(term_cols > 0 ? term_cols - 1")
              != NULL &&
          strstr(term, "cu_YMax = (WORD)(term_rows > 0 ? term_rows - 1")
              != NULL,
          "httpterm.c: cu_XMax/cu_YMax are not term_cols-1/term_rows-1");

    /* The ACTION_DISK_INFO arm refreshes the unit before handing it back.
       Anchored on the InfoData the handler fills, not the "case" label, which
       also appears in the packet-name table. */
    disk = offset_of(term, "struct InfoData *id = (struct InfoData *)BADDR");
    disk_end = disk >= 0 ? offset_of(term + disk, "term_reply(pkt") : -1;
    if (disk_end >= 0)
        disk_end += disk;
    CHECK(disk >= 0 && disk_end > disk &&
          a_before_b(term, disk, disk_end, "term_sync_conunit()",
                     "id->id_InUse"),
          "httpterm.c: ACTION_DISK_INFO hands back id_InUse without first"
          " syncing the ConUnit, so the size is whatever it last was");

    /* A resize refreshes it too -- a cached unit pointer sees the new size. */
    resize = offset_of(term, "http_term_resize(UWORD");
    resize_end = resize >= 0 ? offset_of(term + resize, "http_term_write")
                             : -1;
    if (resize_end >= 0)
        resize_end += resize;
    CHECK(resize >= 0 && resize_end > resize &&
          a_before_b(term, resize, resize_end, "term_rows = rows",
                     "term_sync_conunit()"),
          "httpterm.c: http_term_resize() stores the new size without"
          " re-syncing the ConUnit, so a client that cached the unit keeps"
          " the old size");

    /* Called at least at init, in ACTION_DISK_INFO and in http_term_resize,
       past its one definition. */
    CHECK(count_of(term, "term_sync_conunit") >= 4,
          "httpterm.c: term_sync_conunit is not called from all of init,"
          " ACTION_DISK_INFO and http_term_resize");

    /* The client's plus-one, partner of httpterm's minus-one: the round trip
       is size-preserving, no off-by-one. */
    CHECK(strstr(client, "cu->cu_XMax + 1") != NULL &&
          strstr(client, "cu->cu_YMax + 1") != NULL,
          "amiga_dropbear.c: con_query_size no longer adds one back to"
          " cu_XMax/cu_YMax, so the size it reports is off by one from the"
          " one httpterm stored");

    free(term);
    free(client);
}

/*
 * THE CURSOR REPORT.
 *
 * More writes CSI 6 n after every line, and the answer is the cursor row it
 * paginates by.  Forwarded to the browser, the answer crossed the LAN and
 * arrived in the pager's key reads (the "--- More ---" flood).  The handler
 * answers it itself, off a cursor it tracks from the Shell's output, and does
 * not forward the query.
 */
static void test_cursor(void)
{
    char *term = slurp("src/tools/httpterm.c");
    long  dsr, dsr_end;

    printf("the cursor report\n");

    if (term == NULL) { failures++; return; }

    /* The report exists and is written into the INPUT ring, as CSI ... R. */
    CHECK(strstr(term, "term_cursor_report") != NULL &&
          strstr(term, "(UBYTE)'R'") != NULL &&
          strstr(term, "term_inject(b, n)") != NULL,
          "httpterm.c: no CSI 6 n cursor report injected into the input");

    /* The cursor is tracked from the output, not guessed. */
    CHECK(strstr(term, "term_track_byte") != NULL &&
          strstr(term, "term_seq_param") != NULL,
          "httpterm.c: the cursor is not tracked from the Shell's output");

    /* The DSR arm answers 6 and does NOT fall through to term_seq_flush --
       forwarding it is exactly the bug. */
    dsr = offset_of(term, "case (UBYTE)'n':");
    dsr_end = dsr >= 0 ? offset_of(term + dsr, "break;") : -1;
    if (dsr_end >= 0) dsr_end += dsr;
    CHECK(dsr >= 0 && dsr_end > dsr &&
          a_before_b(term, dsr, dsr_end, "term_seq_param(0UL, 0UL) == 6UL",
                     "term_cursor_report()"),
          "httpterm.c: CSI 6 n is not answered before it could be forwarded");
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

    /* The decision below is only the shipped one if httpterm.c reaches it, and
       only answers for a spawned process if it is handed pr_ConsoleTask. */
    CHECK(strstr(text, "term_owner_admits") != NULL,
          "httpterm.c: the gate no longer goes through httpterm_owner.h, so"
          " nothing below this line is about the shipped predicate");
    CHECK(strstr(text, "pr_ConsoleTask") != NULL,
          "httpterm.c: the gate no longer looks at the caller's"
          " pr_ConsoleTask, so a process the Shell spawned cannot be told"
          " from an unrelated one");
    CHECK(strstr(text, "NT_PROCESS") != NULL,
          "httpterm.c: mp_SigTask is read as a Process without checking that"
          " it is one");

    free(text);

    test_callers();
    test_rings();
    test_winsize();
    test_cursor();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
