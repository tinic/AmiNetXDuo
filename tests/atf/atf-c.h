/*
 * <atf-c.h> for AmigaOS -- enough of FreeBSD's ATF to run its netinet tests.
 * Cases run in sequence in one process; fatal assertions longjmp out.
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

extern jmp_buf atfc_case_jmp;

/* ---------------------------------------------------------- declarations --- */

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
 * ATF_PROGNAME must be a BARE TOKEN, stringified here: an escaped quote in
 * compile_commands.json makes tools/analyze.sh reject the file.
 */
#define ATF_STR_(x)     #x
#define ATF_STR(x)      ATF_STR_(x)

#ifndef ATF_PROGNAME
#  define ATF_PROGNAME  atf
#endif

#define atf_no_error()  0

/* --------------------------------------------------------------- checks --- */

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

/* Must be included after the test's own <unistd.h>. */
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
