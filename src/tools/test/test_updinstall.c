/*
 * The tests for src/tools/updinstall.c, the half of `UpdateAmiNetXDuo
 * INSTALL` that is not I/O.
 *
 *   test_updinstall select    the receipt, the destinations, the mark and
 *                             $VER scan, the selection rule, the container
 *                             member headers, the block count, the
 *                             container fetch's verdict
 *   test_updinstall journal   the journal parse, the state merge, the
 *                             trial INSTALL refuses, the paths PREPARE
 *                             refuses, the journal the cleanup refuses,
 *                             the .new files PREPARE owns, the drawer
 *                             lock's refusals, the 30-character names, a
 *                             stale prior-old/<n>, and the
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

/*
 * The container fetch.  fetchcore opens the sink on a final 2xx only, but
 * still hands it a 404's body: as the sink sees it, a chunk before any
 * open.  That body is neither written nor counted, and the answer is
 * refused=network -- not space, as when the sink called it a write that
 * failed (#497 review).
 */
static void test_download(void)
{
    unsigned long total = 0;
    int           wfail = 0, toolong = 0, r;

    /* A 404 with "Not Found": no open, one chunk, the fetch fails. */
    r = upd_dl_chunk(0, total, 9, 1000);
    CHECK(r == UPD_DL_NETWORK);
    CHECK(upd_dl_verdict(404, 0, wfail, toolong, total, 1000) == UPD_DL_NETWORK);
    /* Its status decides even where a flag is set. */
    CHECK(upd_dl_verdict(404, 1, 1, 0, 0, 1000) == UPD_DL_NETWORK);
    CHECK(upd_dl_verdict(500, 0, 0, 1, 0, 1000) == UPD_DL_NETWORK);
    CHECK(upd_dl_verdict(206, 1, 0, 0, 1000, 1000) == UPD_DL_NETWORK);
    CHECK(upd_dl_verdict(0, 0, 0, 0, 0, 1000) == UPD_DL_NETWORK);

    /* A 200: written up to the manifest's size, and no further. */
    CHECK(upd_dl_chunk(1, 0, 1000, 1000) == UPD_DL_OK);
    CHECK(upd_dl_chunk(1, 999, 1, 1000) == UPD_DL_OK);
    CHECK(upd_dl_chunk(1, 1, 1000, 1000) == UPD_DL_LONG);
    CHECK(upd_dl_verdict(200, 0, 1, 0, 500, 1000) == UPD_DL_SPACE);
    CHECK(upd_dl_verdict(200, 0, 0, 1, 1000, 1000) == UPD_DL_LONG);
    CHECK(upd_dl_verdict(200, 1, 0, 0, 999, 1000) == UPD_DL_NETWORK);
    CHECK(upd_dl_verdict(200, 0, 0, 0, 1000, 1000) == UPD_DL_NETWORK);
    CHECK(upd_dl_verdict(200, 1, 0, 0, 1000, 1000) == UPD_DL_OK);
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

/*
 * After a SWAP the journal is the trial file and the state says
 * phase=trial; CONFIRM needs receipt-next.  A second INSTALL refuses
 * before its cleanup removes that (#497 review).
 */
static void test_trial(void)
{
    static const char kept[] =
        "last_check=2026-11-17\nphase=trial\nversion=1.0.2\n";
    char          out[256];
    unsigned long n;

    CHECK(upd_trial_active(1, 0, 0) == 1);
    CHECK(upd_trial_active(1, "phase=confirmed\n", 16) == 1);
    CHECK(upd_trial_active(0, 0, 0) == 0);
    CHECK(upd_trial_active(0, "", 0) == 0);
    CHECK(upd_trial_active(0, kept, sizeof(kept) - 1) == 1);
    CHECK(upd_trial_active(0, "PHASE=Trial", 11) == 1);
    CHECK(upd_trial_active(0, "phase=confirmed\n", 16) == 0);
    CHECK(upd_trial_active(0, "phase=trialx\nxphase=trial\n", 26) == 0);
    CHECK(upd_trial_active(0, kept, 20) == 0);

    /* The state SWAP writes is a trial; once confirmed it is not. */
    n = upd_state_merge("", 0, "trial", "1.0.2", 0, "full", out, sizeof(out));
    CHECK(n > 0 && upd_trial_active(0, out, n) == 1);
    n = upd_state_merge(kept, sizeof(kept) - 1, "confirmed", "1.0.2", 0, "full",
                        out, sizeof(out));
    CHECK(n > 0 && upd_trial_active(0, out, n) == 0);
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
    const char  *err;       /* exists() of this name fails: a DOS error   */
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

    if (fs->err != 0 && strcmp(fs->err, p) == 0)
        return -1;

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

static void test_trial_cut(const char *prior, int commit)
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

/* A look that cannot be answered, at each name of each entry: the run
   stops (or never looks there), and the next run finishes it right. */
static void test_trial_errors(const char *prior, int commit)
{
    static Fs   fs;
    static char names[TF_N * 5][UPD_APATH_MAX + 32];
    unsigned int i, k, nn = 0;

    for (i = 0; i < TF_N; i++)
    {
        snprintf(names[nn++], sizeof(names[0]), "%s", tf_paths[i]);
        snprintf(names[nn++], sizeof(names[0]), "%s.old", tf_paths[i]);
        snprintf(names[nn++], sizeof(names[0]), "%s.otherbuild", tf_paths[i]);
        snprintf(names[nn++], sizeof(names[0]), "%s/%u", prior, i + 1);
        snprintf(names[nn++], sizeof(names[0]), "%s.tmp", tf_paths[i]);
    }
    for (k = 0; k < nn; k++)
    {
        int r;

        trial_setup(&fs, prior);
        fs.err = names[k];
        r = trial_run(&fs, prior, commit, -1, 0);
        fs.err = 0;
        CHECK(r == 0 || r == 1);
        CHECK(trial_run(&fs, prior, commit, -1, 0) == 1);
        if (!(commit ? fs_committed(&fs, prior) : fs_rolledback(&fs, prior)))
        {
            printf("  %s, %s unanswered (prior %s): wrong end\n",
                   commit ? "commit" : "rollback", names[k], prior);
            failures++;
        }
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

/*
 * What upd_swap_fits() takes, the journal reads back and the swap names;
 * what it refuses, one of them cannot hold.  An assign resolves to a
 * physical path that can be longer than the one the receipt names: PREPARE
 * asks before it stages anything (#497 review: 173 bytes passed, then the
 * swap failed and its own journal did not parse).
 */
static int fits_swaps(const char *path, unsigned int i, const char *prior,
                      int old)
{
    static Fs   fs;
    char        p[UPD_APATH_MAX + 32];
    UpdJEntry   e[2];

    memset(&fs, 0, sizeof(fs));
    memset(e, 0, sizeof(e));
    fs_ops.ctx = &fs;
    fs.budget = -1;
    /* Entry i is the one under test; the one before it is done. */
    e[0].path = "SYS:C/done";
    e[0].len  = 10;
    e[0].done = 1;
    e[i].path = path;
    e[i].len  = (unsigned short)strlen(path);
    e[i].done = 0;
    fs_put(&fs, path, 1);
    CHECK(upd_sidecar(p, sizeof(p), path, strlen(path), ".new") == 1);
    fs_put(&fs, p, 2);
    if (old)
    {
        CHECK(upd_sidecar(p, sizeof(p), path, strlen(path), ".old") == 1);
        fs_put(&fs, p, 3);
    }
    return upd_forward(e, i + 1, prior, &fs_ops, 0) == 1 &&
           fs_get(&fs, path) == 2;
}

static int fits_parses(const char *path)
{
    static char t[UPD_APATH_MAX + 128];
    UpdJournal  j;
    UpdJEntry   e[1];

    snprintf(t, sizeof(t), "format=1\nversion=1.0.1\nfrom=1.0.0\nflavour=full\n"
             "file=ours %s\nready\n", path);
    return upd_journal_parse(t, strlen(t), &j, e, 1) == 1 && j.nfiles == 1 &&
           e[0].len == strlen(path);
}

static void test_swap_fits(void)
{
    static char  path[UPD_APATH_MAX + 32], prior[UPD_APATH_MAX + 32];
    UpdJEntry    e;
    unsigned int len, i;
    int          fits;

    memset(&e, 0, sizeof(e));
    e.path = path;

    /* The live path: up to UPD_APATH_MAX - 1 bytes, as the journal. */
    for (len = 8; len <= UPD_APATH_MAX + 8; len++)
    {
        memset(path, 'a', len);
        memcpy(path, "DH0:", 4);
        path[len] = '\0';
        e.len = (unsigned short)len;
        fits = upd_swap_fits(&e, 0, "U:p");
        CHECK(fits == (len < UPD_APATH_MAX));
        CHECK(fits_swaps(path, 0, "U:p", 1) == fits);
        CHECK(fits_parses(path) == fits);
    }

    /* The review's: an assign resolved to 173 bytes. */
    strcpy(path, "DH0:");
    for (i = 0; i < 6; i++)
        strcat(path, "abcdefghijklmnopqrstuvwxyz/");
    strcat(path, "C/fetch");
    e.len = (unsigned short)strlen(path);
    CHECK(e.len == 173 && upd_swap_fits(&e, 0, "U:p") == 0);

    /* The prior-old drawer's names, <prior>/<n>.tmp, with n's digits. */
    strcpy(path, "SYS:C/ping");
    e.len = 10;
    for (len = UPD_APATH_MAX - 8; len <= UPD_APATH_MAX + 8; len++)
        for (i = 0; i < 2; i++)
        {
            memset(prior, 'p', len);
            memcpy(prior, "U:", 2);
            prior[len] = '\0';
            fits = upd_swap_fits(&e, i, prior);
            CHECK(fits == (len + 1 + 1 + 4 < UPD_APATH_MAX + 12));
            CHECK(fits_swaps(path, i, prior, 1) == fits);
            CHECK(fits_swaps(path, i, prior, 0) == fits);
        }
    memset(prior, 'p', UPD_APATH_MAX + 5);
    memcpy(prior, "U:", 2);
    prior[UPD_APATH_MAX + 5] = '\0';
    CHECK(upd_swap_fits(&e, 8, prior) == 1);    /* "/9.tmp"                */
    CHECK(upd_swap_fits(&e, 9, prior) == 0);    /* "/10.tmp"               */

    /* A byte the journal does not read back, and no path at all. */
    strcpy(path, "SYS:C/p\xe4ng");
    e.len = (unsigned short)strlen(path);
    CHECK(upd_swap_fits(&e, 0, "U:p") == 0 && fits_parses(path) == 0);
    strcpy(path, "SYS:C/my ping");
    e.len = (unsigned short)strlen(path);
    CHECK(upd_swap_fits(&e, 0, "U:p") == 1 && fits_parses(path) == 1);
    e.len = 0;
    CHECK(upd_swap_fits(&e, 0, "U:p") == 0);
}

/*
 * The journal INSTALL's cleanup finds.  Only one read whole and without
 * "ready" is undone, only one not there is ignored: a ready journal that
 * could not be read is refused, and receipt-next stays (#497 review).
 */
static void test_stale_journal(void)
{
    static const char unready[] =
        "format=1\nversion=1.0.1\nfrom=1.0.0\nflavour=full\nfile=ours C:fetch\n";
    static const char ready[] =
        "format=1\nversion=1.0.1\nfrom=1.0.0\nflavour=full\nfile=ours C:fetch\nready\n";
    UpdJournal j;
    UpdJEntry  e[2];

    CHECK(upd_stale_journal(0, 0, 1, &j, e, 2) == UPD_STALE_NONE);
    CHECK(upd_stale_journal(0, 0, 0, &j, e, 2) == UPD_STALE_PENDING);
    CHECK(upd_stale_journal(ready, sizeof(ready) - 1, 0, &j, e, 2) == UPD_STALE_PENDING);
    CHECK(upd_stale_journal("garbage\n", 8, 0, &j, e, 2) == UPD_STALE_PENDING);
    CHECK(upd_stale_journal("", 0, 0, &j, e, 2) == UPD_STALE_PENDING);
    CHECK(upd_stale_journal(unready, sizeof(unready) - 1, 0, &j, 0, 2) == UPD_STALE_PENDING);
    CHECK(upd_stale_journal(unready, sizeof(unready) - 1, 0, &j, e, 2) == UPD_STALE_UNDO &&
          j.nfiles == 1 && e[0].staged == 0);
    /* A cut in the middle of "ready": not there yet, undone. */
    CHECK(upd_stale_journal(ready, sizeof(ready) - 3, 0, &j, e, 2) == UPD_STALE_UNDO);
}

/*
 * The .new files: PREPARE refuses one it did not write, and a failure
 * removes only the ones it did -- never the user's C:fetch.new (#497
 * review).
 */
static void test_staging(void)
{
    static Fs  fs;
    UpdJEntry  e[3];
    unsigned int bad = 99;

    memset(&fs, 0, sizeof(fs));
    memset(e, 0, sizeof(e));
    fs_ops.ctx = &fs;
    fs.budget = -1;
    e[0].path = "SYS:C/fetch";             e[0].len = 11;
    e[1].path = "SYS:C/ping";              e[1].len = 10;
    e[2].path = "SYS:Libs/bsdsocket.library"; e[2].len = 26;

    CHECK(upd_new_free(e, 3, &fs_ops, &bad) == 1 && bad == 99);
    fs_put(&fs, "SYS:C/ping.new", 7);       /* the user's                  */
    CHECK(upd_new_free(e, 3, &fs_ops, &bad) == 0 && bad == 1);
    CHECK(upd_new_free(e, 1, &fs_ops, 0) == 1);

    /* A failure after two were staged: those go, the user's stays. */
    fs_put(&fs, "SYS:C/fetch.new", 1);
    fs_put(&fs, "SYS:Libs/bsdsocket.library.new", 2);
    e[0].staged = 1;
    e[2].staged = 1;
    upd_unstage_new(e, 3, &fs_ops);
    CHECK(fs_get(&fs, "SYS:C/fetch.new") == -1);
    CHECK(fs_get(&fs, "SYS:Libs/bsdsocket.library.new") == -1);
    CHECK(fs_get(&fs, "SYS:C/ping.new") == 7);
    CHECK(e[0].staged == 0 && e[1].staged == 0 && e[2].staged == 0);

    /* None staged (the download failed): nothing removed. */
    fs_put(&fs, "SYS:C/fetch.new", 3);
    upd_unstage_new(e, 3, &fs_ops);
    CHECK(fs_get(&fs, "SYS:C/fetch.new") == 3 && fs_get(&fs, "SYS:C/ping.new") == 7);
}

/* The drawer lock: a second run is refused busy, a read-only drawer is
   left to the write-protection refusal, anything else refused. */
static void test_lock_why(void)
{
    CHECK(upd_lock_why(202) == UPD_LOCK_BUSY);
    CHECK(upd_lock_why(214) == UPD_LOCK_WP);
    CHECK(upd_lock_why(223) == UPD_LOCK_WP);
    CHECK(upd_lock_why(0) == UPD_LOCK_OTHER);
    CHECK(upd_lock_why(205) == UPD_LOCK_OTHER);
    CHECK(upd_lock_why(221) == UPD_LOCK_OTHER);
}

/* The last component of `p`. */
static unsigned long comp_len(const char *p)
{
    const char *c = p, *s;

    for (s = p; *s != '\0'; s++)
        if (*s == '/' || *s == ':')
            c = s + 1;
    return strlen(c);
}

/*
 * Every name the updater makes fits 30 characters, the classic OFS/FFS
 * limit: the Update drawer's files, prior-old/<n> and <n>.tmp, and the
 * .new and .old beside a target, whatever the target's name (#497
 * review: AmiNetXDuo-1.0.0-rc4.1-full.update.lha is 38, and a cut name
 * escaped the cleanup).
 */
static void test_names(void)
{
    static const char *const drawer[] = {
        UPD_N_JOURNAL, UPD_N_JOURNAL_TMP, UPD_N_TRIAL, UPD_N_RECEIPT,
        UPD_N_RECEIPT_NEXT, UPD_N_STATE, UPD_N_STATE_TMP, UPD_N_PRIOR,
        UPD_N_LOCK, UPD_N_LOG, UPD_N_CONTAINER
    };
    static Fs    fs;
    char         p[UPD_APATH_MAX + 32], q[UPD_APATH_MAX + 32];
    UpdJEntry    e[3];
    unsigned int i, len, bad = 99;
    int          k;

    for (i = 0; i < sizeof(drawer) / sizeof(drawer[0]); i++)
        CHECK(strlen(drawer[i]) <= UPD_FNAME_MAX);
    {
        const char *name = UPD_N_CONTAINER;
        size_t      nl = strlen(name), el = strlen(UPD_N_CONTAINER_EXT);

        CHECK(nl > el && strcmp(name + nl - el, UPD_N_CONTAINER_EXT) == 0);
    }
    for (i = 1; i <= UPD_RCPT_MAX; i++)
    {
        snprintf(p, sizeof(p), "%u.tmp", i);
        CHECK(strlen(p) <= UPD_FNAME_MAX);
    }

    /* .new and .old beside a target of every length. */
    for (len = 1; len <= 40; len++)
    {
        snprintf(p, sizeof(p), "SYS:C/%.*s", (int)len,
                 "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN");
        CHECK(upd_sidecar(q, sizeof(q), p, strlen(p), ".new") == 1);
        CHECK(comp_len(q) <= UPD_FNAME_MAX && strcmp(q + strlen(q) - 4, ".new") == 0);
        CHECK(strncmp(q, p, 6) == 0);
        if (len + 4 <= UPD_FNAME_MAX)
            CHECK(strlen(q) == strlen(p) + 4 && strncmp(q, p, strlen(p)) == 0);
        CHECK(upd_sidecar(q, sizeof(q), p, strlen(p), ".old") == 1 &&
              comp_len(q) <= UPD_FNAME_MAX);
    }
    CHECK(upd_sidecar(q, sizeof(q), "C:CreateAmiNetXDuoStatusReport", 30, ".new") == 1 &&
          strcmp(q, "C:CreateAmiNetXDuoStatusRepo.new") == 0);
    CHECK(upd_sidecar(q, 8, "SYS:C/ping", 10, ".new") == 0);

    /* The swap of a 28-character target makes no longer name. */
    memset(&fs, 0, sizeof(fs));
    memset(e, 0, sizeof(e));
    fs_ops.ctx = &fs;
    fs.budget = -1;
    e[0].path = "SYS:C/CreateAmiNetXDuoStatusReport";
    e[0].len  = (unsigned short)strlen(e[0].path);
    fs_put(&fs, e[0].path, 1);
    fs_put(&fs, "SYS:C/CreateAmiNetXDuoStatusRepo.new", 2);
    fs_put(&fs, "SYS:C/CreateAmiNetXDuoStatusRepo.old", 3);
    CHECK(upd_forward(e, 1, "U:Update/" UPD_N_PRIOR, &fs_ops, 0) == 1);
    CHECK(fs_get(&fs, e[0].path) == 2 &&
          fs_get(&fs, "SYS:C/CreateAmiNetXDuoStatusRepo.old") == 1 &&
          fs_get(&fs, "U:Update/" UPD_N_PRIOR "/1") == 3);
    for (k = 0; k < FS_MAX; k++)
        if (fs.used[k])
            CHECK(comp_len(fs.name[k]) <= UPD_FNAME_MAX);

    /* Two long names in one drawer cut alike: refused; elsewhere not. */
    e[0].path = "SYS:C/abcdefghijklmnopqrstuvwxyzAB"; e[0].len = 34;
    e[1].path = "SYS:C/ping";                         e[1].len = 10;
    e[2].path = "SYS:C/abcdefghijklmnopqrstuvwxyzCD"; e[2].len = 34;
    CHECK(upd_sidecars_distinct(e, 2, &bad) == 1);
    CHECK(upd_sidecars_distinct(e, 3, &bad) == 0 && bad == 2);
    e[2].path = "SYS:L/abcdefghijklmnopqrstuvwxyzCD";
    CHECK(upd_sidecars_distinct(e, 3, 0) == 1);
}

/*
 * A prior-old/<n> already there before "aside <n>" is an earlier update's:
 * the swap stops and the user's .old stays (#497 review: it was deleted
 * on the strength of a stale prior-old/<n>).  After "aside <n>" it is
 * this swap's copy, and the .old goes.
 */
static void test_stale_prior(void)
{
    static Fs  fs;
    UpdJEntry  e[1];
    const char *prior = "U:Update/prior-old";

    memset(&fs, 0, sizeof(fs));
    memset(e, 0, sizeof(e));
    fs_ops.ctx = &fs;
    fs.budget = -1;
    e[0].path = "SYS:C/ping";
    e[0].len  = 10;
    fs_put(&fs, "SYS:C/ping", 1);
    fs_put(&fs, "SYS:C/ping.new", 2);
    fs_put(&fs, "SYS:C/ping.old", 3);
    fs_put(&fs, "U:Update/prior-old/1", 9);
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 0);
    CHECK(fs_get(&fs, "SYS:C/ping.old") == 3 && fs_get(&fs, "SYS:C/ping") == 1 &&
          fs_get(&fs, "SYS:C/ping.new") == 2 && fs_get(&fs, "U:Update/prior-old/1") == 9);
    CHECK(strstr(fs.journal, "aside") == 0);

    /* Journalled aside: a copy that was made whole; the original goes. */
    e[0].aside = 1;
    fs_put(&fs, "U:Update/prior-old/1", 3);
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 1);
    CHECK(fs_get(&fs, "SYS:C/ping") == 2 && fs_get(&fs, "SYS:C/ping.old") == 1 &&
          fs_get(&fs, "U:Update/prior-old/1") == 3);
}

/* Each selected member exactly once: a duplicate of one and the lack of
   another counted as many as were wanted (#497 review). */
static void test_all_staged(void)
{
    UpdJEntry e[3];

    memset(e, 0, sizeof(e));
    CHECK(upd_all_staged(e, 0) == 1);
    e[0].staged = 1;
    e[2].staged = 1;
    CHECK(upd_all_staged(e, 3) == 0);
    e[1].staged = 1;
    CHECK(upd_all_staged(e, 3) == 1);
}

/*
 * An empty "$VER: " before the real one -- UpdateAmiNetXDuo's scanner key,
 * NetEnv's template, both NUL-ended in a program's data -- is skipped,
 * and the scan goes on to the version (#497 review: the scan stopped
 * there, found no version, and a newer build was replaced).
 */
static void test_scan_skip(void)
{
    static const char bin[] =
        "\0\0$VER: \0code AmiNetXDuo $VER: \0$VER: junk\0"
        "$VER: UpdateAmiNetXDuo 9.9.9 (17.11.2030)\0tail";
    unsigned char r[32], d[32], f[32], act, why;
    UpdScan       sc;
    UpdVer        v, upd;
    UpdSelIn      in;
    static const char empty[] = "AmiNetXDuo$VER: \0$VER: \0";
    unsigned long i, step;

    for (step = 1; step <= sizeof(bin); step++)
    {
        upd_scan_init(&sc);
        for (i = 0; i < sizeof(bin) - 1; i += step)
            upd_scan_feed(&sc, (const unsigned char *)bin + i,
                          i + step <= sizeof(bin) - 1 ? step : sizeof(bin) - 1 - i);
        CHECK(sc.mark == 1);
        CHECK(upd_scan_version(&sc, &v) == 1 && v.major == 9 && v.minor == 9);
    }

    /* So the newer build is kept, not replaced. */
    fill(r, 1);
    fill(f, 2);
    fill(d, 3);
    CHECK(upd_ver_parse("1.0.0-rc5", 9, &upd));
    memset(&in, 0, sizeof(in));
    in.receipt = r; in.disk = d; in.fresh = f; in.update = &upd; in.scan = &sc;
    upd_select(&in, &act, &why);
    CHECK(act == UPD_ACT_SKIP && why == UPD_WHY_NEWER);

    /* Only empty ones: no version, as before. */
    upd_scan_init(&sc);
    upd_scan_feed(&sc, (const unsigned char *)empty, sizeof(empty) - 1);
    CHECK(upd_scan_version(&sc, &v) == 0);
    /* The file ends in the middle of it: what came counts. */
    upd_scan_init(&sc);
    upd_scan_feed(&sc, (const unsigned char *)"$VER: ping 1.2.3", 16);
    CHECK(upd_scan_version(&sc, &v) == 1 && v.major == 1 && v.patch == 3);
}

/*
 * The fail-closed rules of review three: absent is ERROR_OBJECT_NOT_FOUND
 * alone, for the trial, the state and a live file; the state is merged
 * before PREPARE or refused; no size adds or subtracts past its range; and
 * InstallSameFile RECEIPT keeps the drawers it holds (#497 review).
 */
static void test_fail_closed(void)
{
    static const char trial_state[] = "phase=trial\nversion=1.0.1\n";
    static const char kept[] = "phase=confirmed\nlast_check=2026-11-17\nskip=1.0.3\n";
    static char       big[1200];
    char              out[256];
    unsigned long     n;

    /* The trial and the state. */
    CHECK(upd_trial_refuse(1, 0, 0, 1) == 0);           /* neither there   */
    CHECK(upd_trial_refuse(0, 0, 0, 1) == 1);           /* trial there     */
    CHECK(upd_trial_refuse(1, 0, 0, 0) == 1);           /* state unread    */
    CHECK(upd_trial_refuse(1, kept, sizeof(kept) - 1, 0) == 0);
    CHECK(upd_trial_refuse(1, trial_state, sizeof(trial_state) - 1, 0) == 1);

    CHECK(upd_ioerr_absent(205) == 1);
    CHECK(upd_ioerr_absent(202) == 0);                  /* in use          */
    CHECK(upd_ioerr_absent(218) == 0);                  /* not mounted     */
    CHECK(upd_ioerr_absent(0) == 0);

    /* The state SWAP will write: merged, or refused with nothing dropped. */
    n = upd_state_next(kept, sizeof(kept) - 1, 0, "1.0.2", "full", out, sizeof(out));
    CHECK(n > 0 && strstr(out, "phase=trial\n") != 0 &&
          strstr(out, "last_check=2026-11-17\n") != 0 && strstr(out, "skip=1.0.3\n") != 0);
    CHECK(upd_state_next(0, 0, 1, "1.0.2", "full", out, sizeof(out)) > 0);
    CHECK(upd_state_next(0, 0, 0, "1.0.2", "full", out, sizeof(out)) == 0);
    memset(big, 'k', sizeof(big) - 2);
    big[sizeof(big) - 2] = '\n';
    CHECK(upd_state_next(big, sizeof(big) - 1, 0, "1.0.2", "full", out, sizeof(out)) == 0);

    /* Sizes held, not wrapped. */
    CHECK(upd_sat_add(~0UL - 1, 5) == ~0UL && upd_sat_add(3, 4) == 7);
    CHECK(upd_free_blocks(100, 120) == 0 && upd_free_blocks(120, 100) == 20);
    CHECK(upd_dl_chunk(1, 0xFFFFFF00UL, 0x200, 0xFFFFFFF0UL) == UPD_DL_LONG);
    CHECK(upd_dl_chunk(1, 0x10, 0xFFFFFFFFUL, 1000) == UPD_DL_LONG);
    CHECK(upd_dl_chunk(1, 1000, 24, 1024) == UPD_DL_OK);

    /* InstallSameFile RECEIPT. */
    CHECK(upd_rcpt_write_plan(0, 0, 0) == UPD_RW_ERROR);    /* in use      */
    CHECK(upd_rcpt_write_plan(0, 1, 0) == UPD_RW_NONE);     /* C/fat, first */
    CHECK(upd_rcpt_write_plan(0, 1, 67) == UPD_RW_WRITE);
    CHECK(upd_rcpt_write_plan(1, 0, 0) == UPD_RW_NONE);
    CHECK(upd_rcpt_write_plan(1, 0, 134) == UPD_RW_WRITE);
}

/*
 * A look that fails with a DOS error, not "not found", stops the swap:
 * nothing removed, no line written (#507 review: an error on
 * prior-old/<n> during a recovery read as there and the .old was deleted;
 * one on the live file with no .new wrote "done").
 */
static void test_exists_error(void)
{
    static Fs   fs;
    UpdJEntry   e[1];
    const char *prior = "U:Update/prior-old";

    /* Recovery after "aside 1": prior-old/1 cannot be looked at. */
    memset(&fs, 0, sizeof(fs));
    memset(e, 0, sizeof(e));
    fs_ops.ctx = &fs;
    fs.budget = -1;
    e[0].path = "SYS:C/ping";
    e[0].len  = 10;
    e[0].aside = 1;
    fs_put(&fs, "SYS:C/ping", 1);
    fs_put(&fs, "SYS:C/ping.new", 2);
    fs_put(&fs, "SYS:C/ping.old", 3);
    fs.err = "U:Update/prior-old/1";
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 0);
    CHECK(fs_get(&fs, "SYS:C/ping.old") == 3 && fs_get(&fs, "SYS:C/ping") == 1 &&
          fs_get(&fs, "SYS:C/ping.new") == 2);
    CHECK(fs.journal[0] == '\0' && e[0].done == 0);

    /* The same before "aside": no aside line either. */
    e[0].aside = 0;
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 0);
    CHECK(fs.journal[0] == '\0' && fs_get(&fs, "SYS:C/ping.old") == 3);

    /* The .old cannot be looked at. */
    fs.err = "SYS:C/ping.old";
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 0);
    CHECK(fs.journal[0] == '\0' && fs_get(&fs, "SYS:C/ping") == 1);

    /* No .new, and the live file cannot be looked at: no "done". */
    memset(&fs, 0, sizeof(fs));
    fs_ops.ctx = &fs;
    fs.budget = -1;
    e[0].aside = 0;
    fs_put(&fs, "SYS:C/ping", 2);
    fs_put(&fs, "SYS:C/ping.old", 1);
    fs.err = "SYS:C/ping";
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 0);
    CHECK(fs.journal[0] == '\0' && e[0].done == 0);
    CHECK(fs_get(&fs, "SYS:C/ping") == 2 && fs_get(&fs, "SYS:C/ping.old") == 1);

    /* The .new cannot be looked at. */
    fs.err = "SYS:C/ping.new";
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 0);
    CHECK(fs.journal[0] == '\0');

    /* Cleared: the same disk finishes. */
    fs.err = 0;
    CHECK(upd_forward(e, 1, prior, &fs_ops, 0) == 1 && e[0].done == 1);
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
        test_scan_skip();
        test_download();
    }
    if (strcmp(which, "journal") == 0 || strcmp(which, "all") == 0)
    {
        test_journal_parse();
        test_state();
        test_trial();
        test_swap_fits();
        test_stale_journal();
        test_staging();
        test_lock_why();
        test_names();
        test_stale_prior();
        test_all_staged();
        test_fail_closed();
        test_exists_error();
        test_swap("U:Installed/Update/prior-old");   /* the volume of most */
        test_swap("B:Installed/Update/prior-old");   /* another: copies    */
    }
    if (strcmp(which, "trial") == 0 || strcmp(which, "all") == 0)
    {
        test_state_get();
        test_record();
        test_trial_cut("U:Installed/Update/prior-old", 1);
        test_trial_cut("B:Installed/Update/prior-old", 1);
        test_trial_cut("U:Installed/Update/prior-old", 0);
        test_trial_cut("B:Installed/Update/prior-old", 0);
        test_trial_errors("U:Installed/Update/prior-old", 1);
        test_trial_errors("B:Installed/Update/prior-old", 1);
        test_trial_errors("U:Installed/Update/prior-old", 0);
        test_trial_errors("B:Installed/Update/prior-old", 0);
        test_trial_edges();
    }

    printf("test_updinstall %s: %d checks, %d failures\n", which, checks, failures);
    return failures != 0;
}
