/*
 * <atf-c.h> for AmigaOS -- enough of FreeBSD's ATF to run its netinet tests.
 *
 * FreeBSD's tests/sys/netinet are ordinary userspace programs: they call
 * socket(), bind(), sendto() against the local stack and assert on what comes
 * back.  Nothing in them needs a second machine or a kernel module, so the only
 * thing standing between those files and this stack is <atf-c.h> -- a header
 * this tree can supply -- plus the handful of Unix facilities AmigaOS does not
 * have.  Adopted files keep their BSD-2-Clause header unmodified; this shim is
 * ours.
 *
 * An ATF test program is a table of test cases and a runner that forks one
 * child per case, reads its result off a pipe and reports it to kyua.  There is
 * no fork() here, so this runs them in sequence in one process and reports the
 * way every other guest test in this tree does: one line per assertion and a
 * "N checks, M failures -- PASS/FAIL" trailer, the shape
 * tests/sockopt/sockopt_test.c established and tools/fsuae-run.sh greps for.
 *
 * ------------------------------------------------------------------ macros --
 *
 *   ATF_TC(name)                 declares body + head
 *   ATF_TC_WITHOUT_HEAD(name)    declares body, defines an empty head
 *   ATF_TC_WITH_CLEANUP(name)    declares body + head + cleanup
 *   ATF_TC_HEAD(name, tc)        defines the head
 *   ATF_TC_BODY(name, tc)        defines the body
 *   ATF_TC_CLEANUP(name, tc)     defines the cleanup
 *   ATF_TP_ADD_TCS(tp)           defines main() and the registration function
 *   ATF_TP_ADD_TC(tp, name)      appends one case to the table
 *
 *   ATF_REQUIRE(e)               records; on failure ends THIS case (longjmp)
 *   ATF_REQUIRE_MSG(e, fmt, ...) as above, message formatted with vsnprintf
 *   ATF_REQUIRE_EQ(a, b)         a == b, both printed on failure
 *   ATF_REQUIRE_EQ_MSG(a, b, ...)
 *   ATF_REQUIRE_ERRNO(err, e)    e true and errno == err
 *   ATF_REQUIRE_STREQ(a, b)
 *   ATF_CHECK*                   same set, but the case continues
 *   atf_tc_fail(fmt, ...)        one failure, ends the case
 *   atf_tc_fail_nonfatal(...)    one failure, case continues
 *   atf_tc_skip(fmt, ...)        ends the case, counted as skipped not failed
 *   atf_tc_pass()                ends the case as passed
 *   atf_no_error()               0
 *
 * A test case ends early through longjmp(), which is what ATF's fatal
 * assertions do by ending the child process.  Anything the body had open at
 * that point stays open -- a real ATF child would have had it closed by exit().
 * That is the one behavioural difference that can leak between cases, and it is
 * why a case that leaks descriptors is a case to adapt, not to run as-is.
 *
 * -------------------------------------------------------------- metadata ---
 *
 * atf_tc_set_md_var() is honoured for exactly what can be answered here:
 *
 *   require.user      always skips.  AmigaOS has no user IDs, so neither
 *                     "root" nor "unprivileged" is a state this machine can be
 *                     in, and a test that asserts a privilege boundary is
 *                     asserting something that does not exist.
 *   require.config    always skips.  There is no kyua to supply config
 *                     variables, so atf_tc_has_config_var() is false and the
 *                     test's own skip path would fire anyway.
 *   descr             logged.
 *   anything else     ignored, and named in the log so it is not silently lost.
 *
 * ---------------------------------------------------- deliberately absent ---
 *
 *   fork(), waitpid(), seteuid(), getpwnam()  -- no processes, no user IDs.
 *     A test that binds a port in a child to prove the parent's bind() is
 *     refused cannot be shimmed; it has to be rewritten or dropped.
 *   poll(), POLLRDHUP                         -- WaitSelect() is the only
 *     readiness call, has no revents word and no half-close bit.
 *   getifaddrs()                              -- use IoctlSocket(SIOCGIFCONF).
 *   sysctlbyname()                            -- no sysctl namespace, and the
 *     tests that use it are setting tunables (port range, randomisation) that
 *     are compile-time here.
 *   /dev/bpf, /dev/tap                        -- bpf_open() is a library call,
 *     not a device node; tests/tcpdrill/tapdev.c is the tap.
 *   jail(), pthreads, kqueue                  -- absent from the platform.
 *   ATF_TC_CLEANUP bodies run inline after the body, in the same process.
 *     ATF runs them in a separate one after the body's process is gone, so a
 *     cleanup that undoes global state still works and one that relies on being
 *     a fresh process does not.
 *
 * -------------------------------------------------------------- platform ---
 *
 * close(), read() and write() on a socket become CloseSocket(), recv() and
 * send().  This is the ONE place a #define shadows a libc name, it happens
 * after the test's own #include <unistd.h>, and it is why an adopted file needs
 * no edit at those call sites.  errno works because atf_main.c hands the
 * library a pointer to it with SetErrnoPtr(); it is the real newlib errno, so
 * strerror() reads it correctly.
 *
 * Formatting is newlib's vsnprintf, not RawDoFmt: the messages come from BSD
 * sources and are C-standard format strings, where RawDoFmt's %d is 16 bits and
 * its %s is not the same %s.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ATF_C_H
#define AMINETXDUO_ATF_C_H

#include <setjmp.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- types --- */

/*
 * ATF's atf_tc_t is opaque and reached only through the accessors below, so a
 * struct holding the name and the metadata this runner can answer is the whole
 * of it.  Bodies take it const because ATF's do.
 */
typedef struct atf_tc {
    const char *tc_name;
    void      (*tc_head)(struct atf_tc *);
    void      (*tc_body)(const struct atf_tc *);
    void      (*tc_cleanup)(const struct atf_tc *);
    int         tc_skip;                /* set by the head, read by the runner */
    const char *tc_skip_reason;
} atf_tc_t;

typedef struct atf_tp {
    atf_tc_t   *tp_cases;
    int         tp_count;
    int         tp_max;
} atf_tp_t;

/* ------------------------------------------------------------ the runner --- */

/* atf_main.c */
extern int  atfc_register(atf_tp_t *tp, const char *name,
                          void (*head)(atf_tc_t *),
                          void (*body)(const atf_tc_t *),
                          void (*cleanup)(const atf_tc_t *));
extern int  atfc_run(int (*add_tcs)(atf_tp_t *), const char *progname);
extern int  atfc_record(int ok, const char *what);
extern void atfc_recordf(int ok, const char *what, const char *fmt, ...);
extern void atfc_end(int how);          /* 0 pass, 1 fail, 2 skip */
extern void atfc_endf(int how, const char *fmt, ...);
extern void atfc_note(const char *fmt, ...);

/* The jump buffer the current case's fatal exits land in. */
extern jmp_buf atfc_case_jmp;

/* ---------------------------------------------------------- declarations --- */

/*
 * The `tc` parameter is unused in most bodies and heads.  Upstream's atf-c.h
 * marks it ATF_DEFS_ATTRIBUTE_UNUSED for that reason, and so does this, or an
 * adopted file warns under -Wextra for something it does correctly.
 */
#define ATF_UNUSED  __attribute__((unused))

#define ATF_TC(name)                                                          \
    static void atfc_head_##name(atf_tc_t *);                                 \
    static void atfc_body_##name(const atf_tc_t *)

#define ATF_TC_WITHOUT_HEAD(name)                                             \
    static void atfc_body_##name(const atf_tc_t *);                           \
    static void atfc_head_##name(atf_tc_t *tc ATF_UNUSED) { (void)tc; }

#define ATF_TC_WITH_CLEANUP(name)                                             \
    static void atfc_head_##name(atf_tc_t *);                                 \
    static void atfc_body_##name(const atf_tc_t *);                           \
    static void atfc_cleanup_##name(const atf_tc_t *)

#define ATF_TC_HEAD(name, tcarg)                                              \
    static void atfc_head_##name(atf_tc_t *tcarg ATF_UNUSED)
#define ATF_TC_BODY(name, tcarg)                                              \
    static void atfc_body_##name(const atf_tc_t *tcarg ATF_UNUSED)
#define ATF_TC_CLEANUP(name, tcarg)                                           \
    static void atfc_cleanup_##name(const atf_tc_t *tcarg ATF_UNUSED)

/*
 * A file that uses ATF_TC_WITH_CLEANUP gets its cleanup registered; one that
 * does not has no such symbol to name, which is why registration takes the
 * cleanup pointer explicitly and ATF_TP_ADD_TC passes NULL.  ATF itself works
 * this out with a second macro; here the two forms are spelled out.
 */
#define ATF_TP_ADD_TC(tp, name)                                               \
    atfc_register((tp), #name, atfc_head_##name, atfc_body_##name, NULL)

#define ATF_TP_ADD_TC_WITH_CLEANUP(tp, name)                                  \
    atfc_register((tp), #name, atfc_head_##name, atfc_body_##name,            \
                  atfc_cleanup_##name)

#define ATF_TP_ADD_TCS(tpname)                                                \
    static int atfc_add_tcs(atf_tp_t *tpname);                                \
    int main(void) { return atfc_run(atfc_add_tcs, ATF_STR(ATF_PROGNAME)); }  \
    static int atfc_add_tcs(atf_tp_t *tpname)

/*
 * Named in the banner.  A file that does not set it gets its own basename.
 *
 * A BARE TOKEN, stringified here, and not a string handed over already
 * quoted: -DATF_PROGNAME=\"tcp_socket\" is the only thing in this tree that
 * would put an escape in compile_commands.json, and tools/analyze.sh refuses
 * the file when it finds one rather than mis-parse it.
 */
#define ATF_STR_(x)     #x
#define ATF_STR(x)      ATF_STR_(x)

#ifndef ATF_PROGNAME
#  define ATF_PROGNAME  atf
#endif

#define atf_no_error()  0

/* --------------------------------------------------------------- checks --- */

/*
 * REQUIRE ends the case, CHECK does not.  Both record, so a CHECK failure still
 * lands in the count -- which is what makes the trailer's failure number the
 * same number ATF would have reported.
 */
#define ATF_CHECK(expr)         ((void)atfc_record(!!(expr), #expr))
#define ATF_REQUIRE(expr)                                                     \
    do { if (!atfc_record(!!(expr), #expr)) atfc_end(1); } while (0)

#define ATF_CHECK_MSG(expr, ...)                                              \
    atfc_recordf(!!(expr), #expr, __VA_ARGS__)
#define ATF_REQUIRE_MSG(expr, ...)                                            \
    do {                                                                      \
        int atfc_ok_ = !!(expr);                                              \
        atfc_recordf(atfc_ok_, #expr, __VA_ARGS__);                           \
        if (!atfc_ok_) atfc_end(1);                                           \
    } while (0)

#define ATF_CHECK_EQ(a, b)      ATF_CHECK_MSG((a) == (b), "%s != %s", #a, #b)
#define ATF_REQUIRE_EQ(a, b)    ATF_REQUIRE_MSG((a) == (b), "%s != %s", #a, #b)
#define ATF_CHECK_EQ_MSG(a, b, ...)   ATF_CHECK_MSG((a) == (b), __VA_ARGS__)
#define ATF_REQUIRE_EQ_MSG(a, b, ...) ATF_REQUIRE_MSG((a) == (b), __VA_ARGS__)

#define ATF_CHECK_STREQ(a, b)   ATF_CHECK_MSG(strcmp((a), (b)) == 0,          \
                                              "%s != %s", #a, #b)
#define ATF_REQUIRE_STREQ(a, b) ATF_REQUIRE_MSG(strcmp((a), (b)) == 0,        \
                                                "%s != %s", #a, #b)

/*
 * ATF_REQUIRE_ERRNO(err, expr): the expression must be true AND errno must be
 * err.  ATF's own version also insists the expression be a failed call, which
 * it cannot check either.
 */
#define ATF_CHECK_ERRNO(err, expr)                                            \
    do {                                                                      \
        int atfc_ok_ = !!(expr);                                              \
        atfc_recordf(atfc_ok_ && errno == (err), #expr,                       \
                     "expected errno %d, got %d", (int)(err), errno);         \
    } while (0)

#define ATF_REQUIRE_ERRNO(err, expr)                                          \
    do {                                                                      \
        int atfc_ok_ = !!(expr);                                              \
        int atfc_e_  = errno;                                                 \
        atfc_recordf(atfc_ok_ && atfc_e_ == (err), #expr,                     \
                     "expected errno %d, got %d", (int)(err), atfc_e_);       \
        if (!atfc_ok_ || atfc_e_ != (err)) atfc_end(1);                       \
    } while (0)

/* ------------------------------------------------------- case outcomes --- */

#define atf_tc_fail(...)            atfc_endf(1, __VA_ARGS__)
#define atf_tc_fail_nonfatal(...)   atfc_recordf(0, "atf_tc_fail_nonfatal", __VA_ARGS__)
#define atf_tc_skip(...)            atfc_endf(2, __VA_ARGS__)
#define atf_tc_pass()               atfc_end(0)
#define atf_tc_expect_fail(...)     atfc_note(__VA_ARGS__)

/* --------------------------------------------------------- metadata ------ */

extern void        atf_tc_set_md_var(atf_tc_t *tc, const char *name,
                                     const char *fmt, ...);
extern int         atf_tc_has_config_var(const atf_tc_t *tc, const char *name);
extern const char *atf_tc_get_config_var(const atf_tc_t *tc, const char *name);
extern const char *atf_tc_get_config_var_wd(const atf_tc_t *tc,
                                            const char *name, const char *def);

/* ------------------------------------------------------ platform bridge --- */

/*
 * After the test's own <unistd.h>.  A descriptor here is always a socket: ATF
 * netinet tests open no files, and the ones that do (the sysctl backup dance in
 * tcp_connect_port_test.c) are not adoptable for other reasons.
 */
#ifndef ATF_NO_SOCKET_ALIASES
#  include <proto/bsdsocket.h>
#  undef  close
#  define close(fd)             CloseSocket((LONG)(fd))
#  undef  read
#  define read(fd, b, n)        recv((LONG)(fd), (APTR)(b), (LONG)(n), 0)
#  undef  write
#  define write(fd, b, n)       send((LONG)(fd), (APTR)(b), (LONG)(n), 0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_ATF_C_H */
