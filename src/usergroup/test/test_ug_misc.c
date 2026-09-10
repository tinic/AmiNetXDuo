/*
 * AmiNetXDuo, host-side test for getpass(), GetSalt(), crypt() and the
 * utmp/lastlog stubs.
 *
 * THE ORDER IS THE PROPERTY.  ugl_getpass() writes the prompt, puts the
 * console in raw mode so it stops echoing, reads the password a character at
 * a time, and puts the mode back.  Turning echo off one Read too late has
 * already put a character of someone's password on the screen, and no count
 * of calls can say whether that happened.  shim/proto/dos.h therefore keeps
 * an ORDER log -- one character per call -- and the tests below assert
 * against it.
 *
 * ug_PassBuf[UG_PASSWORD_LEN + 1] is a member of the library base and
 * ug_SaltBuf[16] follows it (usergroup_internal.h:166-167), so a password
 * written past its end lands in ug_SaltBuf where no sanitizer has a redzone.
 * ug_SaltBuf is the canary.  GetSalt() is the other way round: it writes into
 * a CALLER buffer at a caller length, so that one is heap-allocated at
 * exactly the length passed and ASan owns the byte after it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_internal.h"
#include "usergroup_vectors.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- the shim */

struct Task *shim_current_task;
int          shim_forbid_depth;
int          shim_semaphore_depth;

SHIM_DOS_DEFINE_STATE;

/* ------------------------------------------------------------------ stubs */

void ug_set_err(struct UserGroupBase *base, LONG err) { base->ug_Err = err; }

static struct DosLibrary *stub_dosbase;

struct DosLibrary *ug_dos(struct UserGroupBase *base)
{
    (void)base;
    return stub_dosbase;
}

/* ---------------------------------------------------------------- harness */

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/*
 * A FUNCTION, not a macro body.  Written as a macro that tests `(got) ==
 * NULL` inline, the guard is dead for every ARRAY caller -- an array is never
 * null -- and gcc says so under -Waddress.  Taking a pointer parameter makes
 * the same guard real, because the array has decayed by the time it is
 * checked.  cmake/ci-warnings.cmake:40 documents the macro form as a defect.
 */
static void check_str(const char *got, const char *want,
                      const char *file, int line)
{
    checks++;

    if (got == NULL || strcmp(got, want) != 0)
    {
        failures++;
        printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n",
               file, line, want, (got != NULL) ? got : "(null)");
    }
}

#define CHECK_STR(got, want) check_str((got), (want), __FILE__, __LINE__)

static struct UgGlobal      g;
static struct UserGroupBase base;
static struct Process       me;

static void world_reset(void)
{
    memset(&g, 0, sizeof(g));
    memset(&base, 0, sizeof(base));
    memset(&me, 0, sizeof(me));

    me.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    shim_current_task = &me.pr_Task;

    base.ug_Global = &g;
    stub_dosbase   = (struct DosLibrary *)&g;

    /* The buffer that follows ug_PassBuf.  A password written past the end
       of ug_PassBuf lands here. */
    memset(base.ug_SaltBuf, 0x5A, sizeof(base.ug_SaltBuf));

    shim_dos_reset();
}

static int salt_canary_intact(void)
{
    ULONG i;

    for (i = 0; i < sizeof(base.ug_SaltBuf); i++)
    {
        if ((unsigned char)base.ug_SaltBuf[i] != 0x5A)
            return 0;
    }

    return 1;
}

/* Index of a character in the op log, -1 if absent. */
static int op_first(char c)
{
    const char *p = strchr(shim_dos_ops, c);

    return (p == NULL) ? -1 : (int)(p - shim_dos_ops);
}

static int op_last(char c)
{
    const char *p = strrchr(shim_dos_ops, c);

    return (p == NULL) ? -1 : (int)(p - shim_dos_ops);
}

static void console(const char *typed)
{
    shim_dos_add_file("CONSOLE:", typed, (long)strlen(typed));
}

/* ------------------------------------------------------------------ tests */

/*
 * THE ONE THAT MATTERS.  Echo must be off before the first character is read
 * and back on after the last, or the password is on the screen.
 */
static void test_getpass_echo_is_off_around_the_read(void)
{
    STRPTR pw;

    world_reset();
    console("hunter2\n");

    pw = ugl_getpass(&base, (STRPTR)"Password: ");

    CHECK_STR((char *)pw, "hunter2");
    CHECK(base.ug_Err == 0);

    /* open, prompt, raw, reads..., cooked, newline, close */
    CHECK(op_first('o') == 0);
    CHECK(op_first('1') >= 0);
    CHECK(op_first('r') >= 0);
    CHECK(op_first('1') < op_first('r'));       /* off BEFORE the first read */
    CHECK(op_last('0') > op_last('r'));         /* on again AFTER the last   */
    CHECK(op_last('c') == shim_dos_oplen - 1);  /* and the console is closed */

    /* The prompt was written, and the trailing newline after it. */
    CHECK(strncmp(shim_dos_written, "Password: ", 10) == 0);
    CHECK(shim_dos_written[shim_dos_writelen - 1] == '\n');

    /* The password itself was never written back out. */
    CHECK(strstr(shim_dos_written, "hunter2") == NULL);

    CHECK(salt_canary_intact());
}

/* A password longer than the buffer is truncated, not written past the end.
   ug_SaltBuf is the redzone. */
static void test_getpass_truncates(void)
{
    static char typed[UG_PASSWORD_LEN + 64];
    STRPTR      pw;
    int         i;

    world_reset();

    for (i = 0; i < UG_PASSWORD_LEN + 62; i++)
        typed[i] = 'x';
    typed[UG_PASSWORD_LEN + 62] = '\n';
    typed[UG_PASSWORD_LEN + 63] = '\0';

    console(typed);
    pw = ugl_getpass(&base, NULL);

    CHECK(strlen((char *)pw) == UG_PASSWORD_LEN);
    CHECK(salt_canary_intact());
    CHECK(base.ug_PassBuf[UG_PASSWORD_LEN] == '\0');
}

/* Backspace removes a character, and a backspace with nothing to remove must
   not take the length below zero. */
static void test_getpass_backspace(void)
{
    STRPTR pw;

    world_reset();
    console("ab\b\bc\n");
    pw = ugl_getpass(&base, NULL);
    CHECK_STR((char *)pw, "c");

    /* Nothing but backspaces, from an empty buffer. */
    world_reset();
    console("\b\b\b\b\n");
    pw = ugl_getpass(&base, NULL);
    CHECK_STR((char *)pw, "");
    CHECK(salt_canary_intact());

    /* DEL is the same as backspace. */
    world_reset();
    console("ab\177c\n");
    pw = ugl_getpass(&base, NULL);
    CHECK_STR((char *)pw, "ac");

    /*
     * A backspace with nothing to delete must not take the index below zero.
     * It is not enough to check the all-backspace case: there the answer is
     * the empty string either way.  A LEADING backspace followed by a real
     * character is what separates them -- with the guard gone that character
     * is written to ug_PassBuf[-1], BEFORE the buffer, and the answer comes
     * back empty.
     */
    world_reset();
    console("\bc\n");
    pw = ugl_getpass(&base, NULL);
    CHECK_STR((char *)pw, "c");
}

/* A carriage return ends the line as well as a newline: a console can send
   either. */
static void test_getpass_cr_ends_line(void)
{
    STRPTR pw;

    world_reset();
    console("secret\rmore\n");
    pw = ugl_getpass(&base, NULL);
    CHECK_STR((char *)pw, "secret");
}

/* End of input with no terminator at all is still a password, not a hang. */
static void test_getpass_eof(void)
{
    STRPTR pw;

    world_reset();
    console("abc");
    pw = ugl_getpass(&base, NULL);
    CHECK_STR((char *)pw, "abc");
    CHECK(op_last('c') == shim_dos_oplen - 1);  /* still closed */
}

/*
 * If raw mode is refused, the mode is NOT put back: restoring a mode that was
 * never set would turn echo ON for a console that had it off for its own
 * reasons.
 */
static void test_getpass_setmode_refused(void)
{
    STRPTR pw;

    world_reset();
    console("abc\n");
    shim_dos_setmode_fails = 1;

    pw = ugl_getpass(&base, NULL);

    CHECK_STR((char *)pw, "abc");               /* still reads */
    CHECK(op_first('1') == -1);                 /* never went raw */
    CHECK(op_first('0') == -1);                 /* and did not restore */
    CHECK(op_last('c') == shim_dos_oplen - 1);
}

/* No console, no dos.library, and a bare Task are each an empty answer with a
   reason -- never NULL, which a caller would dereference. */
static void test_getpass_no_console(void)
{
    struct Task bare;
    STRPTR      pw;

    world_reset();                              /* CONSOLE: not registered */
    pw = ugl_getpass(&base, NULL);
    CHECK(pw != NULL);
    CHECK_STR((char *)pw, "");
    CHECK(base.ug_Err == UG_ENOTTY);

    world_reset();
    console("abc\n");
    stub_dosbase = NULL;
    pw = ugl_getpass(&base, NULL);
    CHECK(pw != NULL);
    CHECK_STR((char *)pw, "");
    CHECK(base.ug_Err == UG_ENOSYS);
    CHECK(shim_dos_opens == 0);

    world_reset();
    console("abc\n");
    memset(&bare, 0, sizeof(bare));
    bare.tc_Node.ln_Type = NT_TASK;
    shim_current_task = &bare;
    pw = ugl_getpass(&base, NULL);
    CHECK(pw != NULL);
    CHECK_STR((char *)pw, "");
    CHECK(base.ug_Err == UG_ENOTTY);
    CHECK(shim_dos_opens == 0);                 /* never reached Open */
}

/* GetSalt writes into a CALLER buffer at a CALLER length. */
static void test_getsalt_bounds(void)
{
    struct ug_passwd user;
    UBYTE           *buf;
    UBYTE           *r;

    world_reset();
    memset(&user, 0, sizeof(user));
    user.pw_passwd = (char *)"abZZZZ";

    /* Exactly three is the smallest that holds a salt. ASan owns buf[3]. */
    buf = malloc(3);
    CHECK(buf != NULL);
    r = ugl_GetSalt(&base, &user, buf, 3);
    CHECK(r == buf);
    CHECK_STR((char *)buf, "ab");
    CHECK(base.ug_Err == 0);
    free(buf);

    /* Two is too small: the terminator only, and ERANGE.  buf[1] is ASan's. */
    buf = malloc(1);
    CHECK(buf != NULL);
    r = ugl_GetSalt(&base, &user, buf, 1);
    CHECK(r == buf);
    CHECK(buf[0] == '\0');
    CHECK(base.ug_Err == UG_ERANGE);
    free(buf);

    /* A zero length, and a NULL buffer, are both EFAULT and write nothing. */
    CHECK(ugl_GetSalt(&base, &user, NULL, 16) == NULL);
    CHECK(base.ug_Err == UG_EFAULT);

    buf = malloc(1);
    CHECK(ugl_GetSalt(&base, &user, buf, 0) == NULL);
    CHECK(base.ug_Err == UG_EFAULT);
    free(buf);
}

/* With no hash to take one from, the salt is "**", which is still a usable
   crypt() argument. */
static void test_getsalt_defaults(void)
{
    struct ug_passwd user;
    UBYTE            buf[8];

    world_reset();

    CHECK(ugl_GetSalt(&base, NULL, buf, sizeof(buf)) == buf);
    CHECK_STR((char *)buf, "**");

    memset(&user, 0, sizeof(user));
    user.pw_passwd = (char *)"";
    CHECK(ugl_GetSalt(&base, &user, buf, sizeof(buf)) == buf);
    CHECK_STR((char *)buf, "**");

    /* One character is not a salt either. */
    user.pw_passwd = (char *)"a";
    CHECK(ugl_GetSalt(&base, &user, buf, sizeof(buf)) == buf);
    CHECK_STR((char *)buf, "**");

    user.pw_passwd = NULL;
    CHECK(ugl_GetSalt(&base, &user, buf, sizeof(buf)) == buf);
    CHECK_STR((char *)buf, "**");
}

/* crypt() is not implemented and says so, but still returns a usable string
   rather than NULL. */
static void test_crypt_is_enosys(void)
{
    UBYTE *r;

    world_reset();
    r = ugl_crypt(&base, (UBYTE *)"password", (UBYTE *)"ab");

    CHECK(r != NULL);
    CHECK_STR((char *)r, "*");
    CHECK(base.ug_Err == UG_ENOSYS);
    CHECK(salt_canary_intact());
}

/*
 * There is no utmp or lastlog on AmigaOS.  An EMPTY database is the right
 * answer and not a failure: a NULL from getutent() is what "no more entries"
 * looks like, so a caller walks zero records and carries on.  ug_Err must be
 * 0, or a caller that checks it treats the empty walk as an error.
 */
static void test_utmp_is_empty_not_failed(void)
{
    world_reset();

    ugl_setutent(&base);
    base.ug_Err = 12345;
    CHECK(ugl_getutent(&base) == NULL);
    CHECK(base.ug_Err == 0);
    ugl_endutent(&base);

    base.ug_Err = 12345;
    CHECK(ugl_getlastlog(&base, 0) == NULL);
    CHECK(base.ug_Err == 0);

    base.ug_Err = 12345;
    CHECK(ugl_setlastlog(&base, 0, (STRPTR)"root", (STRPTR)"here") == 0);
    CHECK(base.ug_Err == 0);
}

/*
 * THE THREE HELPERS, WHICH UNTIL NOW EVERY TEST STUBBED.
 *
 * They were in ug_library.c, which carries a raw __asm__() block and cannot be
 * compiled off-target, so test_ug_db, test_ug_ids and this file each supplied
 * their own using libc strcmp/strlen.  Those are NOT the same functions: the
 * shipped ones define behaviour where libc has none, and every caller in the
 * library relies on it -- ug_db.c compares pw_name against a caller string that
 * may be NULL, and ug_context.c copies into a fixed cr_login.
 */
static void test_string_helpers(void)
{
    char buf[8];

    /* NULL is a length, not a crash. */
    CHECK(ug_strlen(NULL) == 0);
    CHECK(ug_strlen("") == 0);
    CHECK(ug_strlen("abc") == 3);

    /* NEVER negative for NULL, and equal only when both are.  A caller that
       sorted on the sign would order NULL first with libc and last here. */
    CHECK(ug_strcmp(NULL, NULL) == 0);
    CHECK(ug_strcmp(NULL, "a") == 1);
    CHECK(ug_strcmp("a", NULL) == 1);
    CHECK(ug_strcmp("a", "a") == 0);
    CHECK(ug_strcmp("a", "b") < 0);
    CHECK(ug_strcmp("b", "a") > 0);
    /* High bytes compare UNSIGNED: a latin-1 name must not sort before ASCII. */
    CHECK(ug_strcmp("\xe4", "a") > 0);

    /* Always terminates, never writes size or beyond. */
    memset(buf, 'Z', sizeof(buf));
    ug_strncpy(buf, "abcdefghij", 4);
    CHECK_STR(buf, "abc");
    CHECK(buf[4] == 'Z');               /* one past the size is untouched */

    memset(buf, 'Z', sizeof(buf));
    ug_strncpy(buf, "ab", sizeof(buf));
    CHECK_STR(buf, "ab");

    /* A NULL source still terminates the destination rather than leaving it. */
    memset(buf, 'Z', sizeof(buf));
    ug_strncpy(buf, NULL, sizeof(buf));
    CHECK(buf[0] == '\0');

    /* size 0 and a NULL destination write nothing at all. */
    memset(buf, 'Z', sizeof(buf));
    ug_strncpy(buf, "abc", 0);
    CHECK(buf[0] == 'Z');
    ug_strncpy(NULL, "abc", sizeof(buf));   /* must not fault */

    /* size 1 is the terminator alone. */
    memset(buf, 'Z', sizeof(buf));
    ug_strncpy(buf, "abc", 1);
    CHECK(buf[0] == '\0');
    CHECK(buf[1] == 'Z');
}

int main(void)
{
    test_getpass_echo_is_off_around_the_read();
    test_getpass_truncates();
    test_getpass_backspace();
    test_getpass_cr_ends_line();
    test_getpass_eof();
    test_getpass_setmode_refused();
    test_getpass_no_console();
    test_getsalt_bounds();
    test_getsalt_defaults();
    test_crypt_is_enosys();
    test_utmp_is_empty_not_failed();
    test_string_helpers();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
