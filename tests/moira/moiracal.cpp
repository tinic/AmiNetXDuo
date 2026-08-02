/*
 * AmiNetXDuo -- can Moira give us deterministic host-side cycle counts?
 *
 * This is the same question tests/perf/cpucal.c asks of an emulator, asked of
 * a library instead: run instruction sequences whose cost on real silicon is
 * published, and report what the model charges for them.  The difference is
 * that there is no operating system here, no interrupts and no clock -- the
 * answer is an integer, and running it twice gives the same integer, so no
 * best-of-nine is needed and a cycle budget can be asserted in CI.
 *
 * Three questions, in the order they decide anything:
 *
 *   1. Does Moira charge a 68000 what the M68000PRM says?
 *   2. Does it charge a 68020 what the MC68020UM says -- and does it model
 *      the 256-byte instruction cache?  Our unroll depths are tuned against
 *      that cache, so a model without one cannot reproduce our measurements.
 *   3. Can it be fed the object code the cross toolchain actually produces?
 *
 * Question 3 is answered by construction: the image this loads is
 * src/net68k/n68k_copy.S and n68k_checksum.S assembled by the same
 * m68k-amigaos-gcc the Amiga build uses, linked, and loaded through the hunk reader in hunkload.h.
 * See build.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include "Moira.h"
#include "hunkload.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>

using namespace moira;

/* ------------------------------------------------------------- the machine -- */

static const uint32_t RAM_SIZE  = 16u * 1024u * 1024u;
static const uint32_t CODE_BASE = 0x00002000u;   /* clear of the vector table */
static const uint32_t SCRATCH   = 0x00100000u;   /* buffers under test        */
static const uint32_t STACKTOP  = 0x00080000u;

class Sim : public Moira {

public:

    uint8_t *ram;
    bool     trace = false;

    Sim() { ram = new uint8_t[RAM_SIZE](); }
    ~Sim() override { delete[] ram; }

    uint8_t  read8 (uint32_t a) const override { return ram[a & (RAM_SIZE - 1)]; }
    uint16_t read16(uint32_t a) const override
    {
        uint32_t i = a & (RAM_SIZE - 1);
        return uint16_t((ram[i] << 8) | ram[(i + 1) & (RAM_SIZE - 1)]);
    }
    void write8 (uint32_t a, uint8_t v) const override { ram[a & (RAM_SIZE - 1)] = v; }
    void write16(uint32_t a, uint16_t v) const override
    {
        uint32_t i = a & (RAM_SIZE - 1);
        ram[i] = uint8_t(v >> 8);
        ram[(i + 1) & (RAM_SIZE - 1)] = uint8_t(v);
    }

    void poke32(uint32_t a, uint32_t v) const
    {
        write16(a, uint16_t(v >> 16));
        write16(a + 2, uint16_t(v));
    }
    uint32_t peek32(uint32_t a) const
    {
        return (uint32_t(read16(a)) << 16) | read16(a + 2);
    }

    /* `clock` is protected -- the only thing this evaluation needs that the
       public API does not already expose. */
    int64_t cycles() const { return clock; }
};

static Sim         cpu;
static hunk::Image img;

static uint32_t S(const char *name) { return img[name]; }

/* --------------------------------------------------------------- measuring -- */

/*
 * Seed hooks.  Every kernel runs with the same register picture unless it says
 * otherwise, so a movem row and an add row are comparable.
 */
typedef void (*Seed)(void);

static void seed_default(void)
{
    for (int i = 0; i < 8; i++) cpu.setD(i, 0x12345678u + uint32_t(i));
    for (int i = 0; i < 6; i++) cpu.setA(i, SCRATCH + uint32_t(i) * 0x400u);
    cpu.setA(6, SCRATCH + 0x4000u);
}

/* MULU/MULS on a 68000 are data dependent, so the multiplier has to be pinned
   before the published formula can be checked.  0x00FF is eight one-bits,
   which makes MULU 38+2*8 = 54; the same word Booth-encodes to two 0-1
   transitions, which makes MULS 38+2*2 = 42. */
#define MUL_SRC   0x000000FFu

static void seed_mul(void)
{
    seed_default();
    cpu.setD(1, MUL_SRC);
}

/* Z set, so a Bcc that tests it is taken. */
static void seed_zset(void)
{
    seed_default();
    cpu.setCCR(uint8_t(cpu.getCCR() | 0x04));
}

/*
 * One instruction, one number.  The reset vector points at the instruction, so
 * the prefetch queue is primed exactly as the hardware would have it, and
 * execute() runs precisely one instruction.  No loop to subtract, no repeat
 * count, no statistics: run it again and it is the same integer.
 */
static int64_t cycles_of(uint32_t addr, Seed seed = seed_default)
{
    cpu.poke32(0, STACKTOP);
    cpu.poke32(4, addr);
    cpu.reset();
    seed();

    int64_t c0 = cpu.cycles();
    cpu.execute();
    return cpu.cycles() - c0;
}

/*
 * A whole routine, called the way C calls it.  Arguments are written where the
 * caller would have pushed them; the trampoline's jsr pushes the return address
 * on top, so the callee finds arg1 at sp@(4) as it expects.
 */
static int64_t cycles_of_call(uint32_t target, const std::vector<uint32_t> &args,
                              uint32_t *d0_out = nullptr)
{
    uint32_t sp = STACKTOP - uint32_t(args.size()) * 4u;

    for (size_t i = 0; i < args.size(); i++) cpu.poke32(sp + uint32_t(i) * 4u, args[i]);

    cpu.poke32(S("_tramp_target"), target);
    cpu.poke32(0, sp);
    cpu.poke32(4, S("_tramp"));
    cpu.reset();
    seed_default();

    uint32_t end = S("_tramp_end");
    int64_t  c0  = cpu.cycles();
    int64_t  guard = 0;

    while (cpu.getPC() != end) {

        cpu.execute();
        if (++guard > 200000000) { fprintf(stderr, "runaway at %08x\n", cpu.getPC()); exit(1); }
        if (cpu.isHalted()) { fprintf(stderr, "halted at %08x\n", cpu.getPC()); exit(1); }
    }

    if (d0_out) *d0_out = cpu.getD(0);
    return cpu.cycles() - c0;
}

/* The jsr/rts the trampoline itself costs, so a routine row is the routine. */
static int64_t call_overhead(void)
{
    return cycles_of_call(S("_k_rts"), {});
}

/* ---------------------------------------------------------------- reporting -- */

static int failures = 0;
static int checks   = 0;

static void row(const char *what, uint32_t addr, int published, Seed seed = seed_default)
{
    int64_t c = cycles_of(addr, seed);

    checks++;

    if (published < 0) {

        printf("  %-26s %5lld cycles   (no published figure quoted)\n",
               what, (long long)c);
        return;
    }

    bool ok = (c == published);
    if (!ok) failures++;

    printf("  %-26s %5lld cycles   published %3d   %s\n",
           what, (long long)c, published, ok ? "match" : "** MISMATCH **");
}

/* ------------------------------------------------------------------ suites -- */

struct Pub { int c68000; int c68020; };

/*
 * Published costs.
 *
 * 68000: M68000 Programmer's Reference Manual, "Instruction Execution Times".
 *        The figures there are the total including the operand fetches, which
 *        is what a single-instruction measurement sees.
 * 68020: MC68020 User's Manual, appendix "Instruction Timing Tables".  Those
 *        are given as head/tail/cycles triples against a best/cache/worst
 *        spread; the number quoted here is the manual's "cache case" total,
 *        the same convention tests/perf/cpucal.c uses.
 *
 * A -1 means "we are not asserting a figure here, print what the model says".
 */

static void suite_registers(bool is020)
{
    printf("\n-- register instructions ----------------------------------------\n");

    /*                                            68000  68020 */
    row("ADD.L  Dn,Dm",  S("_k_add_l"),   is020 ?  2 :  8);
    row("ADD.W  Dn,Dm",  S("_k_add_w"),   is020 ?  2 :  4);
    row("MOVE.L Dn,Dm",  S("_k_move_l"),  is020 ?  2 :  4);
    row("MOVE.W Dn,Dm",  S("_k_move_w"),  is020 ?  2 :  4);
    row("ADDX.L Dn,Dm",  S("_k_addx_l"),  is020 ?  2 :  8);
    row("MOVEQ  #0,Dn",  S("_k_moveq"),   is020 ?  2 :  4);
    /* LSL.L #n,Dm is 8+2n on a 68000; n is 2 here.  The 68020 charges a flat
       4 for a shift by an immediate count. */
    row("LSL.L  #2,Dn",  S("_k_lsl_l"),   is020 ?  4 : 12);
    row("NOP",           S("_k_nop"),     is020 ?  2 :  4);

    /* MULU.W is data dependent on a 68000 (38 + 2n, n = ones in the source);
       MULS.W is 38 + 2n over the Booth-encoded source.  The 68020 is a flat
       27 for both.  seed_mul pins the multiplier so both are checkable. */
    row("MULU.W Dn,Dm",  S("_k_mulu_w"),  is020 ? 27 : 54, seed_mul);
    row("MULS.W Dn,Dm",  S("_k_muls_w"),  is020 ? 27 : 42, seed_mul);
}

static void suite_memory(bool is020)
{
    printf("\n-- memory operands ----------------------------------------------\n");

    row("MOVE.L (An)+,Dn", S("_k_move_l_pi_d"),  is020 ?  4 : 12);
    row("MOVE.L Dn,(An)+", S("_k_move_l_d_pi"),  is020 ?  4 : 12);
    row("MOVE.L (An)+,(Am)+", S("_k_move_l_pi_pi"), is020 ? 5 : 20);
    row("MOVE.W (An)+,(Am)+", S("_k_move_w_pi_pi"), is020 ? 5 : 12);
}

static void suite_movem(bool is020)
{
    printf("\n-- movem, which both net68k primitives are built on -------------\n");

    /*
     * 68000: MOVEM.L (An)+,list is 12 + 8n.  MOVEM.L list,-(An) is 8 + 8n.
     * 68020: the manual gives MOVEM mem->reg as 8 + 4n for (An)+ and
     *        reg->mem as 4 + 4n for -(An), in the cache case.
     */
    struct { const char *sym; int n; } m[] = {
        { "_k_movem_l_pi_1",  1 }, { "_k_movem_l_pi_2",  2 },
        { "_k_movem_l_pi_4",  4 }, { "_k_movem_l_pi_7",  7 },
        { "_k_movem_l_pi_8",  8 }, { "_k_movem_l_pi_12", 12 },
    };

    for (auto &e : m) {

        char label[64];
        snprintf(label, sizeof label, "MOVEM.L (An)+,%d regs", e.n);
        row(label, S(e.sym), is020 ? 8 + 4 * e.n : 12 + 8 * e.n);
    }

    row("MOVEM.L 8 regs,-(An)", S("_k_movem_l_pd_8"), is020 ? 4 + 4 * 8 : 8 + 8 * 8);
    row("MOVEM.L 8 regs,(An)",  S("_k_movem_l_ea_8"), is020 ? 8 + 4 * 8 : 8 + 8 * 8);
    row("MOVEM.W (An)+,8 regs", S("_k_movem_w_pi_8"), is020 ? 8 + 4 * 8 : 12 + 4 * 8);
}

static void suite_branches(bool is020)
{
    printf("\n-- control flow -------------------------------------------------\n");

    /*
     * The assembler emitted the word-displacement forms of BRA and Bcc (the
     * targets are the following instruction, so the byte displacement would be
     * zero, which is the escape to the word form), and a PC-relative JSR.  The
     * published figures below are for those encodings, not the absolute ones.
     */
    row("BRA.W (taken)",       S("_k_bra"),       is020 ?  6 : 10);
    row("Bcc.W (taken)",       S("_k_bcc_taken"), is020 ?  6 : 10, seed_zset);
    row("Bcc.W (not taken)",   S("_k_bcc_taken"), is020 ?  6 : 12);
    row("JSR (d16,PC)",        S("_k_jsr_abs"),   is020 ?  7 : 18);
    row("RTS",                 S("_k_rts"),       is020 ?  9 : 16);

    /* DBcc with the counter non-zero: 68000 charges 10, 68020 charges 6. */
    row("DBF (not expired)",   S("_k_dbf"),       is020 ?  6 : 10);
}

/* ------------------------------------------------------------ the I-cache -- */

/*
 * The question our unroll depths depend on.  One loop, ten body sizes, the
 * same total work in each: if the model has a 256-byte instruction cache, the
 * per-pair cost turns upward somewhere near a 256-byte body.  If it has none,
 * the per-pair cost is a flat line and the model cannot say anything about
 * unroll depth at all.
 */
static void suite_icache(void)
{
    printf("\n-- instruction cache probe (68020) -------------------------------\n");
    printf("  the same work at ten body sizes; a 256 B I-cache puts a turn\n"
           "  in the cycles/pair column somewhere near 256 bytes\n\n");
    printf("  %-12s %8s %10s %14s\n", "body bytes", "pairs", "cycles", "cycles/pair");

    struct { const char *sym; int pairs; } b[] = {
        { "_ic_8",   8 }, { "_ic_16", 16 }, { "_ic_18", 18 }, { "_ic_24", 24 },
        { "_ic_30", 30 }, { "_ic_32", 32 }, { "_ic_34", 34 }, { "_ic_40", 40 },
        { "_ic_48", 48 }, { "_ic_64", 64 }, { "_ic_80", 80 },
    };

    const uint32_t iters = 200;

    for (auto &e : b) {

        int64_t c = cycles_of_call(S(e.sym), { iters - 1, SCRATCH });
        double  per = double(c) / double(iters) / double(e.pairs);

        printf("  %-12d %8d %10lld %14.4f\n",
               e.pairs * 8, e.pairs, (long long)c, per);
    }
}

/* ----------------------------------------------------- the real primitives -- */

/*
 * The ground truth these are checked against, from the emulator runs already
 * recorded in docs/:
 *
 *   68020, A1200 under FS-UAE, 14.19 MHz effective:
 *       n68k_copy_bytes   159 ns/B aligned, 209 ns/B at 2 mod 4
 *       n68k_sum_longwords 128.6 ns/B at 1460 B
 *   68000, cycle-exact A500 under WinUAE 6.0.3, 6.69 MHz effective:
 *       n68k_copy_bytes   889.8 ns/B at 0 and 2 mod 4
 *       n68k_sum_longwords 843.4 ns/B at 1460 B, 2877.6 ns/B at 20 B
 */
static void suite_primitives(bool is020)
{
    printf("\n-- src/net68k, the actual shipped object code --------------------\n");

    double mhz = is020 ? 14.19 : 6.69;
    int64_t ovh = call_overhead();

    printf("  trampoline jsr+rts overhead: %lld cycles (subtracted below)\n", (long long)ovh);
    printf("  clock assumed for the ns column: %.2f MHz (the measured effective\n"
           "  rate of the emulator profile the ground truth came from)\n\n", mhz);

    printf("  %-34s %10s %10s %12s\n", "case", "cycles", "cyc/byte", "ns/byte");

    struct CopyCase { const char *name; uint32_t dstoff, srcoff, len; };
    static const CopyCase copies[] = {
        { "copy 1460 B, both 0 mod 4",   0, 0, 1460 },
        { "copy 1460 B, src 2 mod 4",    0, 2, 1460 },
        { "copy 1460 B, dst 2 mod 4",    2, 0, 1460 },
        { "copy 1460 B, both 2 mod 4",   2, 2, 1460 },
        { "copy  512 B, both 0 mod 4",   0, 0,  512 },
        { "copy  128 B, both 0 mod 4",   0, 0,  128 },
        { "copy   20 B, both 0 mod 4",   0, 0,   20 },
    };

    for (auto &c : copies) {

        uint32_t dst = SCRATCH + 0x10000u + c.dstoff;
        uint32_t src = SCRATCH + c.srcoff;

        int64_t cyc = cycles_of_call(S("_n68k_copy_bytes"), { dst, src, c.len }) - ovh;
        double  cpb = double(cyc) / c.len;

        printf("  %-34s %10lld %10.3f %12.1f\n",
               c.name, (long long)cyc, cpb, cpb * 1000.0 / mhz);
    }

    printf("\n");

    struct SumCase { const char *name; uint32_t off, bytes; };
    static const SumCase sums[] = {
        { "checksum 1460 B",  0, 1460 },
        { "checksum 1024 B",  0, 1024 },
        { "checksum  512 B",  0,  512 },
        { "checksum  296 B",  0,  296 },
        { "checksum  256 B",  0,  256 },
        { "checksum  128 B",  0,  128 },
        { "checksum   40 B",  0,   40 },
        { "checksum   20 B",  0,   20 },
    };

    for (auto &c : sums) {

        uint32_t p = SCRATCH + c.off;
        uint32_t longs = c.bytes / 4;

        /* 20 B is five longwords; the routine takes longword counts. */
        int64_t cyc = cycles_of_call(S("_n68k_sum_longwords"), { p, longs }) - ovh;
        double  cpb = double(cyc) / (longs * 4);

        printf("  %-34s %10lld %10.3f %12.1f\n",
               c.name, (long long)cyc, cpb, cpb * 1000.0 / mhz);
    }
}

/* ------------------------------------------------- determinism, restated -- */

static void suite_determinism(void)
{
    printf("\n-- determinism --------------------------------------------------\n");

    int64_t first = cycles_of_call(S("_n68k_sum_longwords"), { SCRATCH, 365 });
    bool    same  = true;

    for (int i = 0; i < 50; i++) {
        if (cycles_of_call(S("_n68k_sum_longwords"), { SCRATCH, 365 }) != first) same = false;
    }

    printf("  n68k_sum_longwords(1460 B) run 51 times: %s (%lld cycles)\n",
           same ? "identical every time" : "** VARIED **", (long long)first);

    if (!same) failures++;
    checks++;
}

/* --------------------------------------------------------------------- main -- */

static void load_image(const std::string &path)
{
    img = hunk::load(path, CODE_BASE,
                     [](uint32_t a, uint8_t v) { cpu.write8(a, v); },
                     [](uint32_t a) { return cpu.read8(a); });

    for (size_t i = 0; i < img.seg.size(); i++)
        printf("  segment %zu: %u bytes at %08x%s\n", i, img.seg[i].size,
               img.seg[i].base, img.seg[i].code ? " (code)" : "");
    printf("  %zu symbols\n", img.sym.size());


    /* Fill the scratch area with something that is not all zeroes, so a
       checksum that reads past its buffer would show up as a wrong answer. */
    for (uint32_t i = 0; i < 0x40000; i += 4) cpu.poke32(SCRATCH + i, 0x01020304u + i);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <image.exe> <68000|68020>\n", argv[0]);
        return 2;
    }

    bool is020 = (std::string(argv[2]) == "68020");

    printf("AmiNetXDuo -- Moira cycle-timing audit\n");
    printf("=====================================\n\n");
    printf("  model: %s\n", is020 ? "M68020" : "M68000");

    cpu.setModel(is020 ? Model::M68020 : Model::M68000);
    load_image(argv[1]);

    suite_registers(is020);
    suite_memory(is020);
    suite_movem(is020);
    suite_branches(is020);

    if (is020) suite_icache();

    suite_primitives(is020);
    suite_determinism();

    printf("\n-----------------------------------------------------------------\n");
    printf("  %d checks, %d mismatches\n", checks, failures);

    return failures ? 1 : 0;
}
