/*
 * AmiNetXDuo, host-side test for the BPF filter VM, the validator and the
 * capture ring.
 *
 * Builds and runs on the development host: bpf_filter.c, bpf_validate.c,
 * bpf_channel.c and bpf_tap.c contain no AmigaOS calls, so all they need is
 * the <exec/types.h> shim in src/config/test/shim, the stubs below, and the
 * replica ABI that -DAMI_BPF_REPLICA selects in include/aminetxduo/bpf.h.
 *
 * The filter programs below are what libpcap emits for "ip", "arp" and
 * "tcp port 80" on an Ethernet link. They run against real frames, and the
 * accept or reject decision is asserted each way round. The "tcp port 80"
 * program exercises the awkward parts: a BPF_LDX|BPF_B|BPF_MSH to pick the IP
 * header length out of the low nibble, a BPF_JSET against the fragment-offset
 * field, and two BPF_IND loads through X.
 *
 * Not covered here, and what covers it instead:
 *   - the exact bpf_hdr and bpf_insn offsets and the BIOC* encodings: asserted
 *     at compile time against the real NDK <net/bpf.h> by bpf_abi_check.c
 *   - Forbid()/Permit(), Signal(), and GetSysTime(): tests/mbuf_bpf/
 *   - ami_bpf_tap_tx(), which needs an NX_PACKET: tests/mbuf_bpf/
 *
 *   cc -std=c11 -Wall -Wextra -DAMI_BPF_REPLICA -I../../../include \
 *      -I../../config/test/shim -I.. test_bpf.c ../bpf_filter.c \
 *      ../bpf_validate.c ../bpf_channel.c ../bpf_tap.c -o test_bpf
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel ownership is a library base on the Amiga. Here it is a token, with
   a second one to prove that a stranger gets EPERM. */
static char t_bpf_base_a;
static char t_bpf_base_b;
#define T_BPF_OWNER  ((APTR)&t_bpf_base_a)
#define T_BPF_OTHER  ((APTR)&t_bpf_base_b)

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;
static int   stub_verbose;
static ULONG stub_clock_sec = 1000;
static APTR  stub_task      = (APTR)"task";
static APTR  stub_signalled_task;
static ULONG stub_signalled_mask;

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    p = calloc(1, size);
    if (p != NULL)
        stub_outstanding++;

    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID) { return stub_outstanding; }

VOID ami_log(int level, const char *fmt, ...)
{
    va_list args;

    (void)level;
    if (!stub_verbose)
        return;

    va_start(args, fmt);
    fputs("  [log] ", stdout);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
}

static void (*stub_on_lock)(void);

VOID ami_bpf_lock(VOID)
{
    if (stub_on_lock != NULL)
    {
        void (*fn)(void) = stub_on_lock;

        stub_on_lock = NULL;
        fn();
    }
}

/*
 * Unlock is the only place inside ami_bpf_read() where another task can get
 * in, so the interleave test below starts its second task there. The hook
 * fires once, on the Nth unlock, and then clears itself.
 */
static void (*stub_on_unlock)(void);
static int    stub_unlock_after;

VOID ami_bpf_unlock(VOID)
{
    if (stub_on_unlock != NULL && --stub_unlock_after == 0)
    {
        void (*fn)(void) = stub_on_unlock;

        stub_on_unlock = NULL;
        fn();
    }
}

VOID ami_bpf_time_init(VOID) { }

VOID ami_bpf_now(ULONG *sec, ULONG *usec)
{
    *sec  = stub_clock_sec;
    *usec = 500000;
}

APTR ami_bpf_current_task(VOID) { return stub_task; }

VOID ami_bpf_sleep(ULONG ticks) { (VOID)ticks; }
ULONG ami_bpf_signals_set(ULONG mask) { (VOID)mask; return 0UL; }

VOID ami_bpf_notify(APTR task, ULONG mask)
{
    if (task == NULL || mask == 0)
        return;

    stub_signalled_task = task;
    stub_signalled_mask |= mask;
}

/* ----------------------------------------------------------- check harness */

static int checks;
static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------ frame builder */

#define ETH_HDR 14

static const UBYTE mac_a[6] = { 0x00, 0x80, 0x10, 0x11, 0x22, 0x33 };
static const UBYTE mac_b[6] = { 0x00, 0x60, 0x97, 0xAA, 0xBB, 0xCC };

static void put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)v;
}

static void put32be(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

/*
 * Ethernet + IPv4 + TCP, with the two fields that the filter under test reads:
 * `ihl_words` (so BPF_MSH has something to compute) and `fragoff` (so the
 * BPF_JSET #0x1fff fragment check has something to reject).
 */
static ULONG make_tcp(UBYTE *buf, UWORD sport, UWORD dport, UBYTE ihl_words,
                      UWORD fragoff, UBYTE proto)
{
    ULONG ip  = ETH_HDR;
    ULONG l4  = ip + (ULONG)ihl_words * 4;
    ULONG end = l4 + 20 + 6;

    memset(buf, 0, end);

    memcpy(buf + 0, mac_a, 6);
    memcpy(buf + 6, mac_b, 6);
    put16(buf + 12, 0x0800);

    buf[ip + 0] = (UBYTE)(0x40 | ihl_words);
    put16(buf + ip + 2, (UWORD)(end - ip));
    put16(buf + ip + 6, fragoff);
    buf[ip + 8] = 64;
    buf[ip + 9] = proto;
    put32be(buf + ip + 12, 0x0A000001UL);       /* 10.0.0.1 */
    put32be(buf + ip + 16, 0x0A000002UL);       /* 10.0.0.2 */

    put16(buf + l4 + 0, sport);
    put16(buf + l4 + 2, dport);
    buf[l4 + 12] = 0x50;

    memset(buf + l4 + 20, 0xEE, 6);

    return end;
}

static ULONG make_arp(UBYTE *buf)
{
    memset(buf, 0, 60);

    memset(buf + 0, 0xFF, 6);                   /* broadcast */
    memcpy(buf + 6, mac_b, 6);
    put16(buf + 12, 0x0806);

    put16(buf + 14, 1);                         /* Ethernet */
    put16(buf + 16, 0x0800);
    buf[18] = 6;
    buf[19] = 4;
    put16(buf + 20, 1);                         /* request */

    return 60;
}

/* ------------------------------------------------------- filter programs */

/* tcpdump "ip". */
static const struct bpf_insn prog_ip[] = {
    BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 12),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0800, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, 262144),
    BPF_STMT(BPF_RET | BPF_K, 0)
};

/* tcpdump "arp". */
static const struct bpf_insn prog_arp[] = {
    BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 12),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0806, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, 262144),
    BPF_STMT(BPF_RET | BPF_K, 0)
};

/*
 * tcpdump "tcp port 80", exactly as libpcap emits it for DLT_EN10MB:
 *
 *   ldh  [12] ; jeq #0x800 jf 12 ; ldb [23] ; jeq #6 jf 12 ; ldh [20]
 *   jset #0x1fff jt 12 ; ldxb 4*([14]&0xf) ; ldh [x+14] ; jeq #80 jt 11
 *   ldh [x+16] ; jeq #80 jt 11 jf 12 ; ret #262144 ; ret #0
 */
static const struct bpf_insn prog_tcp80[] = {
    BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 12),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0800, 0, 10),
    BPF_STMT(BPF_LD  | BPF_B | BPF_ABS, 23),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 6, 0, 8),
    BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 20),
    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x1FFF, 6, 0),
    BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 14),
    BPF_STMT(BPF_LD  | BPF_H | BPF_IND, 14),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 80, 2, 0),
    BPF_STMT(BPF_LD  | BPF_H | BPF_IND, 16),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 80, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, 262144),
    BPF_STMT(BPF_RET | BPF_K, 0)
};

#define NELEM(a) ((ULONG)(sizeof(a) / sizeof((a)[0])))

/* --------------------------------------------------------- validator tests */

static void test_validator(void)
{
    printf("bpf: validator\n");

    CHECK(ami_bpf_validate(prog_ip,     NELEM(prog_ip))     == 0);
    CHECK(ami_bpf_validate(prog_arp,    NELEM(prog_arp))    == 0);
    CHECK(ami_bpf_validate(prog_tcp80,  NELEM(prog_tcp80))  == 0);

    CHECK(ami_bpf_validate(NULL, 1) == -1);
    CHECK(ami_bpf_validate(prog_ip, 0) == -1);
    CHECK(ami_bpf_validate(prog_ip, (ULONG)BPF_MAXINSNS + 1) == -1);

    /* Truncated so the trailing instruction is no longer a BPF_RET. */
    CHECK(ami_bpf_validate(prog_ip, 2) == -1);

    {
        /* Backward jump: k is negative, so it becomes a huge unsigned offset. */
        static const struct bpf_insn bad[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 0),
            BPF_STMT(BPF_JMP | BPF_JA, (LONG)-2),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        CHECK(ami_bpf_validate(bad, NELEM(bad)) == -1);
    }

    {
        /* Forward jump past the end. */
        static const struct bpf_insn bad[] = {
            BPF_STMT(BPF_JMP | BPF_JA, 5),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        CHECK(ami_bpf_validate(bad, NELEM(bad)) == -1);
    }

    {
        /* A conditional jump that lands exactly at the end is still out of
           range, the same as `from + jt >= len` in 4.4BSD. */
        static const struct bpf_insn bad[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        CHECK(ami_bpf_validate(bad, NELEM(bad)) == -1);
    }

    {
        /* Scratch index past BPF_MEMWORDS, both directions. */
        static const struct bpf_insn bad_st[] = {
            BPF_STMT(BPF_ST, BPF_MEMWORDS),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn bad_ld[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_MEM, BPF_MEMWORDS),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn ok_st[] = {
            BPF_STMT(BPF_ST, BPF_MEMWORDS - 1),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        CHECK(ami_bpf_validate(bad_st, NELEM(bad_st)) == -1);
        CHECK(ami_bpf_validate(bad_ld, NELEM(bad_ld)) == -1);
        CHECK(ami_bpf_validate(ok_st,  NELEM(ok_st))  == 0);
    }

    {
        /* Division by a constant zero. A variable divisor is fine here and is
           caught at run time instead. */
        static const struct bpf_insn bad[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 0),
            BPF_STMT(BPF_ALU | BPF_DIV | BPF_K, 0),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        static const struct bpf_insn ok[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 0),
            BPF_STMT(BPF_ALU | BPF_DIV | BPF_X, 0),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        CHECK(ami_bpf_validate(bad, NELEM(bad)) == -1);
        CHECK(ami_bpf_validate(ok,  NELEM(ok))  == 0);
    }

    {
        /* Encodings that are individually valid but illegal together. */
        static const struct bpf_insn ldx_abs[] = {
            BPF_STMT(BPF_LDX | BPF_W | BPF_ABS, 0),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn ld_msh[] = {
            BPF_STMT(BPF_LD | BPF_B | BPF_MSH, 14),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn msh_half[] = {
            BPF_STMT(BPF_LDX | BPF_H | BPF_MSH, 14),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn bad_size[] = {
            BPF_STMT(BPF_LD | 0x18 | BPF_ABS, 0),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn ret_x[] = {
            BPF_STMT(BPF_RET | BPF_X, 0)
        };
        static const struct bpf_insn bad_alu[] = {
            BPF_STMT(BPF_ALU | 0xF0, 0),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        static const struct bpf_insn bad_misc[] = {
            BPF_STMT(BPF_MISC | 0x40, 0),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };
        CHECK(ami_bpf_validate(ldx_abs,  NELEM(ldx_abs))  == -1);
        CHECK(ami_bpf_validate(ld_msh,   NELEM(ld_msh))   == -1);
        CHECK(ami_bpf_validate(msh_half, NELEM(msh_half)) == -1);
        CHECK(ami_bpf_validate(bad_size, NELEM(bad_size)) == -1);
        CHECK(ami_bpf_validate(ret_x,    NELEM(ret_x))    == -1);
        CHECK(ami_bpf_validate(bad_alu,  NELEM(bad_alu))  == -1);
        CHECK(ami_bpf_validate(bad_misc, NELEM(bad_misc)) == -1);
    }
}

/* ------------------------------------------------------------- VM tests */

static void test_filter_real_programs(void)
{
    UBYTE frame[128];
    ULONG len;

    printf("bpf: real libpcap programs against real frames\n");

    /* "ip" accepts IPv4 and rejects ARP. "arp" answers the other way round. */
    len = make_tcp(frame, 1234, 80, 5, 0, 6);
    CHECK(ami_bpf_filter(prog_ip,  NELEM(prog_ip),  frame, len, len) == 262144);
    CHECK(ami_bpf_filter(prog_arp, NELEM(prog_arp), frame, len, len) == 0);

    len = make_arp(frame);
    CHECK(ami_bpf_filter(prog_ip,  NELEM(prog_ip),  frame, len, len) == 0);
    CHECK(ami_bpf_filter(prog_arp, NELEM(prog_arp), frame, len, len) == 262144);

    /* "tcp port 80": destination port matches. */
    len = make_tcp(frame, 1234, 80, 5, 0, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 262144);

    /* Source port matches (the [x+14] arm rather than [x+16]). */
    len = make_tcp(frame, 80, 1234, 5, 0, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 262144);

    /* Neither port matches. */
    len = make_tcp(frame, 4444, 22, 5, 0, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 0);

    /* Not TCP. */
    len = make_tcp(frame, 1234, 80, 5, 0, 17);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 0);

    /* Not IPv4 at all. */
    len = make_arp(frame);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 0);

    /*
     * IP options: ihl is 6, so BPF_MSH must give X = 24 and the two indexed
     * loads must land on the TCP ports four bytes further in. This is the
     * check that catches an MSH implemented as (b & 0xf) or (b << 2).
     */
    len = make_tcp(frame, 1234, 80, 6, 0, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 262144);
    len = make_tcp(frame, 1234, 22, 6, 0, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 0);

    /* A non-first fragment has no ports to read, and the JSET must catch it
       before the indexed loads run. */
    len = make_tcp(frame, 1234, 80, 5, 0x0025, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 0);

    /* The don't-fragment bit is above the offset field and must not match. */
    len = make_tcp(frame, 1234, 80, 5, 0x4000, 6);
    CHECK(ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len) == 262144);
}

static void test_filter_edges(void)
{
    UBYTE frame[128];
    ULONG len;

    printf("bpf: interpreter edge cases\n");

    len = make_tcp(frame, 1234, 80, 5, 0, 6);

    /* No program: accept everything. */
    CHECK(ami_bpf_filter(NULL, 0, frame, len, len) == (ULONG)-1);
    CHECK(ami_bpf_filter(prog_ip, 0, frame, len, len) == (ULONG)-1);

    {
        /* A read past the end rejects instead of a trap. The same frame
           truncated to 20 bytes cannot answer a load at offset 34. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 34),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) != 0);
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, 20) == 0);
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, 36) == 0);   /* 34+4 > 36 */
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, 38) != 0);
    }

    {
        /* A far out-of-range offset must not wrap into a valid one. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, (LONG)0xFFFFFFFEUL),
            BPF_STMT(BPF_RET | BPF_K, 1)
        };
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == 0);
    }

    {
        /* Indexed load where X + k wraps. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, (LONG)0xFFFFFFF0UL),
            BPF_STMT(BPF_LD  | BPF_W | BPF_IND, 0x20),
            BPF_STMT(BPF_RET | BPF_K, 1)
        };
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == 0);
    }

    {
        /* Division by a run-time zero rejects instead of trapping. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_IMM, 100),
            BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 0),
            BPF_STMT(BPF_ALU | BPF_DIV | BPF_X, 0),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == 0);
    }

    {
        /* A shift of 32 or more gives zero here, and not whatever the host
           CPU does. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD  | BPF_W | BPF_IMM, 1),
            BPF_STMT(BPF_ALU | BPF_LSH | BPF_K, 32),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == 0);
    }

    {
        /* BPF_LEN, arithmetic, TAX/TXA, scratch memory, RET A. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD   | BPF_W | BPF_LEN, 0),
            BPF_STMT(BPF_ST, 3),
            BPF_STMT(BPF_LD   | BPF_W | BPF_IMM, 0),
            BPF_STMT(BPF_LD   | BPF_W | BPF_MEM, 3),
            BPF_STMT(BPF_MISC | BPF_TAX, 0),
            BPF_STMT(BPF_LD   | BPF_W | BPF_IMM, 0),
            BPF_STMT(BPF_MISC | BPF_TXA, 0),
            BPF_STMT(BPF_ALU  | BPF_MUL | BPF_K, 2),
            BPF_STMT(BPF_ALU  | BPF_SUB | BPF_K, 4),
            BPF_STMT(BPF_RET  | BPF_A, 0)
        };
        CHECK(ami_bpf_validate(p, NELEM(p)) == 0);
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == len * 2 - 4);
    }

    {
        /* A fall off the end with no RET rejects. The validator refuses such
           a program, but the interpreter must not depend on that. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_IMM, 1)
        };
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == 0);
        CHECK(ami_bpf_validate(p, NELEM(p)) == -1);
    }

    {
        /* An unvalidated jump past the end of the program must not run off
           the array. The interpreter range-checks jumps itself. */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_JMP | BPF_JA, 1000),
            BPF_STMT(BPF_RET | BPF_K, 1)
        };
        CHECK(ami_bpf_validate(p, NELEM(p)) == -1);
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == 0);
    }
}

static void test_filter_scatter(void)
{
    UBYTE      frame[128];
    ULONG      len;
    AmiBpfView view;
    ULONG      linear;

    printf("bpf: a split view answers the same as a contiguous one\n");

    len    = make_tcp(frame, 1234, 80, 5, 0, 6);
    linear = ami_bpf_filter(prog_tcp80, NELEM(prog_tcp80), frame, len, len);

    /*
     * Split exactly where the transmit tap splits: link header in segment 0,
     * packet payload in segment 1. Every load past offset 14 now goes through
     * the segment walk.
     */
    view.wirelen = 0;
    view.caplen  = 0;
    view.nsegs   = 0;
    CHECK(ami_bpf_view_add(&view, frame, ETH_HDR) == 0);
    CHECK(ami_bpf_view_add(&view, frame + ETH_HDR, len - ETH_HDR) == 0);
    CHECK(view.wirelen == len);
    CHECK(view.caplen == len);
    CHECK(ami_bpf_filter_view(prog_tcp80, NELEM(prog_tcp80), &view) == linear);
    CHECK(ami_bpf_filter_view(prog_ip, NELEM(prog_ip), &view) == 262144);

    {
        /*
         * A load that straddles the boundary: [13] is one byte inside the link
         * header and one byte past it. The slow byte-at-a-time path in
         * ami_bpf_load() is the only path that can answer this.
         */
        static const struct bpf_insn p[] = {
            BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 13),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        ULONG expect = ((ULONG)frame[13] << 8) | (ULONG)frame[14];

        CHECK(ami_bpf_filter_view(p, NELEM(p), &view) == expect);
        CHECK(ami_bpf_filter(p, NELEM(p), frame, len, len) == expect);
    }

    {
        /* Many small segments, up to the limit. */
        ULONG i;

        view.wirelen = 0;
        view.caplen  = 0;
        view.nsegs   = 0;
        for (i = 0; i < AMI_BPF_MAX_SEGS; i++)
            CHECK(ami_bpf_view_add(&view, frame + i * 8, 8) == 0);
        CHECK(ami_bpf_view_add(&view, frame, 8) == -1);     /* view is full  */
        CHECK(view.caplen == AMI_BPF_MAX_SEGS * 8);
    }
}

/* ------------------------------------------------------- capture ring tests */

static APTR  inject_cookie;
static UWORD inject_type;
static UBYTE inject_dst[6];
static ULONG inject_len;
static UBYTE inject_payload[64];
static LONG  inject_result;

static LONG test_inject(APTR cookie, UWORD ether_type, const UBYTE *dst,
                        const UBYTE *payload, ULONG len)
{
    inject_cookie = cookie;
    inject_type   = ether_type;
    inject_len    = len;

    if (dst != NULL)
        memcpy(inject_dst, dst, 6);

    if (len > sizeof(inject_payload))
        len = sizeof(inject_payload);
    memcpy(inject_payload, payload, len);

    return inject_result;
}

/* Everything a consumer needs to walk one record. */
typedef struct
{
    ULONG sec;
    ULONG usec;
    ULONG caplen;
    ULONG datalen;
    ULONG hdrlen;
    ULONG stride;
} Rec;

static void read_rec(const UBYTE *p, Rec *r)
{
    r->sec     = ami_bpf_get32(p + AMI_BPF_OFF_TSTAMP_SEC);
    r->usec    = ami_bpf_get32(p + AMI_BPF_OFF_TSTAMP_USEC);
    r->caplen  = ami_bpf_get32(p + AMI_BPF_OFF_CAPLEN);
    r->datalen = ami_bpf_get32(p + AMI_BPF_OFF_DATALEN);
    r->hdrlen  = (ULONG)ami_bpf_get16(p + AMI_BPF_OFF_HDRLEN);
    r->stride  = BPF_WORDALIGN(r->hdrlen + r->caplen);
}

static APTR iface_cookie = (APTR)"eth0-iface";

static void test_channel_basics(void)
{
    ULONG value;
    char  name[IFNAMSIZ];

    printf("bpf: channel lifecycle and ioctls\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);

    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);
    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == AMI_BPF_EBUSY);
    CHECK(ami_bpf_open(T_BPF_OWNER, AMI_BPF_MAX_CHANNELS) == AMI_BPF_ENXIO);

    /* "Any free one" skips the channel already taken and names the one it
       claimed, the form that the Roadshow libpcap uses. */
    CHECK(ami_bpf_open(T_BPF_OWNER, -1) == 1);
    CHECK(ami_bpf_close(T_BPF_OWNER, 1) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 1, BIOCFLUSH, NULL) == AMI_BPF_ENXIO);

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGBLEN, &value) == 0);
    CHECK(value == AMI_BPF_DEFAULT_BLEN);

    /* Clamped to the documented range and word-aligned. */
    value = 7;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSBLEN, &value) == 0);
    CHECK(value == (ULONG)BPF_MINBUFSIZE);
    value = 0x10000;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSBLEN, &value) == 0);
    CHECK(value == (ULONG)BPF_MAXBUFSIZE);
    value = 256;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSBLEN, &value) == 0);
    CHECK(value == 256);

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCVERSION, &value) == 0);

    /* Not bound yet. */
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGETIF, name) == AMI_BPF_EINVAL);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == 0);

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "nosuch") == AMI_BPF_EINVAL);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == 0);

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGETIF, name) == 0);
    CHECK(strcmp(name, "eth0") == 0);

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGDLT, &value) == 0);
    CHECK(value == DLT_EN10MB);

    /* Refused once the buffers exist, as in 4.4BSD. */
    value = 512;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSBLEN, &value) == AMI_BPF_EINVAL);

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCPROMISC, NULL) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, 0xDEADBEEFUL, &value) == AMI_BPF_EINVAL);

    CHECK(ami_bpf_close(T_BPF_OWNER, 0) == 0);
    CHECK(ami_bpf_close(T_BPF_OWNER, 0) == AMI_BPF_ENXIO);
    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_alloc_count() == 0);
}

static void test_capture_records(void)
{
    UBYTE            frame[128];
    UBYTE            out[4096];
    ULONG            len;
    ULONG            value;
    LONG             got;
    Rec              r0;
    Rec              r1;
    struct bpf_stat  st;
    struct bpf_program prog;

    printf("bpf: record format, padding and read framing\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);
    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == 0);

    /* No filter installed: everything is captured. */
    len = make_tcp(frame, 1234, 80, 5, 0, 6);       /* 60 bytes */
    CHECK(len == 60);

    stub_clock_sec = 111;
    ami_bpf_tap_rx(iface_cookie, frame, len);

    /* A frame whose length is not a multiple of four, so the pad between
       records has to be real. */
    stub_clock_sec = 222;
    ami_bpf_tap_rx(iface_cookie, frame, 61);

    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);

    got = ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out));

    /* Record 0. */
    read_rec(out, &r0);
    CHECK(r0.hdrlen  == AMI_BPF_HDRLEN);
    CHECK(r0.hdrlen  == BPF_WORDALIGN(AMI_BPF_HDR_BYTES));
    CHECK(r0.caplen  == 60);
    CHECK(r0.datalen == 60);
    CHECK(r0.sec     == 111);
    CHECK(r0.usec    == 500000);
    CHECK(r0.stride  == 80);
    CHECK(memcmp(out + r0.hdrlen, frame, 60) == 0);

    /* Record 1 starts at BPF_WORDALIGN(hdrlen + caplen) from record 0. */
    read_rec(out + r0.stride, &r1);
    CHECK(r1.caplen  == 61);
    CHECK(r1.datalen == 61);
    CHECK(r1.sec     == 222);
    CHECK(r1.stride  == 84);                        /* WORDALIGN(20 + 61)    */
    CHECK(memcmp(out + r0.stride + r1.hdrlen, frame, 61) == 0);

    /* The read returns both whole records and no trailing alignment on the
       last one. Otherwise a consumer that walks by stride overruns. */
    CHECK(got == (LONG)(r0.stride + r1.hdrlen + r1.caplen));
    CHECK(got == 80 + 81);

    CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out)) == 0);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == 0);

    /* Statistics: two frames seen, none dropped. */
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGSTATS, &st) == 0);
    CHECK(st.bs_recv == 2);
    CHECK(st.bs_drop == 0);

    /* A filter that snaps to 30 bytes: caplen shrinks, datalen does not. */
    {
        static const struct bpf_insn snap[] = {
            BPF_STMT(BPF_RET | BPF_K, 30)
        };

        prog.bf_len   = NELEM(snap);
        prog.bf_insns = (struct bpf_insn *)snap;
        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETF, &prog) == 0);

        ami_bpf_tap_rx(iface_cookie, frame, len);
        got = ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out));
        read_rec(out, &r0);
        CHECK(r0.caplen  == 30);
        CHECK(r0.datalen == 60);
        CHECK(got == (LONG)(AMI_BPF_HDRLEN + 30));
        CHECK(memcmp(out + r0.hdrlen, frame, 30) == 0);
    }

    /* A real filter that rejects: counted as received, but stored nowhere. */
    {
        prog.bf_len   = NELEM(prog_arp);
        prog.bf_insns = (struct bpf_insn *)prog_arp;
        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETF, &prog) == 0);
        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCFLUSH, NULL) == 0);

        ami_bpf_tap_rx(iface_cookie, frame, len);           /* IPv4: no    */
        CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == 0);

        len = make_arp(frame);
        ami_bpf_tap_rx(iface_cookie, frame, len);           /* ARP: yes    */
        CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);

        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGSTATS, &st) == 0);
        CHECK(st.bs_recv == 2);
        CHECK(st.bs_drop == 0);
    }

    /* A new filter discards what is buffered. */
    prog.bf_len   = 0;
    prog.bf_insns = NULL;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETF, &prog) == 0);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == 0);

    /* A program the validator rejects must not be installed. */
    {
        static const struct bpf_insn bad[] = {
            BPF_STMT(BPF_JMP | BPF_JA, 99),
            BPF_STMT(BPF_RET | BPF_K, 0)
        };

        prog.bf_len   = NELEM(bad);
        prog.bf_insns = (struct bpf_insn *)bad;
        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETF, &prog) == AMI_BPF_EINVAL);

        prog.bf_len   = (ULONG)BPF_MAXINSNS + 1;
        prog.bf_insns = (struct bpf_insn *)prog_ip;
        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETF, &prog) == AMI_BPF_EINVAL);
    }

    /* A caller buffer too small for the first record: nothing is consumed, so
       the next read of the correct size still gets it. */
    len = make_tcp(frame, 1234, 80, 5, 0, 6);
    ami_bpf_tap_rx(iface_cookie, frame, len);
    CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, 8) == AMI_BPF_EINVAL);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);
    CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out)) ==
          (LONG)(AMI_BPF_HDRLEN + 60));

    /* Partial drain: whole records only, and the rest survives. */
    ami_bpf_tap_rx(iface_cookie, frame, len);
    ami_bpf_tap_rx(iface_cookie, frame, len);
    got = ami_bpf_read(T_BPF_OWNER, 0, out, 80);    /* room for exactly one */
    CHECK(got == (LONG)(AMI_BPF_HDRLEN + 60));
    read_rec(out, &r0);
    CHECK(r0.caplen == 60);
    got = ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out));
    CHECK(got == (LONG)(AMI_BPF_HDRLEN + 60));
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == 0);

    value = 0;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGBLEN, &value) == 0);
    CHECK(value == AMI_BPF_DEFAULT_BLEN);

    CHECK(ami_bpf_close(T_BPF_OWNER, 0) == 0);
    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_alloc_count() == 0);
}

static void test_overflow_and_signals(void)
{
    UBYTE frame[128];
    UBYTE out[512];
    ULONG len;
    ULONG value;
    struct bpf_stat st;

    printf("bpf: buffer rotation, drops and signal notification\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);
    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);

    /* 128-byte buffers hold exactly one 80-byte record each. */
    value = 128;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSBLEN, &value) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == 0);

    CHECK(ami_bpf_set_notify_mask(T_BPF_OWNER, 0, 1UL << 12) == 0);
    value = 1;
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCIMMEDIATE, &value) == 0);

    stub_signalled_task = NULL;
    stub_signalled_mask = 0;

    len = make_tcp(frame, 1234, 80, 5, 0, 6);

    ami_bpf_tap_rx(iface_cookie, frame, len);   /* store                    */
    CHECK(stub_signalled_task == stub_task);
    CHECK(stub_signalled_mask == (1UL << 12));

    ami_bpf_tap_rx(iface_cookie, frame, len);   /* store full -> rotate     */
    ami_bpf_tap_rx(iface_cookie, frame, len);   /* both full -> drop        */
    ami_bpf_tap_rx(iface_cookie, frame, len);   /* drop                     */

    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCGSTATS, &st) == 0);
    CHECK(st.bs_recv == 4);
    CHECK(st.bs_drop == 2);

    /* Draining lets capture resume. */
    CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out)) ==
          (LONG)(AMI_BPF_HDRLEN + 60));
    CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out)) ==
          (LONG)(AMI_BPF_HDRLEN + 60));
    CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out)) == 0);

    ami_bpf_tap_rx(iface_cookie, frame, len);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);

    /* A frame larger than the whole buffer is truncated, not dropped. */
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCFLUSH, NULL) == 0);
    {
        UBYTE big[1024];
        Rec   r;

        memset(big, 0x5A, sizeof(big));
        memcpy(big, frame, ETH_HDR);
        ami_bpf_tap_rx(iface_cookie, big, (ULONG)sizeof(big));
        CHECK(ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out)) == 128);
        read_rec(out, &r);
        CHECK(r.caplen  == 128 - AMI_BPF_HDRLEN);
        CHECK(r.datalen == 1024);
    }

    CHECK(ami_bpf_close(T_BPF_OWNER, 0) == 0);
    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_alloc_count() == 0);
}

static void test_write_and_binding(void)
{
    UBYTE frame[128];
    ULONG len;

    printf("bpf: write splits the link header, and late binding works\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);

    /* A bind before the interface exists fails, and works after the attach. */
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == AMI_BPF_EINVAL);

    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == 0);

    len = make_tcp(frame, 1234, 80, 5, 0, 6);

    inject_result = 0;
    inject_type   = 0;
    inject_len    = 0;
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, (LONG)len) == (LONG)len);
    CHECK(inject_cookie == iface_cookie);
    CHECK(inject_type == 0x0800);
    CHECK(memcmp(inject_dst, mac_a, 6) == 0);
    CHECK(inject_len == len - ETH_HDR);
    CHECK(memcmp(inject_payload, frame + ETH_HDR, 16) == 0);

    /* Shorter than a link header, or longer than the MTU. */
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, 13) == AMI_BPF_EINVAL);
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, 0) == AMI_BPF_EINVAL);
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, NULL, 20) == AMI_BPF_EINVAL);
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, 1500 + ETH_HDR + 1) ==
          AMI_BPF_EMSGSIZE);

    inject_result = -1;
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, (LONG)len) == AMI_BPF_ENOBUFS);
    inject_result = 0;

    /*
     * A detach unbinds the channel and does not close it. A reattach under the
     * same name rebinds it, because the channel remembers the name it asked
     * for. A capture therefore survives an interface that goes offline and
     * returns.
     */
    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, (LONG)len) == AMI_BPF_ENXIO);
    ami_bpf_tap_rx(iface_cookie, frame, len);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == 0);

    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);
    ami_bpf_tap_rx(iface_cookie, frame, len);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);
    CHECK(ami_bpf_write(T_BPF_OWNER, 0, frame, (LONG)len) == (LONG)len);

    /* An interface with no injector accepts capture but refuses write. */
    {
        static APTR other = (APTR)"eth1-iface";

        CHECK(ami_bpf_attach_interface("eth1", other, DLT_EN10MB, 1500,
                                       NULL) == 0);
        CHECK(ami_bpf_open(T_BPF_OWNER, 1) == 1);
        CHECK(ami_bpf_ioctl(T_BPF_OWNER, 1, BIOCSETIF, "eth1") == 0);
        CHECK(ami_bpf_write(T_BPF_OWNER, 1, frame, (LONG)len) == AMI_BPF_ENXIO);
        ami_bpf_tap_rx(other, frame, len);
        CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 1) > 0);
        CHECK(ami_bpf_close(T_BPF_OWNER, 1) == 0);
        ami_bpf_detach_interface(other);
    }

    /* Taps for an interface nobody registered are ignored. */
    ami_bpf_tap_rx((APTR)"stranger", frame, len);

    ami_bpf_cleanup();
    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_alloc_count() == 0);
}

/*
 * "The packet filter channel you allocate will be associated with the library
 * base ... It will be automatically closed when the library is closed", and
 * EPERM for every call from anyone else.
 */
static void test_channel_ownership(void)
{
    UBYTE frame[128];
    ULONG value;
    ULONG len;
    char  name[IFNAMSIZ];

    printf("bpf: channels belong to the base that opened them\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);

    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == 0);

    /* A stranger sees that the channel exists but cannot touch it. */
    CHECK(ami_bpf_close(T_BPF_OTHER, 0) == AMI_BPF_EPERM);
    CHECK(ami_bpf_read(T_BPF_OTHER, 0, frame, (LONG)sizeof(frame)) ==
          AMI_BPF_EPERM);
    CHECK(ami_bpf_write(T_BPF_OTHER, 0, frame, 60) == AMI_BPF_EPERM);
    CHECK(ami_bpf_ioctl(T_BPF_OTHER, 0, BIOCGETIF, name) == AMI_BPF_EPERM);
    CHECK(ami_bpf_data_waiting(T_BPF_OTHER, 0) == AMI_BPF_EPERM);
    CHECK(ami_bpf_set_notify_mask(T_BPF_OTHER, 0, 1UL << 5) == AMI_BPF_EPERM);
    CHECK(ami_bpf_set_interrupt_mask(T_BPF_OTHER, 0, 1UL << 5) ==
          AMI_BPF_EPERM);

    /* An unopened channel is ENXIO for everyone, owner or not: the handle
       comes first, so a stranger cannot probe which channels are taken. */
    CHECK(ami_bpf_ioctl(T_BPF_OTHER, 1, BIOCGBLEN, &value) == AMI_BPF_ENXIO);

    /* A channel that the stranger opens belongs to the stranger. */
    CHECK(ami_bpf_open(T_BPF_OTHER, 1) == 1);
    CHECK(ami_bpf_ioctl(T_BPF_OTHER, 1, BIOCSETIF, "eth0") == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 1, BIOCGBLEN, &value) == AMI_BPF_EPERM);

    len = make_tcp(frame, 1234, 80, 5, 0, 6);
    ami_bpf_tap_rx(iface_cookie, frame, len);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);
    CHECK(ami_bpf_data_waiting(T_BPF_OTHER, 1) > 0);

    /* A close of one base releases its channels and leaves those of the other
       base alone. */
    ami_bpf_close_owner(T_BPF_OWNER);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == AMI_BPF_ENXIO);
    CHECK(ami_bpf_data_waiting(T_BPF_OTHER, 1) > 0);
    CHECK(ami_bpf_capturing() == 1);

    /* The channel is free again, and goes to the next caller that asks. */
    CHECK(ami_bpf_open(T_BPF_OTHER, -1) == 0);
    CHECK(ami_bpf_close(T_BPF_OTHER, 0) == 0);

    ami_bpf_close_owner(T_BPF_OTHER);
    CHECK(ami_bpf_capturing() == 0);
    CHECK(ami_bpf_data_waiting(T_BPF_OTHER, 1) == AMI_BPF_ENXIO);

    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_alloc_count() == 0);
}

/*
 * Two closes can validate the same numeric slot before either takes the table
 * lock. Let the first close finish and another owner reopen the slot while the
 * second is entering its critical section: the stale close must not retire
 * the replacement.
 */
static void t_replace_mid_close(void)
{
    CHECK(ami_bpf_close(T_BPF_OWNER, 0) == 0);
    CHECK(ami_bpf_open(T_BPF_OTHER, 0) == 0);
}

static void test_reopen_under_closer(void)
{
    ULONG value = 0;

    printf("bpf: a stale close cannot retire a recycled channel\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);

    stub_on_lock = t_replace_mid_close;
    CHECK(ami_bpf_close(T_BPF_OWNER, 0) == AMI_BPF_EPERM);
    CHECK(stub_on_lock == NULL);

    CHECK(ami_bpf_ioctl(T_BPF_OTHER, 0, BIOCGBLEN, &value) == 0);
    CHECK(value == AMI_BPF_DEFAULT_BLEN);
    CHECK(ami_bpf_close(T_BPF_OTHER, 0) == 0);
    CHECK(ami_alloc_count() == 0);
}

/*
 * The base is closed during a copy-out on one of its channels.
 *
 * ami_bpf_close() answers EBUSY for this, because a caller can be told to try
 * again. ami_bpf_close_owner() cannot, because it runs from
 * bsd_child_destroy() on CloseLibrary() and has nowhere to put a refusal. It
 * therefore takes the channel away at once and leaves the two allocations for
 * the reader, which still copies out of one of them. A free there hands freed
 * memory to a live memcpy, and there is no MMU to notice.
 *
 * The reproduction is exact and not an approximation. The hook below runs on
 * the unlock that ami_bpf_read() takes for the copy outside the lock, the one
 * moment when the window is open.
 */
static ULONG t_race_allocs;

static void t_close_owner_mid_read(void)
{
    ami_bpf_close_owner(T_BPF_OWNER);
    t_race_allocs = ami_alloc_count();
}

static void test_close_owner_under_reader(void)
{
    UBYTE frame[128];
    UBYTE out[512];
    ULONG before;
    ULONG len;
    LONG  got;

    printf("bpf: closing the base under a reader keeps its buffer alive\n");

    CHECK(ami_bpf_init() == 0);
    CHECK(ami_bpf_attach_interface("eth0", iface_cookie, DLT_EN10MB, 1500,
                                   test_inject) == 0);

    CHECK(ami_bpf_open(T_BPF_OWNER, 0) == 0);
    CHECK(ami_bpf_ioctl(T_BPF_OWNER, 0, BIOCSETIF, "eth0") == 0);

    len = make_tcp(frame, 1234, 80, 5, 0, 6);
    ami_bpf_tap_rx(iface_cookie, frame, len);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) > 0);

    /* The first unlock inside ami_bpf_read() is the copy-out one: there is a
       record waiting, so the wait loop breaks with the lock still held. */
    before            = ami_alloc_count();
    t_race_allocs     = 0;
    stub_unlock_after = 1;
    stub_on_unlock    = t_close_owner_mid_read;

    got = ami_bpf_read(T_BPF_OWNER, 0, out, (LONG)sizeof(out));

    CHECK(stub_on_unlock == NULL);                  /* the hook did fire */

    /* The defect: the allocations of the channel still exist when the close
       returns, so the copy below reads memory that is still valid. */
    CHECK(before != 0);
    CHECK(t_race_allocs == before);

    /* And the caller gets the frame it asked for. */
    CHECK(got == (LONG)(AMI_BPF_HDRLEN + len));
    CHECK(memcmp(out + AMI_BPF_HDRLEN, frame, len) == 0);

    /* Nothing is left behind: the reader freed what the close deferred. */
    CHECK(ami_alloc_count() == 0);
    CHECK(ami_bpf_capturing() == 0);
    CHECK(ami_bpf_data_waiting(T_BPF_OWNER, 0) == AMI_BPF_ENXIO);

    /* The slot is free again, and not left reserved by the deferral. */
    CHECK(ami_bpf_open(T_BPF_OTHER, -1) == 0);
    CHECK(ami_bpf_close(T_BPF_OTHER, 0) == 0);

    ami_bpf_detach_interface(iface_cookie);
    CHECK(ami_alloc_count() == 0);
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        stub_verbose = 1;

    printf("bpf: record header %lu bytes, bh_hdrlen %lu\n",
           (unsigned long)AMI_BPF_HDR_BYTES, (unsigned long)AMI_BPF_HDRLEN);

    test_validator();
    test_filter_real_programs();
    test_filter_edges();
    test_filter_scatter();
    test_channel_basics();
    test_capture_records();
    test_overflow_and_signals();
    test_write_and_binding();
    test_channel_ownership();
    test_reopen_under_closer();
    test_close_owner_under_reader();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
