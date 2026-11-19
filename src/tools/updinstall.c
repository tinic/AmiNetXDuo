/*
 * updinstall: see updinstall.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updinstall.h"

static int ui_fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int ui_same(const char *a, unsigned long an, const char *b,
                   unsigned long bn)
{
    unsigned long i;

    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (ui_fold((unsigned char)a[i]) != ui_fold((unsigned char)b[i]))
            return 0;
    return 1;
}

/* `n` bytes of `a` start with the NUL-terminated `p`, case aside. */
static unsigned long ui_prefix(const char *a, unsigned long n, const char *p)
{
    unsigned long i;

    for (i = 0; p[i] != '\0'; i++)
        if (i >= n || ui_fold((unsigned char)a[i]) != ui_fold((unsigned char)p[i]))
            return 0;
    return i;
}

static int ui_eq32(const unsigned char *a, const unsigned char *b)
{
    unsigned int i;

    for (i = 0; i < 32; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int ui_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* A path byte: printable ASCII, no space at either end is checked by the
   callers through the field split. */
static int ui_pathbyte(int c)
{
    return c > ' ' && c < 0x7f;
}

static unsigned long ui_len(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* Appends `s` to out[*at], keeping a NUL; 0 when it does not fit. */
static int ui_cat(char *out, unsigned long outlen, unsigned long *at,
                  const char *s, unsigned long n)
{
    unsigned long i;

    if (*at + n + 1 > outlen)
        return 0;
    for (i = 0; i < n; i++)
        out[(*at)++] = s[i];
    out[*at] = '\0';
    return 1;
}

static unsigned long ui_utoa(unsigned long v, char *out)
{
    char          t[12];
    unsigned long n = 0, i;

    do
    {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (i = 0; i < n; i++)
        out[i] = t[n - 1 - i];
    out[n] = '\0';
    return n;
}

void upd_hex32(const unsigned char *b, char *out)
{
    static const char hex[] = "0123456789abcdef";
    unsigned int i;

    for (i = 0; i < 32; i++)
    {
        out[2 * i]     = hex[b[i] >> 4];
        out[2 * i + 1] = hex[b[i] & 15];
    }
    out[64] = '\0';
}

/* ------------------------------------------------------------ receipt --- */

int upd_receipt_parse(const char *text, unsigned long len, UpdRcpt *out,
                      unsigned int max, unsigned int *n)
{
    unsigned long i = 0;
    unsigned int  k = 0;

    *n = 0;
    while (i < len)
    {
        unsigned long e = i, p, j;

        while (e < len && text[e] != '\n')
            e++;
        if (e >= len)
            return 0;                   /* no LF: cut while it was written */
        if (k >= max || e - i < 66 || text[i + 64] != ' ')
            return 0;
        for (j = 0; j < 32; j++)
        {
            int hi = ui_nibble((unsigned char)text[i + 2 * j]);
            int lo = ui_nibble((unsigned char)text[i + 2 * j + 1]);

            if (hi < 0 || lo < 0)
                return 0;
            out[k].sha256[j] = (unsigned char)(hi << 4 | lo);
        }
        p = i + 65;
        if (e - p + 1 > UPD_APATH_MAX)
            return 0;
        for (j = p; j < e; j++)
            if (!ui_pathbyte((unsigned char)text[j]) && text[j] != ' ')
                return 0;
        out[k].path = text + p;
        out[k].len  = (unsigned short)(e - p);
        k++;
        i = e + 1;
    }
    if (k == 0)
        return 0;
    *n = k;
    return 1;
}

int upd_receipt_find(const UpdRcpt *r, unsigned int n, const char *path,
                     unsigned long plen)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        if (ui_same(r[i].path, r[i].len, path, plen))
            return (int)i;
    return -1;
}

int upd_receipt_layout(const UpdRcpt *r, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        if (ui_prefix(r[i].path, r[i].len, "AmiNetXDuo:") != 0)
            return UPD_LAYOUT_DRAWER;
    return UPD_LAYOUT_SYSTEM;
}

static const char *const ui_dest_system[UPD_CLASS_COUNT] = {
    "LIBS:", "DEVS:Networks/", "C:", "DEVS:Internet/"
};
static const char *const ui_dest_drawer[UPD_CLASS_COUNT] = {
    "AmiNetXDuo:Libs/", "AmiNetXDuo:Devs/Networks/", "AmiNetXDuo:C/",
    "AmiNetXDuo:Devs/Internet/"
};
/* The container's member names: tools/make-update-manifest.sh. */
static const char *const ui_member[UPD_CLASS_COUNT] = {
    "Libs/", "Devs/Networks/", "C/", "Devs/Internet/"
};

int upd_dest(char *out, unsigned long outlen, int layout, unsigned int cls,
             const char *path, unsigned long plen)
{
    const char   *pre;
    unsigned long at = 0;

    if (cls >= UPD_CLASS_COUNT || outlen == 0)
        return 0;
    pre = layout == UPD_LAYOUT_DRAWER ? ui_dest_drawer[cls] : ui_dest_system[cls];
    out[0] = '\0';
    return ui_cat(out, outlen, &at, pre, ui_len(pre)) &&
           ui_cat(out, outlen, &at, path, plen);
}

/* ----------------------------------------------------------- selection --- */

static const char ui_ver_key[] = "$VER: ";

void upd_scan_init(UpdScan *s)
{
    s->mark = s->mpos = s->vpos = s->vdone = s->vlen = 0;
    s->ver[0] = '\0';
}

void upd_scan_feed(UpdScan *s, const unsigned char *d, unsigned long n)
{
    static const char    mark[] = UPD_MARK;
    const unsigned long  vk = sizeof(ui_ver_key) - 1;
    unsigned long        i;

    for (i = 0; i < n; i++)
    {
        unsigned char c = d[i];

        /* Neither needle has a proper prefix that is also a suffix, so a
           mismatch restarts at the byte itself. */
        if (!s->mark)
        {
            if (c == (unsigned char)mark[s->mpos])
            {
                if (++s->mpos == sizeof(mark) - 1)
                    s->mark = 1;
            }
            else
            {
                s->mpos = (unsigned char)(c == (unsigned char)mark[0] ? 1 : 0);
            }
        }

        if (s->vdone)
        {
            if (s->mark)
                return;
            continue;
        }
        if (s->vpos < vk)
        {
            if (c == (unsigned char)ui_ver_key[s->vpos])
                s->vpos++;
            else
                s->vpos = (unsigned char)(c == (unsigned char)ui_ver_key[0] ? 1 : 0);
            continue;
        }
        if (c == 0 || c == '\n' || c == '\r' || s->vlen >= sizeof(s->ver) - 1)
        {
            s->ver[s->vlen] = '\0';
            s->vdone = 1;
            continue;
        }
        s->ver[s->vlen++] = (char)c;
        s->ver[s->vlen] = '\0';
    }
}

int upd_scan_version(const UpdScan *s, UpdVer *v)
{
    unsigned long i = 0, b;

    if (s->vpos < sizeof(ui_ver_key) - 1)
        return 0;
    while (i < s->vlen && s->ver[i] == ' ')
        i++;
    while (i < s->vlen && s->ver[i] != ' ')
        i++;                            /* the name                        */
    while (i < s->vlen && s->ver[i] == ' ')
        i++;
    b = i;
    while (i < s->vlen && s->ver[i] != ' ' && s->ver[i] != '(')
        i++;
    if (i == b)
        return 0;
    return upd_ver_parse(s->ver + b, i - b, v);
}

void upd_select(const UpdSelIn *in, unsigned char *act, unsigned char *why)
{
    UpdVer v;

    *act = UPD_ACT_NONE;
    *why = UPD_WHY_OURS;
    if (in->receipt == 0)
        return;
    if (in->disk == 0)
    {
        *act = UPD_ACT_MISSING;
        *why = UPD_WHY_DELETED;
    }
    else if (ui_eq32(in->disk, in->fresh))
    {
        *act = UPD_ACT_SKIP;
        *why = UPD_WHY_CURRENT;
    }
    else if (ui_eq32(in->disk, in->receipt))
    {
        *act = UPD_ACT_REPLACE;
        *why = UPD_WHY_OURS;
    }
    else if (in->scan != 0 && in->scan->mark)
    {
        if (upd_scan_version(in->scan, &v) && upd_ver_cmp(&v, in->update) > 0)
        {
            *act = UPD_ACT_SKIP;
            *why = UPD_WHY_NEWER;
        }
        else
        {
            *act = UPD_ACT_REPLACE;
            *why = UPD_WHY_OTHERBUILD;
        }
    }
    else
    {
        *act = UPD_ACT_KEEP;
        *why = UPD_WHY_FOREIGN;
    }
}

static const char *const ui_acts[] = {
    "none", "replace", "skip", "keep", "missing"
};
static const char *const ui_whys[UPD_WHY_COUNT] = {
    "ours", "current", "otherbuild", "newer", "foreign", "deleted"
};

const char *upd_act_name(unsigned int a)
{
    return a < sizeof(ui_acts) / sizeof(ui_acts[0]) ? ui_acts[a] : 0;
}

const char *upd_why_name(unsigned int w)
{
    return w < UPD_WHY_COUNT ? ui_whys[w] : 0;
}

/* ----------------------------------------------------------- container --- */

static unsigned long ui_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 |
           (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

static unsigned int ui_le16(const unsigned char *p)
{
    return (unsigned int)p[0] | (unsigned int)p[1] << 8;
}

long upd_lha_base(const unsigned char *h, unsigned long n, UpdLha *m)
{
    const unsigned char *b;
    unsigned long        hl, i, sum = 0;
    unsigned int         nl;

    if (n < 1)
        return -2;
    if (h[0] == 0)
        return 0;
    hl = (unsigned long)h[0];
    if (n < hl + 2)
        return -2;
    b = h + 2;
    for (i = 0; i < hl; i++)
        sum += b[i];
    if ((sum & 0xff) != h[1] || hl < 25)
        return -1;
    if (b[0] != '-' || b[1] != 'l' || b[2] != 'h' || b[3] != '0' ||
        b[4] != '-' || b[18] != 1)
        return -1;
    nl = b[19];
    if (nl == 0 || 20 + nl + 5 != hl || nl > sizeof(m->name) - 1)
        return -1;
    for (i = 0; i < nl; i++)
    {
        unsigned char c = b[20 + i];

        if (c <= ' ' || c >= 0x7f || c == '/' || c == ':')
            return -1;
        m->name[i] = (char)c;
    }
    m->name[nl]  = '\0';
    m->name_len  = nl;
    m->skip      = ui_le32(b + 5);
    m->size      = ui_le32(b + 9);
    m->ext_first = ui_le16(b + 20 + nl + 3);
    if (m->skip < m->size || m->skip - m->size > UPD_LHA_EXT_MAX)
        return -1;
    return (long)(hl + 2);
}

int upd_lha_ext(const unsigned char *e, unsigned long n, UpdLha *m)
{
    char          dir[sizeof(m->name)];
    unsigned long pos = 0, dl = 0, i;
    unsigned int  size = m->ext_first;

    while (size != 0)
    {
        if (size < 3 || pos + size > n)
            return 0;
        if (e[pos] == 0x02)
        {
            dl = 0;
            for (i = pos + 1; i < pos + size - 2; i++)
            {
                unsigned char c = e[i];

                if (c == 0xff)
                    c = '/';
                else if (c <= ' ' || c >= 0x7f || c == '/' || c == ':')
                    return 0;
                if (dl >= sizeof(dir) - 1)
                    return 0;
                dir[dl++] = (char)c;
            }
            if (dl == 0 || dir[dl - 1] != '/' || dir[0] == '/')
                return 0;
        }
        pos += size;
        size = ui_le16(e + pos - 2);
    }
    if (pos != n || dl + m->name_len + 1 > sizeof(m->name))
        return 0;
    if (dl != 0)
    {
        for (i = m->name_len + 1; i-- > 0;)
            m->name[dl + i] = m->name[i];
        for (i = 0; i < dl; i++)
            m->name[i] = dir[i];
        m->name_len += (unsigned int)dl;
    }
    return 1;
}

int upd_member_file(const char *name, unsigned long nlen,
                    const UpdFile *files, unsigned int nfiles)
{
    unsigned int  i;

    for (i = 0; i < nfiles; i++)
    {
        unsigned long p;

        if (files[i].cls >= UPD_CLASS_COUNT)
            continue;
        p = ui_prefix(name, nlen, ui_member[files[i].cls]);
        if (p != 0 && ui_same(name + p, nlen - p, files[i].path, files[i].path_len))
            return (int)i;
    }
    return -1;
}

unsigned long upd_blocks(unsigned long size, unsigned long bpb)
{
    unsigned long data, d;

    if (bpb <= 24)
        bpb = 512;
    data = bpb - 24;
    d = size / data + (size % data != 0);
    return d + (d + 71) / 72 + 1;
}

int upd_dl_chunk(int opened, unsigned long total, unsigned long n,
                 unsigned long size)
{
    if (!opened)
        return UPD_DL_NETWORK;
    if (total + n > size)
        return UPD_DL_LONG;
    return UPD_DL_OK;
}

int upd_dl_verdict(long status, int whole, int wfail, int toolong,
                   unsigned long total, unsigned long size)
{
    if (status != 200)
        return UPD_DL_NETWORK;
    if (wfail)
        return UPD_DL_SPACE;
    if (toolong)
        return UPD_DL_LONG;
    if (!whole || total < size)
        return UPD_DL_NETWORK;
    return UPD_DL_OK;
}

/* ------------------------------------------------------------- journal --- */

/* The text of one line as key and value; 1 when it is "key=value". */
static int ui_kv(const char *l, unsigned long n, const char *key,
                 const char **v, unsigned long *vn)
{
    unsigned long k = ui_len(key);

    if (n <= k || l[k] != '=')
        return 0;
    for (n = 0; n < k; n++)
        if (l[n] != key[n])
            return 0;
    *v = l + k + 1;
    *vn = 0;
    while ((*v)[*vn] != '\n')
        (*vn)++;
    return 1;
}

/* "<word> <n>" with n from 1 to `max`; the index from 0, or -1. */
static long ui_word_n(const char *l, unsigned long n, const char *word,
                      unsigned int max)
{
    unsigned long k = ui_len(word), i, v = 0;

    if (n < k + 2 || l[k] != ' ')
        return -1;
    for (i = 0; i < k; i++)
        if (l[i] != word[i])
            return -1;
    if (l[k + 1] == '0')
        return -1;
    for (i = k + 1; i < n; i++)
    {
        if (l[i] < '0' || l[i] > '9' || v > 99999UL)
            return -1;
        v = v * 10 + (unsigned long)(l[i] - '0');
    }
    if (v == 0 || v > max)
        return -1;
    return (long)(v - 1);
}

int upd_journal_parse(const char *text, unsigned long len, UpdJournal *j,
                      UpdJEntry *e, unsigned int max)
{
    unsigned long i = 0, line = 0;
    unsigned int  have = 0;     /* 1 version, 2 from, 4 flavour           */

    j->ready = 0;
    j->nfiles = 0;
    j->flavour = 0;
    while (i < len)
    {
        const char   *l = text + i, *v;
        unsigned long n = 0, vn;
        long          k;

        while (i + n < len && l[n] != '\n')
            n++;
        if (i + n >= len)
            break;                      /* cut mid-append: not there      */
        i += n + 1;

        if (line++ == 0)
        {
            if (n != 8 || ui_prefix(l, n, "format=1") != 8)
                return 0;
            continue;
        }
        if (!j->ready)
        {
            if (ui_kv(l, n, "version", &v, &vn))
            {
                if ((have & 1) || !upd_ver_parse(v, vn, &j->version))
                    return 0;
                have |= 1;
            }
            else if (ui_kv(l, n, "from", &v, &vn))
            {
                if ((have & 2) || !upd_ver_parse(v, vn, &j->from))
                    return 0;
                have |= 2;
            }
            else if (ui_kv(l, n, "flavour", &v, &vn))
            {
                unsigned int f;

                for (f = 0; f < UPD_FLAVOUR_COUNT; f++)
                    if (ui_same(v, vn, upd_flavour_name(f), ui_len(upd_flavour_name(f))))
                        break;
                if ((have & 4) || f == UPD_FLAVOUR_COUNT)
                    return 0;
                j->flavour = (unsigned char)f;
                have |= 4;
            }
            else if (ui_kv(l, n, "file", &v, &vn))
            {
                unsigned long p = 0, q;
                unsigned char why;

                while (p < vn && v[p] != ' ')
                    p++;
                if (ui_same(v, p, "ours", 4))
                    why = UPD_WHY_OURS;
                else if (ui_same(v, p, "otherbuild", 10))
                    why = UPD_WHY_OTHERBUILD;
                else
                    return 0;
                p++;
                if (p >= vn || vn - p + 1 > UPD_APATH_MAX || j->nfiles >= max)
                    return 0;
                for (q = p; q < vn; q++)
                    if (!ui_pathbyte((unsigned char)v[q]) && v[q] != ' ')
                        return 0;
                e[j->nfiles].path  = v + p;
                e[j->nfiles].len   = (unsigned short)(vn - p);
                e[j->nfiles].why   = why;
                e[j->nfiles].aside = 0;
                e[j->nfiles].done  = 0;
                e[j->nfiles].staged = 0;
                j->nfiles++;
            }
            else if (n == 5 && ui_prefix(l, n, "ready") == 5)
            {
                if (have != 7)
                    return 0;
                j->ready = 1;
            }
            else
            {
                return 0;
            }
        }
        else if ((k = ui_word_n(l, n, "aside", j->nfiles)) >= 0)
        {
            e[k].aside = 1;
        }
        else if ((k = ui_word_n(l, n, "done", j->nfiles)) >= 0)
        {
            e[k].done = 1;
        }
        else
        {
            return 0;
        }
    }
    return line > 0;
}

int upd_stale_journal(const char *text, unsigned long len, int absent,
                      UpdJournal *j, UpdJEntry *e, unsigned int max)
{
    if (text == 0)
        return absent ? UPD_STALE_NONE : UPD_STALE_PENDING;
    if (e == 0 || !upd_journal_parse(text, len, j, e, max) || j->ready)
        return UPD_STALE_PENDING;
    return UPD_STALE_UNDO;
}

/* ---------------------------------------------------------------- swap --- */

typedef struct UiNames
{
    char live[UPD_APATH_MAX];
    char fresh[UPD_APATH_MAX + 4];
    char old[UPD_APATH_MAX + 4];
    char prior[UPD_APATH_MAX + 8];
    char tmp[UPD_APATH_MAX + 12];
    char line[16];
} UiNames;

static int ui_names(UiNames *nm, const UpdJEntry *e, unsigned int i,
                    const char *prior)
{
    char          num[12];
    unsigned long at, nn = ui_utoa((unsigned long)i + 1, num);

    at = 0;
    if (!ui_cat(nm->live, sizeof(nm->live), &at, e->path, e->len))
        return 0;
    if (!upd_sidecar(nm->fresh, sizeof(nm->fresh), e->path, e->len, ".new") ||
        !upd_sidecar(nm->old, sizeof(nm->old), e->path, e->len, ".old"))
        return 0;
    at = 0;
    if (!ui_cat(nm->prior, sizeof(nm->prior), &at, prior, ui_len(prior)) ||
        !ui_cat(nm->prior, sizeof(nm->prior), &at, "/", 1) ||
        !ui_cat(nm->prior, sizeof(nm->prior), &at, num, nn))
        return 0;
    at = 0;
    if (!ui_cat(nm->tmp, sizeof(nm->tmp), &at, nm->prior, ui_len(nm->prior)) ||
        !ui_cat(nm->tmp, sizeof(nm->tmp), &at, ".tmp", 4))
        return 0;
    return 1;
}

static int ui_jline(const UpdFsOps *ops, UiNames *nm, const char *word,
                    unsigned int i)
{
    unsigned long at = 0, k = ui_len(word);
    char          num[12];
    unsigned long nn = ui_utoa((unsigned long)i + 1, num);

    if (!ui_cat(nm->line, sizeof(nm->line), &at, word, k) ||
        !ui_cat(nm->line, sizeof(nm->line), &at, " ", 1) ||
        !ui_cat(nm->line, sizeof(nm->line), &at, num, nn))
        return 0;
    return ops->journal(ops->ctx, nm->line);
}

int upd_swap_fits(const UpdJEntry *e, unsigned int i, const char *prior)
{
    char          num[12];
    unsigned long q, nn = ui_utoa((unsigned long)i + 1, num);

    /* As upd_journal_parse() reads a file= path back. */
    if (e->len == 0 || e->len + 1 > UPD_APATH_MAX)
        return 0;
    for (q = 0; q < e->len; q++)
        if (!ui_pathbyte((unsigned char)e->path[q]) && e->path[q] != ' ')
            return 0;
    /* ui_names(): .new and .old fit when <path> does; <prior>/<n>.tmp. */
    return ui_len(prior) + 1 + nn + 4 + 1 <= sizeof(((UiNames *)0)->tmp);
}

/* <path>.new of `e` into ui_new; 0 when it does not fit. */
static char ui_new[UPD_APATH_MAX + 4];

static int ui_new_name(const UpdJEntry *e)
{
    return upd_sidecar(ui_new, sizeof(ui_new), e->path, e->len, ".new");
}

/* What of `path` a sidecar with an `extlen` suffix keeps. */
static unsigned long ui_side_keep(const char *path, unsigned long len,
                                  unsigned long extlen)
{
    unsigned long c = len;

    while (c > 0 && path[c - 1] != '/' && path[c - 1] != ':')
        c--;
    if (len - c + extlen > UPD_FNAME_MAX)
        return c + UPD_FNAME_MAX - extlen;
    return len;
}

int upd_sidecar(char *out, unsigned long cap, const char *path,
                unsigned long len, const char *ext)
{
    unsigned long at = 0, k = ui_len(ext);

    return ui_cat(out, cap, &at, path, ui_side_keep(path, len, k)) &&
           ui_cat(out, cap, &at, ext, k);
}

int upd_sidecars_distinct(const UpdJEntry *e, unsigned int n, unsigned int *bad)
{
    unsigned int i, k;

    for (i = 1; i < n; i++)
        for (k = 0; k < i; k++)
        {
            unsigned long a = ui_side_keep(e[i].path, e[i].len, 4);

            if (ui_same(e[i].path, a, e[k].path, ui_side_keep(e[k].path, e[k].len, 4)))
            {
                if (bad != 0)
                    *bad = i;
                return 0;
            }
        }
    return 1;
}

int upd_all_staged(const UpdJEntry *e, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        if (!e[i].staged)
            return 0;
    return 1;
}

int upd_new_free(const UpdJEntry *e, unsigned int n, const UpdFsOps *ops,
                 unsigned int *bad)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        if (!ui_new_name(&e[i]) || ops->exists(ops->ctx, ui_new))
        {
            if (bad != 0)
                *bad = i;
            return 0;
        }
    return 1;
}

void upd_unstage_new(UpdJEntry *e, unsigned int n, const UpdFsOps *ops)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        if (e[i].staged && ui_new_name(&e[i]) && ops->remove(ops->ctx, ui_new))
            e[i].staged = 0;
}

int upd_forward(UpdJEntry *e, unsigned int n, const char *prior,
                const UpdFsOps *ops, unsigned int *failed)
{
    static UiNames nm;
    unsigned int   i;

    for (i = 0; i < n; i++)
    {
        int has_new, has_live;

        if (failed != 0)
            *failed = i;
        if (e[i].done)
            continue;
        if (!ui_names(&nm, &e[i], i, prior))
            return 0;

        has_new  = ops->exists(ops->ctx, nm.fresh);
        has_live = ops->exists(ops->ctx, nm.live);

        if (has_new && has_live)
        {
            /* The live file is not moved yet, so a .old is the one that
               was there before: another stack's, or an earlier install's. */
            if (ops->exists(ops->ctx, nm.old))
            {
                if (!e[i].aside)
                {
                    /* Not this swap's: an earlier update's.  Deleting the
                       .old on its strength would lose the .old. */
                    if (ops->exists(ops->ctx, nm.prior))
                        return 0;
                    if (!ui_jline(ops, &nm, "aside", i))
                        return 0;
                    e[i].aside = 1;
                }
                if (ops->exists(ops->ctx, nm.prior))
                {
                    /* Copied whole before; the original is still there. */
                    if (!ops->remove(ops->ctx, nm.old))
                        return 0;
                }
                else if (!ops->rename(ops->ctx, nm.old, nm.prior))
                {
                    /* Another volume: a copy under a name of its own, so
                       that <prior>/<n> exists only once it is whole. */
                    if (!ops->remove(ops->ctx, nm.tmp) ||
                        !ops->copy(ops->ctx, nm.old, nm.tmp) ||
                        !ops->rename(ops->ctx, nm.tmp, nm.prior) ||
                        !ops->remove(ops->ctx, nm.old))
                        return 0;
                }
                if (!ops->cut(ops->ctx))
                    return 0;
            }
            if (!ops->rename(ops->ctx, nm.live, nm.old) || !ops->cut(ops->ctx))
                return 0;
            if (!ops->rename(ops->ctx, nm.fresh, nm.live) || !ops->cut(ops->ctx))
                return 0;
        }
        else if (has_new)
        {
            /* Cut between the two renames: the old one is .old already. */
            if (!ops->rename(ops->ctx, nm.fresh, nm.live) || !ops->cut(ops->ctx))
                return 0;
        }
        else if (!has_live)
        {
            return -1;
        }

        if (!ui_jline(ops, &nm, "done", i))
            return 0;
        e[i].done = 1;
        if (!ops->cut(ops->ctx))
            return 0;
    }
    if (failed != 0)
        *failed = n;
    return 1;
}

unsigned int upd_forward_steps(unsigned int n, unsigned int olds)
{
    return 3 * n + olds;
}

/* ---------------------------------------------------------------- state --- */

unsigned long upd_state_merge(const char *old, unsigned long oldlen,
                              const char *phase, const char *version,
                              unsigned long boots, const char *flavour,
                              char *out, unsigned long outlen)
{
    static const char *const keys[] = { "phase=", "version=", "boots=", "flavour=" };
    char          num[12];
    unsigned long at = 0, i = 0;

    if (outlen == 0)
        return 0;
    out[0] = '\0';
    if (!ui_cat(out, outlen, &at, "phase=", 6) ||
        !ui_cat(out, outlen, &at, phase, ui_len(phase)) ||
        !ui_cat(out, outlen, &at, "\nversion=", 9) ||
        !ui_cat(out, outlen, &at, version, ui_len(version)) ||
        !ui_cat(out, outlen, &at, "\nboots=", 7) ||
        !ui_cat(out, outlen, &at, num, ui_utoa(boots, num)) ||
        !ui_cat(out, outlen, &at, "\nflavour=", 9) ||
        !ui_cat(out, outlen, &at, flavour, ui_len(flavour)) ||
        !ui_cat(out, outlen, &at, "\n", 1))
        return 0;

    while (i < oldlen)
    {
        unsigned long n = 0, k;
        int           set = 0;

        while (i + n < oldlen && old[i + n] != '\n')
            n++;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (ui_prefix(old + i, n, keys[k]) != 0)
                set = 1;
        if (!set && n > 0 &&
            (!ui_cat(out, outlen, &at, old + i, n) ||
             !ui_cat(out, outlen, &at, "\n", 1)))
            return 0;
        i += n + 1;
    }
    return at;
}

int upd_trial_active(int trial, const char *state, unsigned long len)
{
    unsigned long i = 0;

    if (trial)
        return 1;
    while (state != 0 && i < len)
    {
        unsigned long n = 0;

        while (i + n < len && state[i + n] != '\n')
            n++;
        if (n == 11 && ui_prefix(state + i, n, "phase=trial") == 11)
            return 1;
        i += n + 1;
    }
    return 0;
}

int upd_lock_why(long ioerr)
{
    switch (ioerr)
    {
    case 202:                           /* ERROR_OBJECT_IN_USE         */
        return UPD_LOCK_BUSY;
    case 214:                           /* ERROR_DISK_WRITE_PROTECTED  */
    case 223:                           /* ERROR_WRITE_PROTECTED       */
        return UPD_LOCK_WP;
    default:
        return UPD_LOCK_OTHER;
    }
}
