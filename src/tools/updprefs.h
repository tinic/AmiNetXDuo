/*
 * updprefs: one key=value line of ENVARC:AmiNetXDuo/Update set, every other
 * line kept -- what Install-AmiNetXDuo writes through `InstallSameFile
 * <file> KEY=VALUE SETKEY`, so that a reinstall sets AUTO= and keeps a
 * CHANNEL= or MIRROR= the user wrote.  NetPrefs' Updates page saves AUTO=
 * and CHANNEL= the same way, and UpdateAmiNetXDuo its state's dates.  The
 * file's syntax is updcheck.h's (keys without case, LF or CR LF lines).
 *
 * Header only, as InstallSameFile and NetPrefs link nothing of the
 * updater; compiled
 * natively by src/tools/test/test_updcheck.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_UPDPREFS_H
#define AMINETXDUO_UPDPREFS_H

static int upd_prefs_fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* `l` (`n` bytes, no LF) sets the key `line` names: its text up to '=',
   without case, spaces before the '=' allowed. */
static int upd_prefs_is_key(const char *l, unsigned long n, const char *line,
                            unsigned long klen)
{
    unsigned long i = 0, k;

    while (i < n && (l[i] == ' ' || l[i] == '\t'))
        i++;
    for (k = 0; k < klen; k++, i++)
        if (i >= n || upd_prefs_fold((unsigned char)l[i]) !=
                      upd_prefs_fold((unsigned char)line[k]))
            return 0;
    while (i < n && (l[i] == ' ' || l[i] == '\t'))
        i++;
    return i < n && l[i] == '=';
}

/*
 * `old` (`oldlen` bytes, may be empty) with `line` ("KEY=VALUE", NUL
 * ended) in place of the first line that sets KEY, later ones dropped, or
 * appended when none does, into `out`.  Every other line is kept as it
 * was; the result ends with a LF.  The length, or 0 when `line` has no key
 * or the result does not fit `outlen`.
 */
static unsigned long upd_prefs_set(const char *old, unsigned long oldlen,
                                   const char *line, char *out,
                                   unsigned long outlen)
{
    unsigned long klen = 0, llen = 0, i = 0, at = 0, n, k;
    int           done = 0;

    while (line[llen] != '\0')
        llen++;
    while (klen < llen && line[klen] != '=')
        klen++;
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
        klen--;
    if (klen == 0 || klen >= llen)
        return 0;

    while (i < oldlen)
    {
        const char   *l = old + i;
        unsigned long body;

        for (n = 0; i + n < oldlen && l[n] != '\n'; n++)
            ;
        body = (n > 0 && l[n - 1] == '\r') ? n - 1 : n;
        if (upd_prefs_is_key(l, body, line, klen))
        {
            if (!done)
            {
                if (at + llen + 1 > outlen)
                    return 0;
                for (k = 0; k < llen; k++)
                    out[at++] = line[k];
                out[at++] = '\n';
                done = 1;
            }
        }
        else
        {
            if (at + n + 1 > outlen)
                return 0;
            for (k = 0; k < n; k++)
                out[at++] = l[k];
            out[at++] = '\n';
        }
        i += n + 1;
    }
    if (!done)
    {
        if (at + llen + 1 > outlen)
            return 0;
        for (k = 0; k < llen; k++)
            out[at++] = line[k];
        out[at++] = '\n';
    }
    return at;
}

/*
 * Two lines set in one pass, as NetPrefs' Save writes AUTO= and CHANNEL=,
 * and the updater the state's last_check= with later= or skip=: `tmp` and
 * `out` hold `cap` bytes each.  The length, or 0 as upd_prefs_set().
 */
static inline unsigned long upd_prefs_set2(const char *old, unsigned long oldlen,
                                           const char *line1, const char *line2,
                                           char *tmp, char *out,
                                           unsigned long cap)
{
    unsigned long n = upd_prefs_set(old, oldlen, line1, tmp, cap);

    return n == 0 ? 0 : upd_prefs_set(tmp, n, line2, out, cap);
}

/*
 * The value of the first line that sets `key` (without case), its spaces
 * and a CR around it dropped, into `out` (`outlen` bytes, NUL ended).  1,
 * or 0 when no line sets it or the value does not fit.
 */
static inline int upd_prefs_get(const char *text, unsigned long len,
                                const char *key, char *out,
                                unsigned long outlen)
{
    unsigned long klen = 0, i = 0, n, b, e, k;

    while (key[klen] != '\0')
        klen++;
    while (i < len)
    {
        const char *l = text + i;

        for (n = 0; i + n < len && l[n] != '\n'; n++)
            ;
        if (upd_prefs_is_key(l, n, key, klen))
        {
            for (b = 0; l[b] != '='; b++)
                ;
            b++;
            e = n;
            while (b < e && (l[b] == ' ' || l[b] == '\t'))
                b++;
            while (e > b && (l[e - 1] == ' ' || l[e - 1] == '\t' ||
                             l[e - 1] == '\r'))
                e--;
            if (e - b + 1 > outlen)
                return 0;
            for (k = 0; k < e - b; k++)
                out[k] = l[b + k];
            out[k] = '\0';
            return 1;
        }
        i += n + 1;
    }
    return 0;
}

#endif /* AMINETXDUO_UPDPREFS_H */
