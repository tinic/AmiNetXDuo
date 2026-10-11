/*
 * NetWireless: Wireless.prefs and interface-file text.  See the header.
 *
 * A line is read as wpa_supplicant 0.7.3's wpa_config_get_line() reads it:
 * leading blanks skipped, '#' lines and blank lines ignored, a '#' after the
 * last '"' ends the line, trailing blanks dropped.  "network={" and "}" are
 * then whole-line matches, and a key is everything before the first '='.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_prefs.h"

typedef struct NwLine
{
    size_t start;                   /* first byte of the line              */
    size_t next;                    /* first byte of the line after it     */
    size_t text;                    /* first byte after leading blanks     */
    size_t text_end;                /* end of the content, comment removed */
} NwLine;

static size_t nw_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* The line at pos; returns 0 at the end of the text. */
static int nw_line(const char *t, size_t len, size_t pos, NwLine *l)
{
    size_t e, q, last_quote, i;

    if (pos >= len) return 0;
    l->start = pos;
    e = pos;
    while (e < len && t[e] != '\n') e++;
    l->next = (e < len) ? e + 1 : e;

    i = pos;
    while (i < e && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r')) i++;
    l->text = i;
    if (i >= e || t[i] == '#')
    {
        l->text_end = i;
        return 1;
    }

    /* A '#' inside the quoted part is text. */
    last_quote = i;
    for (q = i; q < e && t[q] != '"'; q++)
        ;
    if (q < e)
    {
        size_t r;
        for (r = q + 1; r < e; r++)
            if (t[r] == '"') last_quote = r;
    }
    for (q = last_quote; q < e && t[q] != '#'; q++)
        ;
    while (q > i && (t[q - 1] == ' ' || t[q - 1] == '\t' || t[q - 1] == '\r'))
        q--;
    l->text_end = q;
    return 1;
}

static int nw_is(const char *t, const NwLine *l, const char *word)
{
    size_t n = nw_strlen(word), i;

    if (l->text_end - l->text != n) return 0;
    for (i = 0; i < n; i++)
        if (t[l->text + i] != word[i]) return 0;
    return 1;
}

/* The key of a "key=value" line: its length, or 0. */
static size_t nw_key_len(const char *t, const NwLine *l)
{
    size_t i;

    for (i = l->text; i < l->text_end; i++)
        if (t[i] == '=') return i - l->text;
    return 0;
}

static int nw_key_is(const char *t, const NwLine *l, const char *key)
{
    size_t n = nw_key_len(t, l), i;

    if (n == 0 || n != nw_strlen(key)) return 0;
    for (i = 0; i < n; i++)
        if (t[l->text + i] != key[i]) return 0;
    return 1;
}

static int nw_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static long nw_number(const char *t, size_t i, size_t end)
{
    long v = 0;
    int  neg = 0;

    while (i < end && (t[i] == ' ' || t[i] == '\t')) i++;
    if (i < end && (t[i] == '-' || t[i] == '+')) { neg = t[i] == '-'; i++; }
    while (i < end && t[i] >= '0' && t[i] <= '9')
        v = v * 10 + (t[i++] - '0');
    return neg ? -v : v;
}

static void nw_parse_ssid(const char *t, size_t v, size_t end, NwNetwork *n)
{
    size_t i;
    int    len = 0;

    n->ssid_len = -1;
    if (v < end && t[v] == '"')
    {
        /* wpa_config_parse_string(): the closing quote is the last byte. */
        if (end - v < 2 || t[end - 1] != '"') return;
        for (i = v + 1; i + 1 < end; i++)
        {
            if (len >= NW_SSID_MAX) return;
            n->ssid[len++] = (unsigned char)t[i];
        }
    }
    else
    {
        if ((end - v) % 2 != 0) return;
        for (i = v; i < end; i += 2)
        {
            int hi = nw_hex_digit(t[i]), lo = nw_hex_digit(t[i + 1]);
            if (hi < 0 || lo < 0 || len >= NW_SSID_MAX) return;
            n->ssid[len++] = (unsigned char)(hi * 16 + lo);
        }
    }
    n->ssid_len = len;
}

int nw_prefs_parse(const char *text, size_t len, NwNetwork *out, int max)
{
    NwLine l;
    size_t pos = 0;
    int    count = 0;

    while (nw_line(text, len, pos, &l))
    {
        NwNetwork net;
        int       closed = 0;

        pos = l.next;
        if (!nw_is(text, &l, "network={")) continue;

        net.start = l.start;
        net.ssid_len = -1;
        net.priority = 0;
        net.scan_ssid = 0;
        net.open = 0;
        net.has_psk = 0;
        net.disabled = 0;
        net.wep = 0;

        while (nw_line(text, len, pos, &l))
        {
            size_t v;

            pos = l.next;
            if (nw_is(text, &l, "}")) { closed = 1; break; }
            if (nw_key_len(text, &l) == 0) continue;
            v = l.text + nw_key_len(text, &l) + 1;
            if (nw_key_is(text, &l, "ssid"))
                nw_parse_ssid(text, v, l.text_end, &net);
            else if (nw_key_is(text, &l, "priority"))
                net.priority = nw_number(text, v, l.text_end);
            else if (nw_key_is(text, &l, "scan_ssid"))
                net.scan_ssid = nw_number(text, v, l.text_end) != 0;
            else if (nw_key_is(text, &l, "disabled"))
                net.disabled = nw_number(text, v, l.text_end) != 0;
            else if (nw_key_is(text, &l, "psk"))
                net.has_psk = 1;
            else if (nw_key_is(text, &l, "wep_key0") ||
                     nw_key_is(text, &l, "wep_key1") ||
                     nw_key_is(text, &l, "wep_key2") ||
                     nw_key_is(text, &l, "wep_key3"))
                net.wep = 1;
            else if (nw_key_is(text, &l, "key_mgmt"))
                net.open = (l.text_end - v == 4 && text[v] == 'N' &&
                            text[v + 1] == 'O' && text[v + 2] == 'N' &&
                            text[v + 3] == 'E');
        }
        if (!closed) return -1;
        net.end = pos;
        if (net.wep) net.open = 0;
        if (count < max) out[count] = net;
        count++;
    }
    return count;
}

typedef struct NwOut
{
    char  *buf;
    size_t cap;
    size_t used;
    int    ok;
} NwOut;

static void nw_put(NwOut *o, const char *data, size_t n)
{
    size_t i;

    if (!o->ok || o->used + n > o->cap) { o->ok = 0; return; }
    for (i = 0; i < n; i++) o->buf[o->used++] = data[i];
}

static void nw_puts(NwOut *o, const char *s)
{
    nw_put(o, s, nw_strlen(s));
}

static void nw_put_field(NwOut *o, const char *indent, size_t indent_len,
                         const NwField *f, const char *eol, size_t eol_len)
{
    nw_put(o, indent, indent_len);
    nw_puts(o, f->key);
    nw_put(o, "=", 1);
    nw_puts(o, f->value);
    nw_put(o, eol, eol_len);
}

static void nw_patch_block(const char *t, size_t len, const NwNetwork *n,
                           NwBlockEdit *e, NwOut *o)
{
    NwLine      l;
    size_t      pos;
    const char *indent = "\t";
    size_t      indent_len = 1;
    int         have_indent = 0, k;

    for (k = 0; k < e->nfields; k++) e->fields[k].seen = 0;

    /* The "network={" line as it was. */
    (void)nw_line(t, len, n->start, &l);
    nw_put(o, t + l.start, l.next - l.start);
    pos = l.next;

    while (pos < n->end && nw_line(t, len, pos, &l))
    {
        pos = l.next;
        if (nw_is(t, &l, "}"))
        {
            /* Added lines end the way the "}" line does, LF at the end of
               the text. */
            const char *eol = "\n";
            size_t      eol_len = 1;

            if (l.next - l.start >= 2 && t[l.next - 2] == '\r' &&
                t[l.next - 1] == '\n')
            {
                eol = "\r\n";
                eol_len = 2;
            }
            for (k = 0; k < e->nfields; k++)
                if (!e->fields[k].seen && e->fields[k].value != NULL)
                    nw_put_field(o, indent, indent_len, &e->fields[k],
                                 eol, eol_len);
            nw_put(o, t + l.start, l.next - l.start);
            continue;
        }
        if (nw_key_len(t, &l) != 0 && !have_indent)
        {
            indent = t + l.start;
            indent_len = l.text - l.start;
            have_indent = 1;
        }
        for (k = 0; k < e->nfields; k++)
            if (nw_key_is(t, &l, e->fields[k].key)) break;
        if (k == e->nfields)
        {
            nw_put(o, t + l.start, l.next - l.start);
            continue;
        }
        if (!e->fields[k].seen && e->fields[k].value != NULL)
        {
            /* The line's own indentation and line ending, the new value. */
            size_t eol = l.next;
            while (eol > l.text_end && (t[eol - 1] == '\n' || t[eol - 1] == '\r'))
                eol--;
            nw_put_field(o, t + l.start, l.text - l.start, &e->fields[k],
                         t + eol, l.next - eol);
        }
        e->fields[k].seen = 1;
    }
}

int nw_prefs_write(const char *old, size_t oldlen,
                   const NwNetwork *nets, int nnets,
                   NwBlockEdit *edits, int nedits,
                   char *out, size_t cap, size_t *newlen)
{
    NwOut  o;
    size_t pos = 0;
    int    i, k;

    o.buf = out; o.cap = cap; o.used = 0; o.ok = 1;

    for (k = 0; k < nedits; k++)
        if (edits[k].index >= nnets) return 0;

    for (i = 0; i < nnets; i++)
    {
        NwBlockEdit *e = NULL;

        for (k = 0; k < nedits; k++)
            if (edits[k].index == i) { e = &edits[k]; break; }
        nw_put(&o, old + pos, nets[i].start - pos);
        pos = nets[i].end;
        if (e == NULL)
            nw_put(&o, old + nets[i].start, nets[i].end - nets[i].start);
        else if (!e->remove)
            nw_patch_block(old, oldlen, &nets[i], e, &o);
    }
    nw_put(&o, old + pos, oldlen - pos);

    for (k = 0; k < nedits; k++)
    {
        int f;

        if (edits[k].index >= 0 || edits[k].remove) continue;
        if (o.used != 0 && o.ok && out[o.used - 1] != '\n') nw_put(&o, "\n", 1);
        nw_puts(&o, "network={\n");
        for (f = 0; f < edits[k].nfields; f++)
            if (edits[k].fields[f].value != NULL)
                nw_put_field(&o, "\t", 1, &edits[k].fields[f], "\n", 1);
        nw_puts(&o, "}\n");
    }

    if (!o.ok) return 0;
    *newlen = o.used;
    return 1;
}

void nw_prefs_ssid_value(const unsigned char *ssid, int len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    int i, printable = len > 0;

    for (i = 0; i < len; i++)
        if (ssid[i] < 0x20 || ssid[i] > 0x7e) printable = 0;
    if (printable)
    {
        *out++ = '"';
        for (i = 0; i < len; i++) *out++ = (char)ssid[i];
        *out++ = '"';
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            *out++ = hex[ssid[i] >> 4];
            *out++ = hex[ssid[i] & 15];
        }
    }
    *out = '\0';
}

/* word, n bytes, is keyword, case aside. */
static int nw_keyword(const char *word, size_t n, const char *keyword)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        char c = word[i];

        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != keyword[i]) return 0;
    }
    return keyword[n] == '\0';
}

int nw_iface_device(const char *text, size_t len, char *device, size_t cap,
                    unsigned long *unit)
{
    size_t pos = 0;
    int    found = 0;

    while (pos < len)
    {
        size_t e = pos, i, k, v, ve;
        int    is_device, is_unit;

        while (e < len && text[e] != '\n' && text[e] != '\r') e++;
        i = pos;
        pos = (e < len) ? e + 1 : e;
        while (i < e && (text[i] == ' ' || text[i] == '\t')) i++;
        if (i >= e || text[i] == ';' || text[i] == '#') continue;

        k = i;
        while (k < e && text[k] != '=' && text[k] != ' ' && text[k] != '\t') k++;
        is_device = nw_keyword(text + i, k - i, "DEVICE");
        is_unit = nw_keyword(text + i, k - i, "UNIT");
        if (!is_device && !is_unit) continue;

        v = k;
        while (v < e && (text[v] == ' ' || text[v] == '\t')) v++;
        if (v < e && text[v] == '=') v++;
        while (v < e && (text[v] == ' ' || text[v] == '\t')) v++;
        if (v < e && text[v] == '"')
        {
            v++;
            ve = v;
            while (ve < e && text[ve] != '"') ve++;
        }
        else
        {
            ve = v;
            while (ve < e && text[ve] != ' ' && text[ve] != '\t' &&
                   text[ve] != ';' && text[ve] != '#') ve++;
        }
        if (ve == v) continue;

        if (is_device && ve - v < cap)
        {
            for (k = 0; k < ve - v; k++) device[k] = text[v + k];
            device[ve - v] = '\0';
            found = 1;
        }
        else if (is_unit && text[v] >= '0' && text[v] <= '9')
            *unit = (unsigned long)nw_number(text, v, ve);
    }
    return found;
}
