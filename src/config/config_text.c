/*
 * AmiNetXDuo, text handling for the configuration parsers.
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

VOID ami_cfg_join3(char *dst, ULONG dstlen, const char *a, const char *b,
                   const char *c)
{
    ULONG pos = 0;
    int   i;

    if (dst == NULL || dstlen == 0)
        return;

    dst[0] = '\0';

    for (i = 0; i < 3; i++)
    {
        const char *src = (i == 0) ? a : (i == 1) ? b : c;

        if (src == NULL)
            continue;

        while (*src != '\0' && pos + 1 < dstlen)
            dst[pos++] = *src++;
    }

    dst[pos] = '\0';
}

/* ------------------------------------------------------------ diagnostics */

/*
 * One reporter, one current file. The configuration is read once, from one
 * task, at startup, so nothing here needs to be thread-safe; a per-parser
 * context argument would have to be threaded through code the host test drives
 * directly.
 */
static AmiCfgReporter   ami_cfg_reporter;
static APTR             ami_cfg_reporter_user;
static const char      *ami_cfg_current_file = "the configuration";

VOID ami_config_set_reporter(AmiCfgReporter reporter, APTR user)
{
    ami_cfg_reporter      = reporter;
    ami_cfg_reporter_user = user;
}

VOID ami_cfg_problem_file(const char *path)
{
    ami_cfg_current_file = (path != NULL) ? path : "the configuration";
}

BOOL ami_cfg_problems_wanted(VOID)
{
    return (ami_cfg_reporter != NULL) ? TRUE : FALSE;
}

VOID ami_cfg_problem(ULONG line, UWORD severity, const char *text,
                     const char *hint)
{
    AmiCfgProblem problem;

    if (ami_cfg_reporter == NULL || text == NULL)
        return;

    problem.file     = ami_cfg_current_file;
    problem.line     = line;
    problem.severity = severity;
    problem.text     = text;
    problem.hint     = hint;

    ami_cfg_reporter(&problem, ami_cfg_reporter_user);
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

/*
 * Six hexadecimal bytes, for HARDWAREADDRESS.
 *
 * Colons, hyphens or nothing between them, because all three are written in
 * the wild and refusing two of them would be refusing a correct address on a
 * technicality. Exactly six: a short one is a typo, and a long one is not an
 * Ethernet address.
 */
BOOL ami_cfg_parse_mac(const char *s, UBYTE *out)
{
    ULONG i;

    if (s == NULL || out == NULL)
        return FALSE;

    for (i = 0; i < AMI_CFG_MAC_SIZE; i++)
    {
        ULONG byte = 0;
        int   digits;

        if (i > 0 && (*s == ':' || *s == '-'))
            s++;

        for (digits = 0; digits < 2; digits++)
        {
            char c = *s;
            ULONG nibble;

            if (c >= '0' && c <= '9')       nibble = (ULONG)(c - '0');
            else if (c >= 'a' && c <= 'f')  nibble = (ULONG)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')  nibble = (ULONG)(c - 'A' + 10);
            else                            return FALSE;

            byte = (byte << 4) | nibble;
            s++;
        }

        out[i] = (UBYTE)byte;
    }

    return (*s == '\0') ? TRUE : FALSE;
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

