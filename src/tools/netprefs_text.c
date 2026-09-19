/* SPDX-License-Identifier: MIT */
#include "netprefs_text.h"

static size_t np_len(const char *s)
{
    size_t n = 0;
    while (s != NULL && s[n] != '\0') n++;
    return n;
}

static int equal_nocase(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen && b[i] != '\0'; i++)
    {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return i == alen && b[i] == '\0';
}

int np_interface_name_safe(const char *name, size_t limit)
{
    size_t n = 0;

    if (name == NULL || limit < 2) return 0;
    while (name[n] != '\0')
    {
        char c = name[n++];
        if (n == 1 && !((c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9')))
            return 0;
        if (n >= limit ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return n != 0;
}

/*
 * The keywords the parser reads as the GUI's keys (src/config/config_parse.c):
 * a file that says IPADDRESS= is edited in place as ADDRESS, not left with a
 * stale IPADDRESS beside a new ADDRESS.  Each row is the GUI key followed by
 * its aliases, NULL-terminated.
 */
static const char *const np_key_aliases[][5] =
{
    { "ADDRESS",   "IPADDRESS",   NULL },
    { "NETMASK",   "SUBNETMASK",  NULL },
    { "PRIORITY",  "PRI",         NULL },
    { "ADDRESS6",  "IPADDRESS6",  NULL },
    { "CONFIGURE6","IPTYPE6",     NULL },
    { NULL }
};

static int key_matches(const char *seen, size_t seen_len, const char *key)
{
    size_t r, a;

    if (equal_nocase(seen, seen_len, key)) return 1;
    for (r = 0; np_key_aliases[r][0] != NULL; r++)
    {
        if (!equal_nocase(key, np_len(key), np_key_aliases[r][0])) continue;
        for (a = 1; np_key_aliases[r][a] != NULL; a++)
            if (equal_nocase(seen, seen_len, np_key_aliases[r][a])) return 1;
    }
    return 0;
}

/* Numeric IPTYPE is the SANA-II packet type, not an address-mode alias.
   Be conservative: anything beginning like a number remains byte-for-byte,
   even if a later parser would reject the rest of the value. */
static int iptype_is_address_mode(const char *line, size_t len, size_t value)
{
    while (value < len && (line[value] == ' ' || line[value] == '\t')) value++;
    if (value < len && line[value] == '"')
    {
        value++;
        while (value < len && (line[value] == ' ' || line[value] == '\t')) value++;
    }
    return value >= len || line[value] < '0' || line[value] > '9';
}

static long field_of_line(const char *line, size_t len,
                          NpTextField *fields, size_t count)
{
    size_t begin = 0, end, value, i;
    int alphabetic_iptype;
    long first = -1;
    while (begin < len && (line[begin] == ' ' || line[begin] == '\t')) begin++;
    if (begin == len || line[begin] == '#' || line[begin] == ';') return -1;
    end = begin;
    while (end < len && line[end] != '=' && line[end] != ' ' &&
           line[end] != '\t' && line[end] != '\r' && line[end] != '\n') end++;
    while (end < len && (line[end] == ' ' || line[end] == '\t')) end++;
    if (end >= len || line[end] != '=') return -1;
    value = end + 1;
    end = begin;
    while (end < len && line[end] != '=' && line[end] != ' ' &&
           line[end] != '\t' && line[end] != '\r' && line[end] != '\n') end++;
    alphabetic_iptype = equal_nocase(line + begin, end - begin, "IPTYPE") &&
                        iptype_is_address_mode(line, len, value);
    for (i = 0; i < count; i++)
    {
        if (!key_matches(line + begin, end - begin, fields[i].key) &&
            !(alphabetic_iptype &&
              equal_nocase(fields[i].key, np_len(fields[i].key), "CONFIGURE")))
            continue;
        if (first < 0) first = (long)i;
        if (!fields[i].seen) return (long)i;
    }
    /* More old occurrences than fields are still duplicates and are dropped. */
    return first;
}

static int append(char *out, size_t cap, size_t *used,
                  const char *data, size_t length)
{
    size_t i;
    if (*used + length > cap) return 0;
    for (i = 0; i < length; i++) out[(*used)++] = data[i];
    return 1;
}

static int append_field(char *out, size_t cap, size_t *used,
                        const NpTextField *f)
{
    size_t n;
    if (f->value == NULL) return 1;
    n = np_len(f->key);
    if (!append(out, cap, used, f->key, n) ||
        !append(out, cap, used, " = ", 3)) return 0;
    n = np_len(f->value);
    return append(out, cap, used, f->value, n) &&
           append(out, cap, used, "\n", 1);
}

int np_text_patch(const char *old, size_t oldlen, NpTextField *fields,
                  size_t count, char *out, size_t cap, size_t *newlen)
{
    size_t at = 0, used = 0, i;
    while (at < oldlen)
    {
        size_t start = at, content;
        long field;
        while (at < oldlen && old[at] != '\n') at++;
        if (at < oldlen) at++;
        content = at - start;
        field = field_of_line(old + start, content, fields, count);
        if (field < 0)
        {
            if (!append(out, cap, &used, old + start, content)) return 0;
        }
        else if (!fields[field].seen)
        {
            fields[field].seen = 1;
            if (!append_field(out, cap, &used, &fields[field])) return 0;
        }
        /* Drop duplicate keys owned by the GUI. */
    }
    if (used != 0 && out[used - 1] != '\n')
        if (!append(out, cap, &used, "\n", 1)) return 0;
    for (i = 0; i < count; i++)
        if (!fields[i].seen && !append_field(out, cap, &used, &fields[i])) return 0;
    *newlen = used;
    return 1;
}

/* One Shell word: p at its first character, returns one past its last. */
static size_t word_end(const char *line, size_t len, size_t p)
{
    while (p < len && line[p] != ' ' && line[p] != '\t' &&
           line[p] != '\r' && line[p] != '\n') p++;
    return p;
}

static size_t skip_blanks(const char *line, size_t len, size_t p)
{
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    return p;
}

/* A Shell redirection, ">file", ">>file", "<file", "*>file", with the file
   either attached or the next word.  Returns one past it, or p unchanged. */
static size_t skip_redirection(const char *line, size_t len, size_t p)
{
    size_t q = p;

    if (q < len && line[q] == '*') q++;
    if (q >= len || (line[q] != '>' && line[q] != '<')) return p;
    while (q < len && (line[q] == '>' || line[q] == '<')) q++;
    if (q < len && line[q] != ' ' && line[q] != '\t' &&
        line[q] != '\r' && line[q] != '\n')
        return word_end(line, len, q);
    q = skip_blanks(line, len, q);
    return word_end(line, len, q);
}

int np_startup_line(const char *line, size_t len, const char *name,
                    int *commented, int *wildcard)
{
    size_t p = 0, a, b;
    const char *base;
    *commented = 0;
    *wildcard = 0;
    p = skip_blanks(line, len, 0);
    if (p < len && line[p] == ';')
    {
        *commented = 1;
        p = skip_blanks(line, len, p + 1);
    }
    if (p == len || line[p] == '#') return 0;
    /* The command word, past a leading "Run" and any redirections on either
       side of it: "Run >NIL: C:AddNetInterface ..." and
       "C:AddNetInterface >>SYS:boot.log DEVS:NetInterfaces/genet" are both
       lines a machine here boots with. */
    for (;;)
    {
        size_t q;

        p = skip_blanks(line, len, p);
        q = skip_redirection(line, len, p);
        if (q != p) { p = q; continue; }
        a = p;
        b = word_end(line, len, p);
        if (b == a) return 0;
        if (equal_nocase(line + a, b - a, "Run")) { p = b; continue; }
        break;
    }
    base = line + a;
    while (a < b)
    {
        if (line[a] == ':' || line[a] == '/') base = line + a + 1;
        a++;
    }
    if (!equal_nocase(base, (size_t)((line + b) - base), "AddNetInterface"))
        return 0;
    p = b;
    for (;;)
    {
        size_t q;

        p = skip_blanks(line, len, p);
        q = skip_redirection(line, len, p);
        if (q != p) { p = q; continue; }
        break;
    }
    a = p;
    b = word_end(line, len, p);
    if (b == a) return 0;
    base = line + a;
    while (a < b)
    {
        if (line[a] == ':' || line[a] == '/') base = line + a + 1;
        a++;
    }
    if (base < line + b && (*base == '~' || *base == '#'))
    {
        *wildcard = 1;
        return 1;
    }
    return equal_nocase(base, (size_t)((line + b) - base), name);
}
