/*
 * AmiNetXDuo -- text handling for the configuration parsers.
 *
 * No dos.library, no newlib: these are user-edited files and this code runs
 * inside a shared library, so everything here works on a plain memory buffer
 * and uses only self-contained string helpers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

static char ami_cfg_empty[] = "";

/* ------------------------------------------------------------ tiny string */

ULONG ami_cfg_strlen(const char *s)
{
    const char *p = s;

    while (*p != '\0')
        p++;

    return (ULONG)(p - s);
}

VOID ami_cfg_zero(APTR p, ULONG len)
{
    UBYTE *b = (UBYTE *)p;

    while (len-- > 0)
        *b++ = 0;
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

int ami_cfg_stricmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = to_lower(*a++);
        char cb = to_lower(*b++);

        if (ca != cb)
            return (int)(UBYTE)ca - (int)(UBYTE)cb;
    }

    return (int)(UBYTE)to_lower(*a) - (int)(UBYTE)to_lower(*b);
}

VOID ami_cfg_copy_string(char *dst, ULONG dstlen, const char *src)
{
    ULONG i;

    if (dst == NULL || dstlen == 0)
        return;

    for (i = 0; i + 1 < dstlen && src != NULL && src[i] != '\0'; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}

/* ------------------------------------------------------------ line reader */

char *ami_cfg_next_line(char **cursor)
{
    char *p = *cursor;
    char *start;

    if (p == NULL || *p == '\0')
        return NULL;

    start = p;

    while (*p != '\0' && *p != '\n' && *p != '\r')
        p++;

    if (*p != '\0')
    {
        char c = *p;

        *p++ = '\0';

        /* CRLF and (defensively) LFCR count as one line break. */
        if ((c == '\r' && *p == '\n') || (c == '\n' && *p == '\r'))
            p++;
    }

    *cursor = p;

    return start;
}

char *ami_cfg_trim(char *s)
{
    char *end;

    if (s == NULL)
        return NULL;

    while (*s == ' ' || *s == '\t' || *s == '\f' || *s == '\v')
        s++;

    end = s + ami_cfg_strlen(s);
    while (end > s)
    {
        char c = end[-1];

        if (c != ' ' && c != '\t' && c != '\f' && c != '\v' && c != '\r')
            break;
        end--;
    }
    *end = '\0';

    return s;
}

VOID ami_cfg_strip_comment(char *s, const char *chars)
{
    BOOL in_quotes = FALSE;

    if (s == NULL || chars == NULL)
        return;

    for (; *s != '\0'; s++)
    {
        const char *c;

        if (*s == '"')
        {
            in_quotes = !in_quotes;
            continue;
        }
        if (*s == '*' && in_quotes && s[1] != '\0')
        {
            /* AmigaDOS escape: skip the escaped character. */
            s++;
            continue;
        }
        if (in_quotes)
            continue;

        for (c = chars; *c != '\0'; c++)
        {
            if (*s == *c)
            {
                *s = '\0';
                return;
            }
        }
    }
}

VOID ami_cfg_unquote(char *s)
{
    char *out;
    char *p;

    if (s == NULL || *s != '"')
        return;

    p   = s + 1;
    out = s;

    while (*p != '\0' && *p != '"')
    {
        if (*p == '*' && p[1] != '\0')
        {
            p++;
            switch (*p)
            {
            case 'n': case 'N': *out++ = '\n';   break;
            case 'e': case 'E': *out++ = '\033'; break;
            default:            *out++ = *p;     break;
            }
            p++;
        }
        else
        {
            *out++ = *p++;
        }
    }

    *out = '\0';
}

/*
 * Consume one whitespace-delimited (or "quoted") item, NUL-terminating it in
 * place. Returns NULL when the cursor is exhausted.
 */
static char *scan_item(char **cursor)
{
    char *p = *cursor;
    char *start;
    char *out;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0')
    {
        *cursor = p;
        return NULL;
    }

    if (*p == '"')
    {
        p++;
        start = p;
        out   = p;

        while (*p != '\0' && *p != '"')
        {
            if (*p == '*' && p[1] != '\0')
            {
                p++;
                switch (*p)
                {
                case 'n': case 'N': *out++ = '\n';   break;
                case 'e': case 'E': *out++ = '\033'; break;
                default:            *out++ = *p;     break;
                }
                p++;
            }
            else
            {
                *out++ = *p++;
            }
        }

        if (*p == '"')
            p++;

        /* out never runs past p, so this only clobbers consumed input. */
        *out    = '\0';
        *cursor = p;

        return start;
    }

    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        p++;

    if (*p != '\0')
        *p++ = '\0';

    *cursor = p;

    return start;
}

ULONG ami_cfg_tokenize(char *line, char **tokens, ULONG max)
{
    ULONG n = 0;

    if (line == NULL || tokens == NULL)
        return 0;

    while (n < max)
    {
        char *item = scan_item(&line);

        if (item == NULL)
            break;

        tokens[n++] = item;
    }

    return n;
}

BOOL ami_cfg_next_pair(char **cursor, char **key, char **value)
{
    char *p;
    char *k;
    char *kend;

    if (cursor == NULL || *cursor == NULL)
        return FALSE;

    p = *cursor;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0')
    {
        *cursor = p;
        return FALSE;
    }

    k = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '=')
        p++;
    kend = p;

    /* "device = x", "device= x" and "device =x" are all legal. */
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '=')
    {
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }

    *kend = '\0';

    if (key != NULL)
        *key = k;

    {
        char *item = scan_item(&p);

        if (value != NULL)
            *value = (item != NULL) ? item : ami_cfg_empty;
    }

    *cursor = p;

    return TRUE;
}

/* --------------------------------------------------------------- numbers */

BOOL ami_cfg_parse_ulong(const char *s, ULONG *out)
{
    ULONG value = 0;
    ULONG digits = 0;
    ULONG base = 10;

    if (s == NULL)
        return FALSE;

    while (*s == ' ' || *s == '\t')
        s++;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        s += 2;
    }

    for (; *s != '\0'; s++)
    {
        ULONG digit;

        if (*s >= '0' && *s <= '9')
            digit = (ULONG)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            digit = (ULONG)(*s - 'a') + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            digit = (ULONG)(*s - 'A') + 10;
        else
            break;

        value = value * base + digit;
        digits++;
    }

    while (*s == ' ' || *s == '\t')
        s++;

    if (digits == 0 || *s != '\0')
        return FALSE;

    if (out != NULL)
        *out = value;

    return TRUE;
}

BOOL ami_cfg_parse_bool(const char *s, BOOL *out)
{
    static const char *const yes[] = { "yes", "true", "on",  "1", NULL };
    static const char *const no[]  = { "no",  "false", "off", "0", NULL };
    int i;

    if (s == NULL)
        return FALSE;

    for (i = 0; yes[i] != NULL; i++)
    {
        if (ami_cfg_stricmp(s, yes[i]) == 0)
        {
            if (out != NULL)
                *out = TRUE;
            return TRUE;
        }
    }
    for (i = 0; no[i] != NULL; i++)
    {
        if (ami_cfg_stricmp(s, no[i]) == 0)
        {
            if (out != NULL)
                *out = FALSE;
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * Split "a.b.c.d" into up to four octets. Returns the part count, or 0 when
 * the text is not a dotted decimal number at all.
 */
static ULONG split_dotted(const char *s, ULONG *parts)
{
    ULONG count = 0;

    if (s == NULL)
        return 0;

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s == '\0')
        return 0;

    for (;;)
    {
        ULONG value  = 0;
        ULONG digits = 0;

        while (*s >= '0' && *s <= '9')
        {
            value = value * 10 + (ULONG)(*s - '0');
            if (value > 255)
                return 0;
            digits++;
            s++;
        }

        if (digits == 0 || count >= 4)
            return 0;

        parts[count++] = value;

        if (*s != '.')
            break;
        s++;
    }

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s != '\0')
        return 0;

    return count;
}

BOOL ami_config_parse_ip(const char *text, ULONG *out)
{
    ULONG parts[4];

    if (split_dotted(text, parts) != 4)
        return FALSE;

    if (out != NULL)
        *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];

    return TRUE;
}

BOOL ami_cfg_parse_net_number(const char *s, ULONG *out)
{
    ULONG parts[4];
    ULONG count;
    ULONG value = 0;
    ULONG i;

    count = split_dotted(s, parts);
    if (count == 0)
        return FALSE;

    /*
     * BSD inet_network() semantics, which is what /etc/networks and
     * getnetbyname() use: the octets are packed right-aligned, so "127" is
     * 127 and "192.168.1" is 0x00C0A801.
     */
    for (i = 0; i < count; i++)
        value = (value << 8) | parts[i];

    if (out != NULL)
        *out = value;

    return TRUE;
}

VOID ami_config_format_ip(ULONG addr, char *buf, ULONG buflen)
{
    char  tmp[16];
    ULONG pos = 0;
    int   octet;

    if (buf == NULL || buflen == 0)
        return;

    for (octet = 3; octet >= 0; octet--)
    {
        ULONG v = (addr >> (octet * 8)) & 0xFFUL;
        char  digits[3];
        int   n = 0;

        do
        {
            digits[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (v != 0);

        while (n > 0)
            tmp[pos++] = digits[--n];

        if (octet > 0)
            tmp[pos++] = '.';
    }

    tmp[pos] = '\0';

    ami_cfg_copy_string(buf, buflen, tmp);
}

/* ------------------------------------------------------------------ IPv6 -- */

/*
 * Compiled only in an AMINETXDUO_IPV6 build.
 *
 * The guard is not cosmetic: config_text.c is one object, so the linker pulls
 * the whole of it in for any caller of ami_cfg_trim(), and leaving these two
 * functions unguarded put 3.4 KB of IPv6 text conversion into a floor build
 * that has no IPv6 to convert. Measured, not assumed -- netstack_test grew
 * from 162,936 to 166,316 bytes of .text with them in.
 */
#ifdef AMINETXDUO_IPV6


/*
 * RFC 4291 §2.2 text form.  Written from the grammar rather than adapted from
 * a BSD inet_pton(), because the two callers want slightly different dialects
 * (see the contract in include/aminetxduo/config.h) and because this file is
 * deliberately free of libc.
 *
 * The awkward parts of the format, and how they are handled:
 *
 *   - "::" stands for one or more groups of zeroes and may appear at most
 *     once.  Groups are collected into a flat array as they are read; the
 *     position of "::" is remembered and the array is shifted to the right at
 *     the end.  That is simpler and smaller than trying to place them as they
 *     arrive, and it makes "::" at either end fall out for free.
 *   - a trailing dotted quad ("::ffff:192.168.1.1") occupies the last two
 *     groups.  It is only legal in the last position, and only when four
 *     octets are present.
 *   - a group is one to four hex digits.  Five is an error, not a truncation.
 */

static BOOL ip6_hex_digit(char c, ULONG *value)
{
    if (c >= '0' && c <= '9')
        *value = (ULONG)(c - '0');
    else if (c >= 'a' && c <= 'f')
        *value = (ULONG)(c - 'a') + 10UL;
    else if (c >= 'A' && c <= 'F')
        *value = (ULONG)(c - 'A') + 10UL;
    else
        return FALSE;

    return TRUE;
}

BOOL ami_config_parse_ip6(const char *text, ULONG out[AMI_CFG_IP6_WORDS],
                          ULONG *prefix_out)
{
    UWORD       group[8];
    const char *s = text;
    ULONG       count     = 0;
    LONG        gap       = -1;     /* index the "::" run starts at        */
    ULONG       prefix    = 0;
    BOOL        have_pfx  = FALSE;
    ULONG       i;

    if (s == NULL)
        return FALSE;

    while (*s == ' ' || *s == '\t')
        s++;

    /* A leading "::" is the only way a leading ':' is legal. */
    if (s[0] == ':')
    {
        if (s[1] != ':')
            return FALSE;
        gap = 0;
        s += 2;
    }

    while (*s != '\0' && *s != '/' && *s != ' ' && *s != '\t')
    {
        ULONG value  = 0;
        ULONG digits = 0;
        ULONG digit;

        /* A dotted quad here consumes the last two groups and ends the
           address.  Detected by scanning ahead for a '.' before the next
           ':' -- the digits themselves are ambiguous until then. */
        {
            const char *look = s;

            while (*look != '\0' && *look != ':' && *look != '/' &&
                   *look != ' ' && *look != '\t' && *look != '.')
                look++;

            if (*look == '.')
            {
                ULONG parts[4];
                char  quad[16];
                ULONG n = 0;

                while (s[n] != '\0' && s[n] != '/' && s[n] != ' ' &&
                       s[n] != '\t')
                {
                    if (n >= sizeof(quad) - 1)
                        return FALSE;
                    quad[n] = s[n];
                    n++;
                }
                quad[n] = '\0';

                if (split_dotted(quad, parts) != 4)
                    return FALSE;

                if (count + 2 > 8)
                    return FALSE;

                group[count++] = (UWORD)((parts[0] << 8) | parts[1]);
                group[count++] = (UWORD)((parts[2] << 8) | parts[3]);

                s += n;
                break;
            }
        }

        while (ip6_hex_digit(*s, &digit))
        {
            if (digits >= 4)
                return FALSE;
            value = (value << 4) | digit;
            digits++;
            s++;
        }

        if (digits == 0 || count >= 8)
            return FALSE;

        group[count++] = (UWORD)value;

        if (*s != ':')
            break;

        s++;

        if (*s == ':')
        {
            if (gap >= 0)
                return FALSE;       /* a second "::" */
            gap = (LONG)count;
            s++;

            /* "1::" ends here; anything else continues with a group. */
            if (*s == '\0' || *s == '/' || *s == ' ' || *s == '\t')
                break;
        }
        else if (*s == '\0' || *s == '/' || *s == ' ' || *s == '\t')
        {
            return FALSE;           /* a trailing single ':' */
        }
    }

    if (*s == '/')
    {
        ULONG digits = 0;

        if (prefix_out == NULL)
            return FALSE;           /* inet_pton() dialect: no prefix allowed */

        s++;
        while (*s >= '0' && *s <= '9')
        {
            prefix = prefix * 10UL + (ULONG)(*s - '0');
            if (prefix > 128UL)
                return FALSE;
            digits++;
            s++;
        }

        if (digits == 0)
            return FALSE;

        have_pfx = TRUE;
    }

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s != '\0')
        return FALSE;

    if (gap < 0)
    {
        if (count != 8)
            return FALSE;
    }
    else
    {
        /*
         * "::" must stand for at least one group -- "1:2:3:4:5:6:7::8" has
         * eight groups already and is malformed, not merely redundant.
         */
        if (count >= 8)
            return FALSE;

        for (i = count; i > (ULONG)gap; i--)
            group[i - 1 + (8 - count)] = group[i - 1];
        for (i = (ULONG)gap; i < (ULONG)gap + (8 - count); i++)
            group[i] = 0;
    }

    if (out != NULL)
    {
        for (i = 0; i < AMI_CFG_IP6_WORDS; i++)
            out[i] = ((ULONG)group[i * 2] << 16) | (ULONG)group[i * 2 + 1];
    }

    if (prefix_out != NULL && have_pfx)
        *prefix_out = prefix;

    return TRUE;
}

VOID ami_config_format_ip6(const ULONG addr[AMI_CFG_IP6_WORDS],
                           char *buf, ULONG buflen)
{
    char  tmp[AMI_CFG_IP6_STRLEN];
    UWORD group[8];
    ULONG pos       = 0;
    LONG  best_at   = -1;
    ULONG best_len  = 0;
    LONG  run_at    = -1;
    ULONG run_len   = 0;
    ULONG i;

    if (buf == NULL || buflen == 0)
        return;

    buf[0] = '\0';

    if (addr == NULL || buflen < AMI_CFG_IP6_STRLEN)
        return;

    for (i = 0; i < AMI_CFG_IP6_WORDS; i++)
    {
        group[i * 2]     = (UWORD)(addr[i] >> 16);
        group[i * 2 + 1] = (UWORD)(addr[i] & 0xFFFFUL);
    }

    /* Longest run of two or more zero groups; leftmost wins a tie
       (RFC 5952 4.2.3). */
    for (i = 0; i < 8; i++)
    {
        if (group[i] == 0)
        {
            if (run_at < 0)
            {
                run_at  = (LONG)i;
                run_len = 0;
            }
            run_len++;

            if (run_len > best_len)
            {
                best_len = run_len;
                best_at  = run_at;
            }
        }
        else
        {
            run_at  = -1;
            run_len = 0;
        }
    }

    if (best_len < 2)
        best_at = -1;

    /*
     * The colon bookkeeping is the classic BSD/glibc inet_ntop6 shape, which
     * is worth copying rather than re-deriving: ONE colon is emitted where the
     * elided run starts, the separator in front of the group that follows the
     * run supplies the second, and a run that reaches the end of the address
     * gets its second colon added afterwards. That handles "::", "::1", "1::"
     * and "fe80::1" with no special cases.
     */
    for (i = 0; i < 8; i++)
    {
        if (best_at >= 0 && (LONG)i >= best_at &&
            (LONG)i < best_at + (LONG)best_len)
        {
            if ((LONG)i == best_at)
                tmp[pos++] = ':';
            continue;
        }

        if (i != 0)
            tmp[pos++] = ':';

        /*
         * v4-mapped (::ffff:a.b.c.d) and the deprecated v4-compatible form are
         * written with a dotted tail (RFC 5952 5). The test is the one every
         * other stack uses, so the output matches theirs byte for byte.
         */
        if (i == 6 && best_at == 0 &&
            (best_len == 6 || (best_len == 5 && group[5] == 0xFFFF)))
        {
            char  quad[16];
            ULONG addr4 = ((ULONG)group[6] << 16) | (ULONG)group[7];
            ULONG n;

            ami_config_format_ip(addr4, quad, sizeof(quad));
            for (n = 0; quad[n] != '\0'; n++)
                tmp[pos++] = quad[n];
            break;
        }

        {
            UWORD v = group[i];
            char  digits[4];
            int   n = 0;

            do
            {
                ULONG nibble = (ULONG)(v & 0xF);

                digits[n++] = (char)((nibble < 10) ? ('0' + (int)nibble)
                                                   : ('a' + (int)nibble - 10));
                v = (UWORD)(v >> 4);
            }
            while (v != 0);

            while (n > 0)
                tmp[pos++] = digits[--n];
        }
    }

    /* A run that ran to the end of the address still owes its second colon. */
    if (best_at >= 0 && (ULONG)best_at + best_len == 8)
        tmp[pos++] = ':';

    tmp[pos] = '\0';

    ami_cfg_copy_string(buf, buflen, tmp);
}

#endif /* AMINETXDUO_IPV6 */
