/*
 * AmiNetXDuo, host fuzz driver for DEVS:Internet/passwd and .../group.
 *
 * Both files are read on the first getpwnam()/getgrnam() of every program
 * that opens usergroup.library, and neither is written by us, an ftp or
 * ssh port hands them to whatever the machine's owner, or whatever unpacked
 * an archive into DEVS:, left there. There is no MMU underneath, so a write
 * past an arena is not a fault but somebody else's memory.
 *
 * The group parser sizes an arena in one pass and fills it in another, which
 * is the shape the netdb alias pool had when it overran: a disagreement
 * between the two passes is invisible on the Amiga and immediate here. Its
 * particular disagreement was line endings, ug_next_line() ends a line on
 * '\r' as well as '\n', and the sizing pass counted only '\n', so a
 * CR-terminated file sized for one line and parsed dozens.
 *
 * Usage:
 *   fuzz_usergroup < case         the bytes as both files
 *   fuzz_usergroup -g < case      the group file alone
 *   fuzz_usergroup -p < case      the passwd file alone
 *   fuzz_usergroup -s             the seed cases, including the CR regression
 *   fuzz_usergroup -r SEED COUNT  built-in random generator, no corpus needed
 */

#include "ug_parse.h"
#include "aminetxduo/compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    /*
     * malloc, not calloc: the arena must arrive dirty, so that a gr_mem
     * vector the parser failed to terminate is walked off the end under
     * ASan rather than stopping on a zero somebody else paid for.
     */
    p = malloc(size);
    if (p != NULL)
    {
        memset(p, 0xA5, size);
        stub_outstanding++;
    }

    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID)
{
    return stub_outstanding;
}

VOID ami_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

/* ------------------------------------------------------------- the drives */

/* Every string the parser produced is read into this, so nothing is elided. */
static volatile size_t fz_sink;

/*
 * Walking every gr_mem vector is the assertion, not decoration: the arena
 * overrun this driver exists for corrupts memory during the parse but leaves
 * the records looking fine, and it is the walk to the NULL terminator that
 * reads the slots ASan is guarding.
 */
static void fz_run_group(const char *data, size_t len)
{
    struct UgDatabase *db = (struct UgDatabase *)calloc(1, sizeof(*db));
    char *text = (char *)malloc(len + 1);
    UWORD i;

    if (db == NULL || text == NULL)
    {
        free(db);
        free(text);
        return;
    }

    memcpy(text, data, len);
    text[len] = '\0';

    ug_db_parse_group(db, text, (ULONG)len);

    for (i = 0; i < db->gr_count; i++)
    {
        char **mem = db->gr[i].gr_mem;
        unsigned n = 0;

        if (mem == NULL)
            abort();

        while (mem[n] != NULL)
        {
            fz_sink += strlen(mem[n]);
            n++;
        }
    }

    if (db->gr_members != NULL)
        ami_free(db->gr_members);
    free(text);
    free(db);
}

static void fz_run_passwd(const char *data, size_t len)
{
    struct UgDatabase *db = (struct UgDatabase *)calloc(1, sizeof(*db));
    char *text = (char *)malloc(len + 1);
    UWORD i;

    if (db == NULL || text == NULL)
    {
        free(db);
        free(text);
        return;
    }

    memcpy(text, data, len);
    text[len] = '\0';

    ug_db_parse_passwd(db, text);

    for (i = 0; i < db->pw_count; i++)
    {
        fz_sink += strlen(db->pw[i].pw_name);
        fz_sink += strlen(db->pw[i].pw_passwd);
        fz_sink += strlen(db->pw[i].pw_gecos);
        fz_sink += strlen(db->pw[i].pw_dir);
        fz_sink += strlen(db->pw[i].pw_shell);
    }

    free(text);
    free(db);
}

static void fz_run_once(const char *data, size_t len, int which)
{
    if (which != 'p')
        fz_run_group(data, len);
    if (which != 'g')
        fz_run_passwd(data, len);

    if (ami_alloc_count() != 0)
        abort();
}

/* ------------------------------------------------------------- seed cases */

/*
 * The regression case is first and is the reason this file exists. Sixty-four
 * CR-terminated member-less groups contain no '\n' at all, so the sizing pass
 * that counted '\n' alone allocated four pointers while the parse wrote
 * sixty-four NULL terminators, 240 bytes past a 16-byte block on the target.
 */
static void fz_seeds(void)
{
    static const char *const cases[] =
    {
        "a:*:0:\rb:*:1:\rc:*:2:\rd:*:3:\re:*:4:\rf:*:5:\r",
        "wheel:*:0:root,jane\rusers:*:100:jane\rguests:*:200:\r",
        "wheel:*:0:root,jane\r\nusers:*:100:jane\r\n",
        "wheel:*:0:root\rusers:*:100:jane\r\nguests:*:200:\n",
        "\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r",
        ",,,,,,,,,,,,,,,,\r",
        "a:*:0:x,y,z\r",
        "a:*:0:x,y,z",
        "#\r#\r#\r#\r#\r#\r#\r#\r",
        ":\r:\r:\r:\r:\r:\r:\r:\r",
        "",
        "root:*:0:0:AmigaOS user:SYS::\rjane:*:1000:100:J:W:S:\r",
        "root\rjane:*\rbob:*:1\r"
    };
    char big[4096];
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        fz_run_once(cases[i], strlen(cases[i]), 0);

    /* UG_MAX_GROUP CR-terminated lines: the count the report was written on. */
    big[0] = '\0';
    for (i = 0; i < UG_MAX_GROUP; i++)
    {
        char line[24];

        snprintf(line, sizeof(line), "g%u:*:%u:\r", i, i);
        strcat(big, line);
    }
    fz_run_once(big, strlen(big), 'g');

    /* The same, with members, so the member writes are pressured as well. */
    big[0] = '\0';
    for (i = 0; i < UG_MAX_GROUP; i++)
    {
        char line[40];

        snprintf(line, sizeof(line), "g%u:*:%u:root,jane,bob\r", i, i);
        strcat(big, line);
    }
    fz_run_once(big, strlen(big), 'g');
}

/* ------------------------------------------------------- random generator */

static unsigned long fz_state;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(fz_state >> 33);
}

/*
 * Atoms rather than uniform noise, for the same reason fuzz_config uses them:
 * this grammar is line breaks, colons and commas, and random bytes reach
 * almost none of it. All four terminators are in the set on purpose.
 */
static const char *const fz_atoms[] =
{
    "\n", "\r", "\r\n", "\n\r", ":", ",", "#", " ", "\t", "*",
    "a", "Z", "0", "9", "root", "wheel", "jane", "users", "guests",
    "SYS:", "C:Shell", "AmigaOS user", "-1", "+7", "4294967296",
    "::::::", ",,,,,,", "a:*:0:", "a:*:0:x,y,z", ":*:0:",
    "\x7f", "\x80", "\xff", "%s"
};

static size_t fz_generate(char *out, size_t cap)
{
    size_t len = 0;
    unsigned atoms = fz_rand() % 200u + 1u;

    while (atoms-- > 0)
    {
        const char *a = fz_atoms[fz_rand() % (sizeof(fz_atoms) /
                                              sizeof(fz_atoms[0]))];
        size_t      n = strlen(a);

        if (len + n >= cap)
            break;

        memcpy(out + len, a, n);
        len += n;
    }

    return len;
}

int main(int argc, char **argv)
{
    static char buf[65536];
    int which = 0;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-g") == 0)
        {
            which = 'g';
        }
        else if (strcmp(argv[i], "-p") == 0)
        {
            which = 'p';
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            fz_seeds();
            return 0;
        }
        else if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;

            fz_seeds();
            fz_state = seed;

            for (n = 0; n < count; n++)
            {
                size_t len = fz_generate(buf, sizeof(buf));

                fz_run_once(buf, len, which);
            }

            return 0;
        }
    }

    {
        size_t len = fread(buf, 1, sizeof(buf), stdin);

        fz_run_once(buf, len, which);
    }

    return 0;
}
