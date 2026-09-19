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

static long field_of_line(const char *line, size_t len,
                          NpTextField *fields, size_t count)
{
    size_t begin = 0, end, i;
    long first = -1;
    while (begin < len && (line[begin] == ' ' || line[begin] == '\t')) begin++;
    if (begin == len || line[begin] == '#' || line[begin] == ';') return -1;
    end = begin;
    while (end < len && line[end] != '=' && line[end] != ' ' &&
           line[end] != '\t' && line[end] != '\r' && line[end] != '\n') end++;
    while (end < len && (line[end] == ' ' || line[end] == '\t')) end++;
    if (end >= len || line[end] != '=') return -1;
    end = begin;
    while (end < len && line[end] != '=' && line[end] != ' ' &&
           line[end] != '\t' && line[end] != '\r' && line[end] != '\n') end++;
    for (i = 0; i < count; i++)
    {
        if (!equal_nocase(line + begin, end - begin, fields[i].key))
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

int np_startup_line(const char *line, size_t len, const char *name,
                    int *commented, int *wildcard)
{
    size_t p = 0, a, b;
    const char *base;
    *commented = 0;
    *wildcard = 0;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p < len && line[p] == ';')
    {
        *commented = 1;
        p++;
        while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    }
    if (p == len || line[p] == '#') return 0;
    a = p;
    while (p < len && line[p] != ' ' && line[p] != '\t' &&
           line[p] != '\r' && line[p] != '\n') p++;
    b = p;
    base = line + a;
    while (a < b)
    {
        if (line[a] == ':' || line[a] == '/') base = line + a + 1;
        a++;
    }
    if (!equal_nocase(base, (size_t)((line + b) - base), "AddNetInterface"))
        return 0;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    a = p;
    while (p < len && line[p] != ' ' && line[p] != '\t' &&
           line[p] != '\r' && line[p] != '\n') p++;
    b = p;
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
