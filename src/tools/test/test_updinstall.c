/*
 * The tests for src/tools/updinstall.c, the half of `UpdateAmiNetXDuo
 * INSTALL` that is not I/O.
 *
 *   test_updinstall select    the receipt, the destinations, the mark and
 *                             $VER scan, the selection rule, the container
 *                             member headers, the block count
 *   test_updinstall journal   the journal parse, the state merge, and the
 *                             swap over an in-memory volume: stopped after
 *                             every single operation (a power cut there),
 *                             then finished from the journal it left, and
 *                             a journal without "ready" undone
 *
 * SPDX-License-Identifier: MIT
 */

#include "updinstall.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

static void fill(unsigned char *h, unsigned char v)
{
    memset(h, v, 32);
}

static const char *hexof(unsigned char v)
{
    static char out[65];
    unsigned char b[32];

    fill(b, v);
    upd_hex32(b, out);
    return out;
}

/* ------------------------------------------------------------ receipt --- */

static void test_receipt(void)
{
    static char   text[4096];
    UpdRcpt       r[8];
    unsigned int  n;
    char          d[UPD_APATH_MAX];

    char          h1[65];

    snprintf(h1, sizeof(h1), "%s", hexof(0x11));
    snprintf(text, sizeof(text), "%s C:ping\n%s LIBS:bsdsocket.library\n",
             h1, hexof(0xab));
    CHECK(upd_receipt_parse(text, strlen(text), r, 8, &n) == 1 && n == 2);
    CHECK(r[0].len == 6 && memcmp(r[0].path, "C:ping", 6) == 0);
    CHECK(r[1].sha256[0] == 0xab && r[1].sha256[31] == 0xab);
    CHECK(upd_receipt_find(r, n, "c:PING", 6) == 0);
    CHECK(upd_receipt_find(r, n, "libs:bsdsocket.library", 22) == 1);
    CHECK(upd_receipt_find(r, n, "C:pin", 5) == -1);
    CHECK(upd_receipt_layout(r, n) == UPD_LAYOUT_SYSTEM);

    /* Truncated (no LF), empty, upper-case hex, too many, a short hash. */
    CHECK(upd_receipt_parse(text, strlen(text) - 1, r, 8, &n) == 0);
    CHECK(upd_receipt_parse("", 0, r, 8, &n) == 0);
    CHECK(upd_receipt_parse(text, strlen(text), r, 1, &n) == 0);
    snprintf(text, sizeof(text), "%s C:ping\n", hexof(0xab));
    text[0] = 'A';
    CHECK(upd_receipt_parse(text, strlen(text), r, 8, &n) == 0);
    CHECK(upd_receipt_parse("abcd C:ping\n", 12, r, 8, &n) == 0);
    snprintf(text, sizeof(text), "%s\n", hexof(0x01));
    CHECK(upd_receipt_parse(text, strlen(text), r, 8, &n) == 0);
    snprintf(text, sizeof(text), "%s C:a\tb\n", hexof(0x01));
    CHECK(upd_receipt_parse(text, strlen(text), r, 8, &n) == 0);

    snprintf(text, sizeof(text), "%s AmiNetXDuo:C/ping\n", hexof(0x22));
    CHECK(upd_receipt_parse(text, strlen(text), r, 8, &n) == 1 && n == 1);
    CHECK(upd_receipt_layout(r, n) == UPD_LAYOUT_DRAWER);

    CHECK(upd_dest(d, sizeof(d), UPD_LAYOUT_SYSTEM, UPD_CLASS_C, "ping", 4) &&
          strcmp(d, "C:ping") == 0);
    CHECK(upd_dest(d, sizeof(d), UPD_LAYOUT_SYSTEM, UPD_CLASS_NETWORKS,
                   "anxnet.device", 13) &&
          strcmp(d, "DEVS:Networks/anxnet.device") == 0);
    CHECK(upd_dest(d, sizeof(d), UPD_LAYOUT_DRAWER, UPD_CLASS_LIBS,
                   "tls.library", 11) &&
          strcmp(d, "AmiNetXDuo:Libs/tls.library") == 0);
    CHECK(upd_dest(d, sizeof(d), UPD_LAYOUT_DRAWER, UPD_CLASS_CERTIFICATES,
                   "certificates", 12) &&
          strcmp(d, "AmiNetXDuo:Devs/Internet/certificates") == 0);
    CHECK(upd_dest(d, 8, UPD_LAYOUT_DRAWER, UPD_CLASS_C, "ping", 4) == 0);
    CHECK(upd_dest(d, sizeof(d), UPD_LAYOUT_SYSTEM, UPD_CLASS_COUNT, "x", 1) == 0);
}

/* ---------------------------------------------------------------- scan --- */

static UpdScan scan_chunks(const char *s, unsigned long step)
{
    UpdScan       sc;
    unsigned long n = strlen(s), i;

    upd_scan_init(&sc);
    for (i = 0; i < n; i += step)
        upd_scan_feed(&sc, (const unsigned char *)s + i,
                      i + step <= n ? step : n - i);
    return sc;
}

static void test_scan(void)
{
    static const char ours[] =
        "\0\x03junk AmiNe AmiNetXDu $VER: ping 1.0.0-rc4 (1.1.2026) AmiNetXDuo abc\0tail";
    UpdScan       s;
    UpdVer        v, w;
    unsigned long step;

    for (step = 1; step <= 9; step++)
    {
        UpdScan sc;
        unsigned long i;

        upd_scan_init(&sc);
        for (i = 0; i < sizeof(ours) - 1; i += step)
            upd_scan_feed(&sc, (const unsigned char *)ours + i,
                          i + step <= sizeof(ours) - 1 ? step : sizeof(ours) - 1 - i);
        CHECK(sc.mark == 1);
        CHECK(upd_scan_version(&sc, &v) == 1);
        CHECK(upd_ver_parse("1.0.0-rc4", 9, &w) && upd_ver_cmp(&v, &w) == 0);
    }

    s = scan_chunks("a ping from another vendor\n$VER: ping 3.2 (1.1.2020)\n", 3);
    CHECK(s.mark == 0);
    CHECK(upd_scan_version(&s, &v) == 0);       /* "3.2" is not ours      */

    s = scan_chunks("$VER: ping 9.9.9 (1.1.2030) AmiNetXDuo, a newer build\n", 2);
    CHECK(s.mark == 1 && upd_scan_version(&s, &v) == 1 && v.major == 9);

    s = scan_chunks("$VER: bsdsocket.library 1.0.0 (1.1.2026)", 5);
    CHECK(s.mark == 0 && upd_scan_version(&s, &v) == 1 && v.pre[0] == '\0');

    s = scan_chunks("no version here AmiNetXDuo", 4);
    CHECK(s.mark == 1 && upd_scan_version(&s, &v) == 0);

    /* "$$VER: " and "AAmiNetXDuo": a mismatch restarts at that byte. */
    s = scan_chunks("$$VER: x 1.2.3 AAmiNetXDuo", 1);
    CHECK(s.mark == 1 && upd_scan_version(&s, &v) == 1 && v.minor == 2);
}

/* ----------------------------------------------------------- selection --- */

static void test_select(void)
{
    unsigned char r[32], d[32], f[32], act, why;
    UpdScan       mark, foreign, newer;
    UpdVer        upd;
    UpdSelIn      in;

    fill(r, 1);
    fill(f, 2);
    CHECK(upd_ver_parse("1.0.0-rc5", 9, &upd));
    mark = scan_chunks("$VER: ping 1.0.0-rc1 (1.1.2026) AmiNetXDuo, another build\n", 7);
    newer = scan_chunks("$VER: ping 9.9.9 (1.1.2030) AmiNetXDuo, a newer build\n", 7);
    foreign = scan_chunks("a ping from another vendor\n$VER: ping 3.2 (1.1.2020)\n", 7);

    memset(&in, 0, sizeof(in));
    in.fresh = f;
    in.update = &upd;

    /* Not in the receipt: never touched, on disk or not. */
    in.receipt = 0; in.disk = d; in.scan = &mark;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_NONE);

    in.receipt = r; in.disk = 0; in.scan = 0;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_MISSING && why == UPD_WHY_DELETED);

    fill(d, 2); in.disk = d; in.scan = &foreign;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_SKIP && why == UPD_WHY_CURRENT);

    fill(d, 1); in.scan = &foreign;     /* untouched: the mark not needed */
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_REPLACE && why == UPD_WHY_OURS);

    fill(d, 3); in.scan = &mark;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_REPLACE && why == UPD_WHY_OTHERBUILD);

    in.scan = &newer;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_SKIP && why == UPD_WHY_NEWER);

    in.scan = &foreign;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_KEEP && why == UPD_WHY_FOREIGN);

    /* The same version, other bytes: another build, replaced. */
    mark = scan_chunks("$VER: ping 1.0.0-rc5 (2.1.2026) AmiNetXDuo\n", 64);
    in.scan = &mark;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_REPLACE && why == UPD_WHY_OTHERBUILD);

    CHECK(strcmp(upd_act_name(UPD_ACT_KEEP), "keep") == 0);
    CHECK(strcmp(upd_why_name(UPD_WHY_OTHERBUILD), "otherbuild") == 0);
    CHECK(upd_why_name(UPD_WHY_COUNT) == 0 && upd_act_name(9) == 0);
}

/* ----------------------------------------------------------- container --- */

static unsigned int crc16(const unsigned char *d, unsigned long n)
{
    unsigned int crc = 0, k;

    while (n--)
    {
        crc ^= *d++;
        for (k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/* One member as dist/lhapack.py writes it. */
static unsigned long lha_entry(unsigned char *out, const char *dir,
                               const char *name, const unsigned char *data,
                               unsigned long size)
{
    unsigned char ext[300], body[300];
    unsigned long en = 0, bn = 0, i, nl = strlen(name), dl = strlen(dir);
    unsigned int  sum = 0, crc = crc16(data, size);

    if (dl)
    {
        unsigned long bl = 1 + dl + 1;      /* type, dir with 0xff, final */

        ext[en++] = (unsigned char)((bl + 2) & 0xff);
        ext[en++] = (unsigned char)((bl + 2) >> 8);
        ext[en++] = 0x02;
        for (i = 0; i < dl; i++)
            ext[en++] = dir[i] == '/' ? 0xff : (unsigned char)dir[i];
        ext[en++] = 0xff;
    }
    ext[en++] = 0;
    ext[en++] = 0;

    memcpy(body, "-lh0-", 5); bn = 5;
#define LE32(v) do { unsigned long _v = (v); body[bn++] = _v & 0xff;            \
    body[bn++] = (_v >> 8) & 0xff; body[bn++] = (_v >> 16) & 0xff;              \
    body[bn++] = (_v >> 24) & 0xff; } while (0)
    LE32(size + en - 2);
    LE32(size);
    LE32(0x00210000UL);
#undef LE32
    body[bn++] = 0x20;
    body[bn++] = 0x01;
    body[bn++] = (unsigned char)nl;
    memcpy(body + bn, name, nl); bn += nl;
    body[bn++] = (unsigned char)(crc & 0xff);
    body[bn++] = (unsigned char)(crc >> 8);
    body[bn++] = 'A';
    body[bn++] = ext[0];
    body[bn++] = ext[1];
    for (i = 0; i < bn; i++)
        sum += body[i];

    out[0] = (unsigned char)bn;
    out[1] = (unsigned char)(sum & 0xff);
    memcpy(out + 2, body, bn);
    memcpy(out + 2 + bn, ext + 2, en - 2);
    memcpy(out + 2 + bn + en - 2, data, size);
    return 2 + bn + en - 2 + size;
}

static void test_container(void)
{
    static unsigned char arc[4096];
    static const unsigned char d1[] = "ping bytes";
    static const unsigned char d2[] = "certs";
    UpdFile       files[3];
    unsigned long at = 0, n1;
    UpdLha        m;
    long          hl;

    memset(files, 0, sizeof(files));
    files[0].path = "bsdsocket.library"; files[0].path_len = 17; files[0].cls = UPD_CLASS_LIBS;
    files[1].path = "ping";   files[1].path_len = 4;  files[1].cls = UPD_CLASS_C;
    files[2].path = "certificates"; files[2].path_len = 12; files[2].cls = UPD_CLASS_CERTIFICATES;

    n1 = lha_entry(arc, "C", "ping", d1, sizeof(d1) - 1);
    at = n1;
    at += lha_entry(arc + at, "Devs/Internet", "certificates", d2, sizeof(d2) - 1);
    arc[at++] = 0;

    CHECK(upd_lha_base(arc, 1, &m) == -2);
    hl = upd_lha_base(arc, at, &m);
    CHECK(hl > 0 && m.size == sizeof(d1) - 1);
    CHECK(upd_lha_ext(arc + hl, m.skip - m.size, &m) == 1);
    CHECK(strcmp(m.name, "C/ping") == 0 && m.name_len == 6);
    CHECK(memcmp(arc + hl + (m.skip - m.size), d1, m.size) == 0);
    CHECK(upd_member_file(m.name, m.name_len, files, 3) == 1);

    hl = upd_lha_base(arc + n1, at - n1, &m);
    CHECK(hl > 0);
    CHECK(upd_lha_ext(arc + n1 + hl, m.skip - m.size, &m) == 1);
    CHECK(strcmp(m.name, "Devs/Internet/certificates") == 0);
    CHECK(upd_member_file(m.name, m.name_len, files, 3) == 2);
    CHECK(upd_lha_base(arc + at - 1, 1, &m) == 0);     /* the end         */

    /* A class prefix that is not the file's, a name not in the manifest. */
    CHECK(upd_member_file("Libs/ping", 9, files, 3) == -1);
    CHECK(upd_member_file("Devs/Networks/certificates", 26, files, 3) == -1);
    CHECK(upd_member_file("C/pong", 6, files, 3) == -1);
    CHECK(upd_member_file("c/PING", 6, files, 3) == 1);

    /* A bad checksum, a compressed method, a bad extended chain. */
    arc[1] ^= 1;
    CHECK(upd_lha_base(arc, at, &m) == -1);
    arc[1] ^= 1;
    arc[2 + 3] = '5';
    arc[1] = (unsigned char)(arc[1] + ('5' - '0'));
    CHECK(upd_lha_base(arc, at, &m) == -1);
    arc[2 + 3] = '0';
    arc[1] = (unsigned char)(arc[1] - ('5' - '0'));
    hl = upd_lha_base(arc, at, &m);
    CHECK(hl > 0);
    m.ext_first = 200;
    CHECK(upd_lha_ext(arc + hl, m.skip - m.size, &m) == 0);

    CHECK(upd_blocks(0, 512) == 1);      /* the header alone        */
    CHECK(upd_blocks(488, 512) == 3);
    CHECK(upd_blocks(489, 512) == 4);
    CHECK(upd_blocks(488UL * 72, 512) == 72 + 1 + 1);
    CHECK(upd_blocks(488UL * 73, 512) == 73 + 2 + 1);
}

/* ------------------------------------------------------------- journal --- */

static void test_journal_parse(void)
{
    static const char good[] =
        "format=1\nversion=1.0.0-rc5.1\nfrom=1.0.0-rc5\nflavour=full\n"
        "file=ours SYS:C/ping\nfile=otherbuild Work:Libs/x.library\n"
        "ready\naside 2\ndone 1\ndon";
    UpdJournal j;
    UpdJEntry  e[4];
    char       bad[512];

    CHECK(upd_journal_parse(good, sizeof(good) - 1, &j, e, 4) == 1);
    CHECK(j.ready == 1 && j.nfiles == 2 && j.flavour == UPD_FLAVOUR_FULL);
    CHECK(strcmp(j.version.pre, "rc5.1") == 0 && strcmp(j.from.pre, "rc5") == 0);
    CHECK(e[0].len == 10 && memcmp(e[0].path, "SYS:C/ping", 10) == 0);
    CHECK(e[0].why == UPD_WHY_OURS && e[1].why == UPD_WHY_OTHERBUILD);
    CHECK(e[0].done == 1 && e[0].aside == 0 && e[1].aside == 1 && e[1].done == 0);

    /* Not ready: cut before the "ready" line was whole. */
    CHECK(upd_journal_parse(good, 101, &j, e, 4) == 1 && j.ready == 0);

    CHECK(upd_journal_parse(good, sizeof(good) - 1, &j, e, 1) == 0);
    CHECK(upd_journal_parse("format=2\n", 9, &j, e, 4) == 0);
    CHECK(upd_journal_parse("", 0, &j, e, 4) == 0);
    snprintf(bad, sizeof(bad), "format=1\nversion=1.0.0\nready\n");
    CHECK(upd_journal_parse(bad, strlen(bad), &j, e, 4) == 0);
    snprintf(bad, sizeof(bad), "format=1\nversion=1.0.0\nfrom=0.9.0\nflavour=full\n"
             "file=ours C:a\nready\ndone 2\n");
    CHECK(upd_journal_parse(bad, strlen(bad), &j, e, 4) == 0);
    snprintf(bad, sizeof(bad), "format=1\nversion=1.0.0\nfrom=0.9.0\nflavour=full\n"
             "file=ours C:a\nready\nfile=ours C:b\n");
    CHECK(upd_journal_parse(bad, strlen(bad), &j, e, 4) == 0);
    snprintf(bad, sizeof(bad), "format=1\nversion=1.0.0\nfrom=0.9.0\nflavour=full\n"
             "file=mine C:a\n");
    CHECK(upd_journal_parse(bad, strlen(bad), &j, e, 4) == 0);
    snprintf(bad, sizeof(bad), "format=1\nversion=1.0.0\nfrom=0.9.0\nflavour=full\n"
             "done 1\n");
    CHECK(upd_journal_parse(bad, strlen(bad), &j, e, 4) == 0);
    snprintf(bad, sizeof(bad), "format=1\nversion=1.0.0\nversion=1.0.0\n");
    CHECK(upd_journal_parse(bad, strlen(bad), &j, e, 4) == 0);
}

static void test_state(void)
{
    static const char old[] =
        "phase=confirmed\nlast_check=2026-11-17\nversion=1.0.0\nskip=1.0.1\n\nboots=3";
    char          out[256];
    unsigned long n;

    n = upd_state_merge(old, sizeof(old) - 1, "trial", "1.0.2", 0, "full",
                        out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(strcmp(out, "phase=trial\nversion=1.0.2\nboots=0\nflavour=full\n"
                      "last_check=2026-11-17\nskip=1.0.1\n") == 0);
    n = upd_state_merge("", 0, "trial", "1.0.2", 12, "fat", out, sizeof(out));
    CHECK(n > 0 && strcmp(out, "phase=trial\nversion=1.0.2\nboots=12\nflavour=fat\n") == 0);
    CHECK(upd_state_merge(old, sizeof(old) - 1, "trial", "1.0.2", 0, "full",
                          out, 40) == 0);
}

/* ------------------------------------------- the swap on a test volume --- */

/*
 * A volume of named files with an integer for content.  "B:" names are a
 * second volume, which no rename crosses.  Every operation spends one of
 * `budget`; with none left it fails and does nothing -- the power is off --
 * except that a copy leaves its target half written and a journal line is
 * left without its LF, as a real cut does.
 */
#define FS_MAX 64
typedef struct Fs
{
    char  name[FS_MAX][UPD_APATH_MAX + 16];
    int   val[FS_MAX];
    int   used[FS_MAX];
    char  journal[4096];
    long  budget;           /* -1: unlimited                              */
    unsigned int cuts;
    unsigned int stop_cut;  /* 0: none                                    */
} Fs;

static int fs_find(Fs *fs, const char *p)
{
    int i;

    for (i = 0; i < FS_MAX; i++)
        if (fs->used[i] && strcmp(fs->name[i], p) == 0)
            return i;
    return -1;
}

static int fs_spend(Fs *fs)
{
    if (fs->budget < 0)
        return 1;
    if (fs->budget == 0)
        return 0;
    fs->budget--;
    return 1;
}

static void fs_put(Fs *fs, const char *p, int v)
{
    int i = fs_find(fs, p);

    if (i < 0)
        for (i = 0; i < FS_MAX && fs->used[i]; i++)
            ;
    snprintf(fs->name[i], sizeof(fs->name[i]), "%s", p);
    fs->val[i] = v;
    fs->used[i] = 1;
}

static int fs_get(Fs *fs, const char *p)
{
    int i = fs_find(fs, p);

    return i < 0 ? -1 : fs->val[i];
}

static int vol_b(const char *p) { return p[0] == 'B' && p[1] == ':'; }

static int op_exists(void *c, const char *p)
{
    Fs *fs = c;

    return fs_find(fs, p) >= 0;
}

static int op_rename(void *c, const char *a, const char *b)
{
    Fs *fs = c;
    int i;

    if (!fs_spend(fs))
        return 0;
    i = fs_find(fs, a);
    if (i < 0 || fs_find(fs, b) >= 0 || vol_b(a) != vol_b(b))
        return 0;
    snprintf(fs->name[i], sizeof(fs->name[i]), "%s", b);
    return 1;
}

static int op_remove(void *c, const char *p)
{
    Fs *fs = c;
    int i;

    if (!fs_spend(fs))
        return 0;
    i = fs_find(fs, p);
    if (i >= 0)
        fs->used[i] = 0;
    return 1;
}

static int op_copy(void *c, const char *a, const char *b)
{
    Fs *fs = c;
    int v = fs_get(fs, a);

    if (!fs_spend(fs))
    {
        if (v >= 0)
            fs_put(fs, b, -7);          /* half a file                    */
        return 0;
    }
    if (v < 0)
        return 0;
    fs_put(fs, b, v);
    return 1;
}

static int op_journal(void *c, const char *line)
{
    Fs     *fs = c;
    size_t  n = strlen(fs->journal);

    if (!fs_spend(fs))
    {
        snprintf(fs->journal + n, sizeof(fs->journal) - n, "%.3s", line);
        return 0;
    }
    snprintf(fs->journal + n, sizeof(fs->journal) - n, "%s\n", line);
    return 1;
}

static int op_cut(void *c)
{
    Fs *fs = c;

    fs->cuts++;
    return !(fs->stop_cut != 0 && fs->cuts == fs->stop_cut);
}

/* ctx is set by fs_setup(): one volume at a time. */
static UpdFsOps fs_ops = {
    0, op_exists, op_rename, op_remove, op_copy, op_journal, op_cut
};

/* Files: live value 10+i, .new 20+i, a foreign .old 30+i for the ones
   flagged.  The prior-old drawer is on volume A ("U:") or B. */
static const char *const tf_paths[] = {
    "SYS:C/ping", "SYS:Libs/bsdsocket.library", "B:Devs/Networks/anxnet.device",
    "SYS:C/fetch"
};
static const int tf_old[] = { 0, 1, 1, 0 };
#define TF_N 4

static void fs_setup(Fs *fs)
{
    char p[UPD_APATH_MAX + 8];
    int  i;

    memset(fs, 0, sizeof(*fs));
    fs_ops.ctx = fs;
    fs->budget = -1;
    strcpy(fs->journal,
           "format=1\nversion=1.0.0-rc5.1\nfrom=1.0.0-rc5\nflavour=full\n");
    for (i = 0; i < TF_N; i++)
    {
        size_t n = strlen(fs->journal);

        fs_put(fs, tf_paths[i], 10 + i);
        snprintf(p, sizeof(p), "%s.new", tf_paths[i]);
        fs_put(fs, p, 20 + i);
        if (tf_old[i])
        {
            snprintf(p, sizeof(p), "%s.old", tf_paths[i]);
            fs_put(fs, p, 30 + i);
        }
        snprintf(fs->journal + n, sizeof(fs->journal) - n, "file=%s %s\n",
                 i == 1 ? "otherbuild" : "ours", tf_paths[i]);
    }
}

/* All new, the old ones at .old, the foreign ones aside, nothing stray. */
static int fs_all_new(Fs *fs, const char *prior)
{
    char p[UPD_APATH_MAX + 16];
    int  i, ok = 1, k;

    for (i = 0; i < TF_N; i++)
    {
        ok &= fs_get(fs, tf_paths[i]) == 20 + i;
        snprintf(p, sizeof(p), "%s.old", tf_paths[i]);
        ok &= fs_get(fs, p) == 10 + i;
        snprintf(p, sizeof(p), "%s.new", tf_paths[i]);
        ok &= fs_get(fs, p) == -1;
        snprintf(p, sizeof(p), "%s/%d", prior, i + 1);
        ok &= fs_get(fs, p) == (tf_old[i] ? 30 + i : -1);
        snprintf(p, sizeof(p), "%s/%d.tmp", prior, i + 1);
        ok &= fs_get(fs, p) == -1;
    }
    for (k = 0; k < FS_MAX; k++)
        if (fs->used[k] && fs->val[k] == -7)
            ok = 0;
    return ok;
}

static int fs_all_old(Fs *fs)
{
    char p[UPD_APATH_MAX + 16];
    int  i, ok = 1;

    for (i = 0; i < TF_N; i++)
    {
        ok &= fs_get(fs, tf_paths[i]) == 10 + i;
        snprintf(p, sizeof(p), "%s.new", tf_paths[i]);
        ok &= fs_get(fs, p) == -1;
        snprintf(p, sizeof(p), "%s.old", tf_paths[i]);
        ok &= fs_get(fs, p) == (tf_old[i] ? 30 + i : -1);
    }
    return ok;
}

static void fs_trim(Fs *fs)
{
    char *lf = strrchr(fs->journal, '\n');

    if (lf != 0)
        lf[1] = '\0';
}

static int recover(Fs *fs, const char *prior)
{
    UpdJournal   j;
    UpdJEntry    e[TF_N];
    static char  copy[4096];
    unsigned int i;

    fs->budget = -1;
    fs->stop_cut = 0;
    strcpy(copy, fs->journal);
    if (!upd_journal_parse(copy, strlen(copy), &j, e, TF_N))
        return 0;
    if (!j.ready)
    {
        /* Undo: every .new goes, nothing else changed. */
        for (i = 0; i < j.nfiles; i++)
        {
            char p[UPD_APATH_MAX + 8];

            snprintf(p, sizeof(p), "%.*s.new", (int)e[i].len, e[i].path);
            op_remove(fs, p);
        }
        return 2;
    }
    /* A journal line cut short has no LF: the recovery cuts it off
       (SetFileSize to the last LF) before it appends. */
    fs_trim(fs);
    return upd_forward(e, j.nfiles, prior, &fs_ops, 0) == 1;
}

static void test_swap(const char *prior)
{
    static Fs    fs;
    UpdJournal   j;
    UpdJEntry    e[TF_N];
    static char  copy[4096];
    unsigned int full_ops, k, olds = 0, i;
    long         used;

    for (i = 0; i < TF_N; i++)
        olds += (unsigned int)tf_old[i];

    /* The whole swap, uncut: every cut point counted. */
    fs_setup(&fs);
    strcat(fs.journal, "ready\n");
    strcpy(copy, fs.journal);
    CHECK(upd_journal_parse(copy, strlen(copy), &j, e, TF_N) == 1 && j.ready);
    fs.budget = 100000;
    CHECK(upd_forward(e, j.nfiles, prior, &fs_ops, 0) == 1);
    used = 100000 - fs.budget;
    CHECK(fs_all_new(&fs, prior));
    CHECK(fs.cuts == upd_forward_steps(TF_N, olds));
    full_ops = (unsigned int)used;

    /* Stopped after every operation, then finished from the journal. */
    for (k = 0; k <= full_ops; k++)
    {
        fs_setup(&fs);
        strcat(fs.journal, "ready\n");
        strcpy(copy, fs.journal);
        upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
        fs.budget = (long)k;
        (void)upd_forward(e, j.nfiles, prior, &fs_ops, 0);
        CHECK(recover(&fs, prior) == 1);
        if (!fs_all_new(&fs, prior))
        {
            printf("  swap cut after %u operations (prior %s): not all new\n",
                   k, prior);
            failures++;
        }
        /* And cut again during the recovery itself. */
        if (k > 0)
        {
            fs_setup(&fs);
            strcat(fs.journal, "ready\n");
            strcpy(copy, fs.journal);
            upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
            fs.budget = (long)k;
            (void)upd_forward(e, j.nfiles, prior, &fs_ops, 0);
            {
                UpdJournal j2;
                UpdJEntry  e2[TF_N];
                static char c2[4096];

                fs_trim(&fs);
                strcpy(c2, fs.journal);
                upd_journal_parse(c2, strlen(c2), &j2, e2, TF_N);
                fs.budget = (long)(k / 2);
                (void)upd_forward(e2, j2.nfiles, prior, &fs_ops, 0);
            }
            CHECK(recover(&fs, prior) == 1 && fs_all_new(&fs, prior));
        }
    }

    /* Each cut point stops it, and the journal finishes it. */
    for (k = 1; k <= upd_forward_steps(TF_N, olds); k++)
    {
        fs_setup(&fs);
        strcat(fs.journal, "ready\n");
        strcpy(copy, fs.journal);
        upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
        fs.stop_cut = k;
        CHECK(upd_forward(e, j.nfiles, prior, &fs_ops, 0) == 0);
        CHECK(recover(&fs, prior) == 1 && fs_all_new(&fs, prior));
    }

    /* Without "ready": undone, all old. */
    fs_setup(&fs);
    CHECK(recover(&fs, prior) == 2 && fs_all_old(&fs));

    /* A file whose live and .new are both gone. */
    fs_setup(&fs);
    strcat(fs.journal, "ready\n");
    strcpy(copy, fs.journal);
    upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
    op_remove(&fs, "SYS:C/ping");
    op_remove(&fs, "SYS:C/ping.new");
    CHECK(upd_forward(e, j.nfiles, prior, &fs_ops, 0) == -1);
}

/* ------------------------------------------- the trial: commit, rollback --- */

/* No half file, no .tmp, nothing left in the prior drawer. */
static int fs_clean(Fs *fs, const char *prior)
{
    char p[UPD_APATH_MAX + 16];
    int  i, k, ok = 1;

    for (i = 0; i < TF_N; i++)
    {
        snprintf(p, sizeof(p), "%s/%d", prior, i + 1);
        ok &= fs_get(fs, p) == -1;
        snprintf(p, sizeof(p), "%s/%d.tmp", prior, i + 1);
        ok &= fs_get(fs, p) == -1;
        snprintf(p, sizeof(p), "%s.tmp", tf_paths[i]);
        ok &= fs_get(fs, p) == -1;
        snprintf(p, sizeof(p), "%s.new", tf_paths[i]);
        ok &= fs_get(fs, p) == -1;
    }
    for (k = 0; k < FS_MAX; k++)
        if (fs->used[k] && fs->val[k] == -7)
            ok = 0;
    return ok;
}

/* Committed: every file new; ours' .old gone, or the prior one back; the
   other build (file 1) at .otherbuild with its prior .old back. */
static int fs_committed(Fs *fs, const char *prior)
{
    char p[UPD_APATH_MAX + 16];
    int  i, ok = fs_clean(fs, prior);

    for (i = 0; i < TF_N; i++)
    {
        ok &= fs_get(fs, tf_paths[i]) == 20 + i;
        snprintf(p, sizeof(p), "%s.old", tf_paths[i]);
        ok &= fs_get(fs, p) == (tf_old[i] ? 30 + i : -1);
        snprintf(p, sizeof(p), "%s.otherbuild", tf_paths[i]);
        ok &= fs_get(fs, p) == (i == 1 ? 11 : -1);
    }
    return ok;
}

static int fs_rolledback(Fs *fs, const char *prior)
{
    char p[UPD_APATH_MAX + 16];
    int  i, ok = fs_clean(fs, prior) && fs_all_old(fs);

    for (i = 0; i < TF_N; i++)
    {
        snprintf(p, sizeof(p), "%s.otherbuild", tf_paths[i]);
        ok &= fs_get(fs, p) == -1;
    }
    return ok;
}

/* The state INSTALL leaves: the swap done, its journal the trial. */
static void trial_setup(Fs *fs, const char *prior)
{
    UpdJournal  j;
    UpdJEntry   e[TF_N];
    static char copy[4096];

    fs_setup(fs);
    strcat(fs->journal, "ready\n");
    strcpy(copy, fs->journal);
    upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
    CHECK(upd_forward(e, j.nfiles, prior, &fs_ops, 0) == 1 && fs_all_new(fs, prior));
}

/* From the trial file as the disk has it, the way BOOT runs it: a line
   cut short cut off, then commit (1) or rollback (0) to the end. */
static int trial_run(Fs *fs, const char *prior, int commit, long budget,
                     unsigned int stop)
{
    UpdJournal  j;
    UpdJEntry   e[TF_N];
    static char copy[4096];

    fs_trim(fs);
    strcpy(copy, fs->journal);
    if (!upd_journal_parse(copy, strlen(copy), &j, e, TF_N) || !j.ready)
        return -2;
    fs->budget = budget;
    fs->stop_cut = stop;
    fs->cuts = 0;
    return commit ? upd_commit(e, j.nfiles, prior, &fs_ops, 0)
                  : upd_rollback(e, j.nfiles, prior, &fs_ops, 0);
}

static void test_trial(const char *prior, int commit)
{
    static Fs    fs;
    unsigned int k, full_ops, full_cuts;
    long         left;

    trial_setup(&fs, prior);
    CHECK(trial_run(&fs, prior, commit, 100000, 0) == 1);
    left = fs.budget;
    full_cuts = fs.cuts;
    CHECK(commit ? fs_committed(&fs, prior) : fs_rolledback(&fs, prior));
    full_ops = (unsigned int)(100000 - left);
    /* Run again over a finished one: nothing to do, nothing changed. */
    CHECK(trial_run(&fs, prior, commit, -1, 0) == 1);
    CHECK(commit ? fs_committed(&fs, prior) : fs_rolledback(&fs, prior));

    /* Stopped after every operation, finished by the next boot; and that
       boot cut again half way. */
    for (k = 0; k <= full_ops; k++)
    {
        int ok;

        trial_setup(&fs, prior);
        (void)trial_run(&fs, prior, commit, (long)k, 0);
        CHECK(trial_run(&fs, prior, commit, -1, 0) == 1);
        ok = commit ? fs_committed(&fs, prior) : fs_rolledback(&fs, prior);
        if (!ok)
        {
            printf("  %s cut after %u operations (prior %s): wrong end\n",
                   commit ? "commit" : "rollback", k, prior);
            failures++;
        }
        trial_setup(&fs, prior);
        (void)trial_run(&fs, prior, commit, (long)k, 0);
        (void)trial_run(&fs, prior, commit, (long)(k / 2), 0);
        CHECK(trial_run(&fs, prior, commit, -1, 0) == 1);
        CHECK(commit ? fs_committed(&fs, prior) : fs_rolledback(&fs, prior));
    }

    /* Each cut point stops it, and the next run finishes it. */
    for (k = 1; k <= full_cuts; k++)
    {
        trial_setup(&fs, prior);
        CHECK(trial_run(&fs, prior, commit, -1, k) == 0);
        CHECK(trial_run(&fs, prior, commit, -1, 0) == 1);
        CHECK(commit ? fs_committed(&fs, prior) : fs_rolledback(&fs, prior));
    }
}

static void test_trial_edges(void)
{
    static Fs    fs;
    UpdJournal   j;
    UpdJEntry    e[TF_N];
    static char  copy[4096];
    const char  *prior = "U:Installed/Update/prior-old";

    /* .otherbuild taken: the other build stays at .old, its prior .old in
       the prior drawer; nothing is lost. */
    trial_setup(&fs, prior);
    fs_put(&fs, "SYS:Libs/bsdsocket.library.otherbuild", 99);
    fs_trim(&fs);
    strcpy(copy, fs.journal);
    upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
    CHECK(upd_commit(e, j.nfiles, prior, &fs_ops, 0) == 1);
    CHECK(upd_commit_stays(&e[1]) && !upd_commit_stays(&e[0]) &&
          !upd_commit_stays(&e[2]));
    CHECK(fs_get(&fs, "SYS:Libs/bsdsocket.library.old") == 11);
    CHECK(fs_get(&fs, "U:Installed/Update/prior-old/2") == 31);
    CHECK(fs_get(&fs, "SYS:Libs/bsdsocket.library.otherbuild") == 99);
    CHECK(fs_get(&fs, "B:Devs/Networks/anxnet.device.old") == 32);
    /* The journal says so: run again, still both there. */
    fs_trim(&fs);
    strcpy(copy, fs.journal);
    upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
    CHECK(e[1].kept && !e[1].other && upd_commit_stays(&e[1]));
    CHECK(upd_commit(e, j.nfiles, prior, &fs_ops, 0) == 1);
    CHECK(fs_get(&fs, "U:Installed/Update/prior-old/2") == 31);

    /* A rollback whose .old and live file are both gone. */
    trial_setup(&fs, prior);
    op_remove(&fs, "SYS:C/fetch");
    op_remove(&fs, "SYS:C/fetch.old");
    fs_trim(&fs);
    strcpy(copy, fs.journal);
    upd_journal_parse(copy, strlen(copy), &j, e, TF_N);
    CHECK(upd_rollback(e, j.nfiles, prior, &fs_ops, 0) == -1);

    /* The trial's own lines. */
    {
        static const char t[] =
            "format=1\nversion=1.0.1\nfrom=1.0.0\nflavour=micro\n"
            "file=ours C:a\nfile=otherbuild C:b\nready\ndone 1\ndone 2\n"
            "back 2\nother 2\nkept 1\nkept";
        CHECK(upd_journal_parse(t, sizeof(t) - 1, &j, e, TF_N) == 1);
        CHECK(e[1].back && !e[0].back && e[1].other && e[0].kept && !e[1].kept);
        CHECK(upd_journal_trim(t, sizeof(t) - 1) == sizeof(t) - 1 - 4);
        CHECK(upd_journal_trim("x", 1) == 0 && upd_journal_trim("x\n", 2) == 2);
        static const char k2[] = "format=1\nversion=1.0.1\nfrom=1.0.0\nflavour=full\n"
                                 "file=ours C:a\nready\nkept 2\n";

        CHECK(upd_journal_parse(k2, sizeof(k2) - 1, &j, e, TF_N) == 0);
    }
}

static void test_state_get(void)
{
    static const char st[] = "phase=trial\r\nVersion=1.0.0-rc5.1\nboots=2\nskip=";
    char v[16];

    CHECK(upd_state_get(st, sizeof(st) - 1, "phase", v, sizeof(v)) && strcmp(v, "trial") == 0);
    CHECK(upd_state_get(st, sizeof(st) - 1, "version", v, sizeof(v)) &&
          strcmp(v, "1.0.0-rc5.1") == 0);
    CHECK(upd_state_get(st, sizeof(st) - 1, "boots", v, sizeof(v)) && strcmp(v, "2") == 0);
    CHECK(upd_state_get(st, sizeof(st) - 1, "skip", v, sizeof(v)) && v[0] == '\0');
    CHECK(!upd_state_get(st, sizeof(st) - 1, "phas", v, sizeof(v)));
    CHECK(!upd_state_get(st, sizeof(st) - 1, "flavour", v, sizeof(v)));
    CHECK(!upd_state_get(st, sizeof(st) - 1, "version", v, 5));
}

static void test_record(void)
{
    static const char rec[] =
        "; S:AmiNetXDuo-Files -- written by the AmiNetXDuo installer.\n"
        "FailAt 21\nDelete \"C:ping\" QUIET\n";
    static const char sec1[] = UPD_REC_BEGIN "\n; 1.0.1\n" UPD_REC_END "\n";
    static const char sec2[] = UPD_REC_BEGIN "\n; 1.0.2\nDelete x\n" UPD_REC_END "\n";
    char          a[512], b[512];
    unsigned long n, m;

    n = upd_record_merge(rec, sizeof(rec) - 1, sec1, sizeof(sec1) - 1, a, sizeof(a));
    CHECK(n == strlen(a) && n == sizeof(rec) - 1 + sizeof(sec1) - 1);
    CHECK(strstr(a, "FailAt 21\n" UPD_REC_BEGIN) != 0);
    CHECK(strstr(a, UPD_REC_END "\nDelete \"C:ping\" QUIET\n") != 0);
    /* Again: the block replaced, not added. */
    m = upd_record_merge(a, n, sec2, sizeof(sec2) - 1, b, sizeof(b));
    CHECK(m == sizeof(rec) - 1 + sizeof(sec2) - 1);
    CHECK(strstr(b, "1.0.1") == 0 && strstr(b, "FailAt 21\n" UPD_REC_BEGIN "\n; 1.0.2") != 0);
    CHECK(strstr(b, "Delete \"C:ping\"") != 0);
    /* No FailAt line: first.  Too small: 0. */
    n = upd_record_merge("Delete y\n", 9, sec1, sizeof(sec1) - 1, a, sizeof(a));
    CHECK(n > 0 && strncmp(a, UPD_REC_BEGIN, strlen(UPD_REC_BEGIN)) == 0);
    CHECK(upd_record_merge(rec, sizeof(rec) - 1, sec1, sizeof(sec1) - 1, a, 40) == 0);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";

    if (strcmp(which, "select") == 0 || strcmp(which, "all") == 0)
    {
        test_receipt();
        test_scan();
        test_select();
        test_container();
    }
    if (strcmp(which, "journal") == 0 || strcmp(which, "all") == 0)
    {
        test_journal_parse();
        test_state();
        test_swap("U:Installed/Update/prior-old");   /* the volume of most */
        test_swap("B:Installed/Update/prior-old");   /* another: copies    */
    }
    if (strcmp(which, "trial") == 0 || strcmp(which, "all") == 0)
    {
        test_state_get();
        test_record();
        test_trial("U:Installed/Update/prior-old", 1);
        test_trial("B:Installed/Update/prior-old", 1);
        test_trial("U:Installed/Update/prior-old", 0);
        test_trial("B:Installed/Update/prior-old", 0);
        test_trial_edges();
    }

    printf("test_updinstall %s: %d checks, %d failures\n", which, checks, failures);
    return failures != 0;
}
