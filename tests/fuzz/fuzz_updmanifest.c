/*
 * AmiNetXDuo, host fuzz driver for the update manifest and signature file
 * parsers in src/tools/updmanifest.c.
 *
 * Usage:
 *   fuzz_updmanifest -s             the seed cases
 *   fuzz_updmanifest -r SEED COUNT  built-in random generator, no corpus needed
 *
 * Each input is parsed from a heap block of exactly its size, so ASan sees a
 * read one past the end.
 */

#include "updmanifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- rng */

static unsigned long fz_state;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(fz_state >> 33);
}

static unsigned fz_below(unsigned n)
{
    return (n == 0) ? 0 : (fz_rand() % n);
}

/* ------------------------------------------------------------- the seeds */

#define H1 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const char *fz_lines[] = {
    "format=1",
    "# comment",
    "product=AmiNetXDuo",
    "version=1.0.0-rc5",
    "channel=prerelease",
    "certificates.prev=" H1,
    "container=full 123456 " H1 " AmiNetXDuo-1.0.0-rc5-full.update.lha",
    "container=micro 1 " H1 " m.lha",
    "file=full LIBS 98765 " H1 " bsdsocket.library",
    "file=full DEVS/Networks 4321 " H1 " anxa2065.device",
    "file=full C 0 " H1 " AmiNetXDuo Tools/fetch",
    "file=full certificates 200000 " H1 " certificates",
    "file=micro LIBS 50000 " H1 " bsdsocket.library",
    "x.later=1",
};

#define FZ_NLINES   (sizeof(fz_lines) / sizeof(fz_lines[0]))

/* Bytes the mutator splices in: the ones the parser decides on. */
static const char *fz_tokens[] = {
    "..", "/", ":", "//", " ", "=", "\n", "\r", "#", "0", "4294967296",
    "full", "fat", "nano", "LIBS", "C", "end=", "format=1\n", "-rc10",
    "release", "file=", "container=", H1, "g",
};

#define FZ_NTOKENS  (sizeof(fz_tokens) / sizeof(fz_tokens[0]))

#define FZ_TEXT_MAX 4096

/* ---------------------------------------------------------- the checking */

static unsigned long fz_case;
static unsigned long fz_accepted;
static UpdFile       fz_files[8];

static int fz_fail(const char *what, const char *t, unsigned long n)
{
    unsigned long i;

    printf("fuzz_updmanifest: %s at case %lu\n", what, fz_case);
    printf("  input (%lu bytes): ", n);
    for (i = 0; i < n && i < 400UL; i++)
    {
        unsigned c = (unsigned char)t[i];

        if (c >= 0x20 && c < 0x7f && c != '\\')
            printf("%c", (char)c);
        else
            printf("\\x%02x", c);
    }
    printf("\n");
    return 1;
}

static int fz_inside(const char *p, unsigned long n,
                     const char *t, unsigned long len)
{
    return p >= t && n <= len && (unsigned long)(p - t) <= len - n;
}

static int fz_one(const char *src, unsigned long len)
{
    char        *t = malloc(len ? len : 1);
    UpdManifest  m;
    UpdWant      w;
    UpdSigLine   sl[UPD_SIG_LINES];
    unsigned int nsl = 99, i;
    UpdResult    r;
    int          bad = 0;

    if (t == NULL)
        return 0;
    memcpy(t, src, len);

    upd_ver_parse("1.0.0-rc4", 9, &w.installed);
    w.channel = UPD_CHANNEL_PRERELEASE;
    w.flavour = (unsigned char)(fz_case % UPD_FLAVOUR_COUNT);

    r = upd_manifest_parse(t, len, &w, &m, fz_files, 8);

    if ((unsigned)r >= UPD_RESULT_COUNT)
        bad = fz_fail("a result outside the enum", t, len);

    if (r == UPD_OK)
    {
        char   v[UPD_VER_MAX];
        UpdVer back;

        fz_accepted++;

        if (len == 0 || t[len - 1] != '\n')
            bad = fz_fail("accepted input without a final LF", t, len);
        if (m.nfiles > 8)
            bad = fz_fail("more files than the table", t, len);
        if (!fz_inside(m.container, m.container_len, t, len) ||
            m.container_len == 0)
            bad = fz_fail("container name outside the input", t, len);

        for (i = 0; i < m.nfiles && i < 8; i++)
        {
            const UpdFile *f = &fz_files[i];
            unsigned int   k;

            if (!fz_inside(f->path, f->path_len, t, len) ||
                f->path_len == 0 || f->path_len > UPD_PATH_MAX ||
                f->cls >= UPD_CLASS_COUNT)
                bad = fz_fail("a bad file entry", t, len);
            else if (f->path[0] == '/' || f->path[f->path_len - 1] == '/' ||
                     memchr(f->path, ':', f->path_len) != NULL)
                bad = fz_fail("accepted a path it must refuse", t, len);

            for (k = 0; k + 1 < f->path_len; k++)
            {
                if (f->path[k] == '/' && f->path[k + 1] == '/')
                    bad = fz_fail("accepted a // path", t, len);
            }
        }

        upd_ver_format(&m.version, v);
        if (!upd_ver_parse(v, (unsigned long)strlen(v), &back) ||
            upd_ver_cmp(&back, &m.version) != 0 ||
            upd_ver_cmp(&m.version, &w.installed) <= 0)
            bad = fz_fail("an accepted version that does not round-trip",
                          t, len);
    }

    r = upd_sig_parse(t, len, sl, &nsl);
    if (r == UPD_OK && (nsl == 0 || nsl > UPD_SIG_LINES))
        bad = fz_fail("a signature line count out of range", t, len);
    if (r != UPD_OK && nsl != 0)
        bad = fz_fail("a refused signature file left a count", t, len);

    free(t);
    return bad;
}

/* --------------------------------------------------------- the generator */

/* end= over the lines before the last one, appended or replacing it. */
static unsigned long fz_end(char *out, unsigned long n)
{
    unsigned long lines = 0, i;

    for (i = 0; i < n; i++)
        lines += (out[i] == '\n');

    if (n + 16UL < FZ_TEXT_MAX)
        n += (unsigned long)snprintf(out + n, 16, "end=%lu\n", lines);

    return n;
}

static unsigned long fz_make(char *out)
{
    unsigned long n = 0;
    unsigned      k, lines = fz_below(16) + 1;

    if (fz_below(16) == 0)
    {
        unsigned long len = fz_below(600);

        for (n = 0; n < len; n++)
            out[n] = (char)fz_rand();
        return n;
    }

    /* Half the time the seed manifest in order, so most of what follows
       is a small step from one that parses. */
    if (fz_below(2) == 0)
        lines = FZ_NLINES;

    for (k = 0; k < lines; k++)
    {
        const char   *l = (lines == FZ_NLINES) ? fz_lines[k]
                          : (k == 0 && fz_below(8) != 0)
                              ? fz_lines[0]
                              : fz_lines[fz_below(FZ_NLINES)];
        unsigned long ln = strlen(l);

        if (n + ln + 2UL >= FZ_TEXT_MAX)
            break;
        memcpy(out + n, l, ln);
        n += ln;
        out[n++] = '\n';
    }

    /* Splices, overwrites and cuts. */
    for (k = fz_below(4); k > 0; k--)
    {
        const char   *tok = fz_tokens[fz_below(FZ_NTOKENS)];
        unsigned long tn = strlen(tok);
        unsigned long at = fz_below((unsigned)n + 1);

        switch (fz_below(3))
        {
            case 0:
                if (n + tn < FZ_TEXT_MAX - 32UL)
                {
                    memmove(out + at + tn, out + at, n - at);
                    memcpy(out + at, tok, tn);
                    n += tn;
                }
                break;
            case 1:
                if (n > 0)
                    out[fz_below((unsigned)n)] = (char)fz_rand();
                break;
            default:
                if (at < n)
                {
                    unsigned long cut = fz_below((unsigned)(n - at)) + 1;

                    memmove(out + at, out + at + cut, n - at - cut);
                    n -= cut;
                }
                break;
        }
    }

    if (fz_below(4) != 0)
        n = fz_end(out, n);

    if (fz_below(8) == 0 && n > 0)
        n = fz_below((unsigned)n);

    return n;
}

/* ---------------------------------------------------------------- main */

static int fz_seeds(void)
{
    char          text[FZ_TEXT_MAX];
    unsigned long n = 0, i, cases = 0;

    /* The whole seed set, then every prefix of it. */
    for (i = 0; i < FZ_NLINES; i++)
    {
        n += (unsigned long)snprintf(text + n, sizeof(text) - n, "%s\n",
                                     fz_lines[i]);
    }
    n = fz_end(text, n);

    for (i = 0; i <= n; i++)
    {
        unsigned long f;

        for (f = 0; f < UPD_FLAVOUR_COUNT; f++)
        {
            fz_case = i * UPD_FLAVOUR_COUNT + f;
            if (fz_one(text, i))
                return 1;
            cases++;
        }
    }

    if (fz_accepted != 2)
    {
        printf("fuzz_updmanifest: the seed manifest was not accepted\n");
        return 1;
    }

    {
        static const char *sigs[] = {
            "key=a sig=" H1 H1 "\n",
            "key=a sig=" H1 H1 "\nkey=b sig=" H1 H1 "\n",
            "key=a sig=" H1 "\n",
            "key=0123456789abcdefX sig=" H1 H1 "\n",
            "key=a sig=" H1 H1,
            NULL
        };

        for (i = 0; sigs[i] != NULL; i++)
        {
            fz_case = cases++;
            if (fz_one(sigs[i], (unsigned long)strlen(sigs[i])))
                return 1;
        }
    }

    printf("fuzz_updmanifest: %lu seed case(s), %lu accepted, clean\n",
           cases, fz_accepted);
    return 0;
}

int main(int argc, char **argv)
{
    char          text[FZ_TEXT_MAX];
    unsigned long seed  = 1;
    unsigned long count = 10000;
    unsigned long n;
    int           i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            seed  = strtoul(argv[++i], NULL, 0);
            count = strtoul(argv[++i], NULL, 0);
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            return fz_seeds();
        }
    }

    fz_state = seed;

    for (n = 0; n < count; n++)
    {
        unsigned long len = fz_make(text);

        fz_case = n;
        if (fz_one(text, len))
            return 1;
    }

    printf("fuzz_updmanifest: %lu case(s) from seed %lu, %lu accepted, "
           "clean\n", count, seed, fz_accepted);
    return 0;
}
