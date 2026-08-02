/*
 * AmiNetXDuo -- cycle attribution on a host-side 68k core.
 *
 * Leaf-function cycle counts are the easy half.  The half worth having is a
 * flat profile in real 68k cycles across a whole transfer, because the copy
 * and the checksum together are about 20% of one and roughly 78% is
 * unaccounted for somewhere inside NetX Duo's protocol processing, and
 * AmigaOS has no profiler.
 *
 * The design under test, unchanged from the brief:
 *
 *   - at every instruction, take the delta of the core's cycle counter and
 *     accumulate it into a per-PC bucket;
 *   - in the same place, decode jsr/bsr/rts and maintain a shadow stack, so
 *     the same run yields callgrind-style inclusive costs;
 *   - no source changes anywhere, which -finstrument-functions could not
 *     manage without skewing exactly the small leaf functions that matter.
 *
 * What this file establishes is that Moira supports it with no hook at all.
 * `execute()` is public and runs exactly one instruction; `getPC()` is public
 * and, at the point of the call, holds the address of the instruction about to
 * run; the cycle counter is one protected member away.  So the profiler is an
 * outer loop over execute(), which is both simpler than a callback and
 * strictly more capable -- it can stop, inspect and resume anywhere.
 *
 * MOIRA_WILL_EXECUTE is widened by build.sh anyway, so the callback route is
 * measured here too, side by side, to price it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "Moira.h"
#include "hunkload.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

using namespace moira;

static const uint32_t RAM_SIZE  = 16u * 1024u * 1024u;
static const uint32_t CODE_BASE = 0x00002000u;
static const uint32_t SCRATCH   = 0x00100000u;
static const uint32_t STACKTOP  = 0x00080000u;

/* ------------------------------------------------------------------ the CPU -- */

class Sim : public Moira {

public:

    uint8_t *ram;

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
        write16(a, uint16_t(v >> 16)); write16(a + 2, uint16_t(v));
    }

    int64_t cycles() const { return clock; }

    /*
     * The callback route, for pricing against the outer loop.  Upstream's
     * MOIRA_WILL_EXECUTE lists three instructions; build.sh widens it to
     * `true`, which is a recompile of the core, not a runtime switch -- worth
     * knowing before planning around it.  Note the callback is handed the
     * decoded instruction but NOT the PC, so a profiler still has to read
     * getPC0() itself.
     */
    bool     hooked  = false;
    uint64_t hookHits = 0;

    void willExecute(const char *, Instr, Mode, Size, u16) override
    {
        if (hooked) hookHits++;
    }
};

static Sim         cpu;
static hunk::Image img;

/* ---------------------------------------------------------------- profiling -- */

struct FuncStat {
    int64_t  exclusive = 0;
    int64_t  inclusive = 0;
    uint64_t calls     = 0;
    uint64_t instrs    = 0;
};

struct Frame {
    uint32_t func;
    int64_t  entered;       /* cycle count on entry, for the inclusive total */
    uint32_t sp;            /* the stack pointer the matching rts will see   */
};

static std::map<uint32_t, int64_t>     per_pc;
static std::map<std::string, FuncStat> per_func;
static std::map<std::string, std::map<std::string, int64_t>> edges;   /* caller -> callee -> cycles */

static uint64_t total_instrs  = 0;
static int64_t  total_cycles  = 0;
static uint64_t stack_repairs = 0;

/* Close out the top `n` frames, crediting each one's inclusive total to the
   function and to the caller edge. */
static void unwind(std::vector<Frame> &stack, size_t n)
{
    while (n-- && stack.size() > 1) {

        Frame f = stack.back();
        stack.pop_back();

        std::string callee = img.resolve(f.func);
        std::string caller = img.resolve(stack.back().func);
        int64_t     total  = cpu.cycles() - f.entered;

        /* Recursion: only the outermost activation may add to the inclusive
           total, or a self-recursive routine counts its own body once per
           level. */
        bool nested = false;
        for (auto &g : stack) if (img.resolve(g.func) == callee) nested = true;
        if (!nested) per_func[callee].inclusive += total;

        edges[caller][callee] += total;
    }
}

/* jsr and bsr push a return address; rts, rtr and rte pop one.  Nothing else
   in the 68000 set moves the call depth, and a jmp tail call deliberately does
   not -- it belongs to the frame it jumped out of, which is what callgrind
   would say too. */
static bool is_call(uint16_t op)
{
    return (op & 0xFFC0) == 0x4E80 ||           /* jsr <ea>  */
           (op & 0xFF00) == 0x6100;             /* bsr       */
}

static bool is_return(uint16_t op)
{
    return op == 0x4E75 || op == 0x4E77 || op == 0x4E73;    /* rts, rtr, rte */
}

/*
 * One profiled run.  Stops when the PC reaches `until`, which is the
 * trampoline's illegal instruction.
 */
static void profile(uint32_t until, bool use_hook)
{
    std::vector<Frame> stack;
    std::string        cur = img.resolve(cpu.getPC());

    stack.push_back({ cpu.getPC(), cpu.cycles(), cpu.getSP() });

    cpu.hooked = use_hook;

    while (cpu.getPC() != until) {

        uint32_t pc  = cpu.getPC();
        uint16_t op  = cpu.read16(pc);
        int64_t  c0  = cpu.cycles();

        cpu.execute();

        int64_t spent = cpu.cycles() - c0;

        per_pc[pc]  += spent;
        total_cycles += spent;
        total_instrs++;

        const char *fn = img.resolve(pc);
        auto &fs = per_func[fn];
        fs.exclusive += spent;
        fs.instrs++;

        if (is_call(op)) {

            uint32_t callee = cpu.getPC();
            stack.push_back({ callee, cpu.cycles(), cpu.getSP() + 4 });
            per_func[img.resolve(callee)].calls++;

        } else if (is_return(op) && stack.size() > 1) {

            unwind(stack, 1);

        } else if (stack.size() > 1 && cpu.getSP() > stack.back().sp) {

            /*
             * The stack pointer moved above the frame that is supposed to own
             * it without an rts having run.  On this workload that never
             * happens; on ThreadX it will, every time the scheduler swaps a
             * task's stack out, and a shadow stack that only trusts rts would
             * drift further out of step with every switch until the profile
             * was fiction.  Unwinding to wherever the stack pointer now says
             * we are costs nothing when it is not needed and is the difference
             * between a usable profile and a silently wrong one when it is.
             */
            size_t n = 0;
            while (stack.size() - n > 1 && cpu.getSP() > stack[stack.size() - 1 - n].sp) n++;
            unwind(stack, n);
            stack_repairs++;
        }
    }

    cpu.hooked = false;
    (void)cur;
}

/* ------------------------------------------------------------------ driving -- */

static void seed(void)
{
    for (int i = 0; i < 8; i++) cpu.setD(i, 0u);
    for (int i = 0; i < 7; i++) cpu.setA(i, SCRATCH);
}

static void enter(uint32_t target, const std::vector<uint32_t> &args)
{
    uint32_t sp = STACKTOP - uint32_t(args.size()) * 4u;

    for (size_t i = 0; i < args.size(); i++) cpu.poke32(sp + uint32_t(i) * 4u, args[i]);

    cpu.poke32(img["_tramp_target"], target);
    cpu.poke32(0, sp);
    cpu.poke32(4, img["_tramp"]);
    cpu.reset();
    seed();
}

/* ----------------------------------------------------------------- reporting -- */

static void report(void)
{
    printf("\n-- flat profile, exclusive 68k cycles ---------------------------\n\n");
    printf("  %-28s %12s %7s %12s %10s %8s\n",
           "function", "excl cycles", "%", "incl cycles", "instrs", "calls");

    std::vector<std::pair<std::string, FuncStat>> v(per_func.begin(), per_func.end());
    std::sort(v.begin(), v.end(),
              [](auto &a, auto &b) { return a.second.exclusive > b.second.exclusive; });

    for (auto &e : v) {
        printf("  %-28s %12lld %6.2f%% %12lld %10llu %8llu\n",
               e.first.c_str(), (long long)e.second.exclusive,
               100.0 * double(e.second.exclusive) / double(total_cycles),
               (long long)e.second.inclusive,
               (unsigned long long)e.second.instrs,
               (unsigned long long)e.second.calls);
    }

    printf("\n  total %lld cycles over %llu instructions, %llu stack repairs\n",
           (long long)total_cycles, (unsigned long long)total_instrs,
           (unsigned long long)stack_repairs);

    printf("\n-- call edges, inclusive ----------------------------------------\n\n");
    for (auto &c : edges) {
        for (auto &e : c.second) {
            printf("  %-28s -> %-28s %12lld\n",
                   c.first.c_str(), e.first.c_str(), (long long)e.second);
        }
    }

    printf("\n-- the ten hottest instructions ---------------------------------\n\n");
    std::vector<std::pair<uint32_t, int64_t>> p(per_pc.begin(), per_pc.end());
    std::sort(p.begin(), p.end(), [](auto &a, auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < p.size() && i < 10; i++) {
        printf("  %08x  %-24s %12lld\n",
               p[i].first, img.resolve(p[i].first), (long long)p[i].second);
    }
}

/* --------------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <68000|68020> [reps]\n", argv[0]);
        return 2;
    }

    bool     is020 = (std::string(argv[2]) == "68020");
    uint32_t reps  = (argc > 3) ? uint32_t(strtoul(argv[3], nullptr, 0)) : 200;

    cpu.setModel(is020 ? Model::M68020 : Model::M68000);

    try {
        img = hunk::load(argv[1], CODE_BASE,
                         [](uint32_t a, uint8_t v) { cpu.write8(a, v); },
                         [](uint32_t a) { return cpu.read8(a); });
    } catch (const std::exception &e) {
        fprintf(stderr, "%s: %s\n", argv[1], e.what());
        return 1;
    }

    printf("AmiNetXDuo -- Moira cycle attribution prototype\n");
    printf("==============================================\n\n");
    printf("  model %s, image %s\n", is020 ? "M68020" : "M68000", argv[1]);
    for (size_t i = 0; i < img.seg.size(); i++)
        printf("  segment %zu: %u bytes at %08x%s\n", i, img.seg[i].size, img.seg[i].base,
               img.seg[i].code ? " (code)" : "");
    /* Statics, from the link map plus per-object nm.  Without them a file-local
       function's cycles land on whichever global precedes it and the profile
       lies quietly; wl_fold is the one in this workload. */
    {
        std::string lp(argv[1]);
        size_t dot = lp.rfind('.');
        if (dot != std::string::npos) lp = lp.substr(0, dot);
        lp += ".locals";

        FILE *lf = fopen(lp.c_str(), "r");
        size_t n = 0;
        if (lf) {
            char name[256];
            unsigned addr;
            while (fscanf(lf, "%x %255s", &addr, name) == 2) {
                img.sym[name] = CODE_BASE + addr;
                img.rsym.emplace(CODE_BASE + addr, name);
                n++;
            }
            fclose(lf);
        }
        printf("  %zu symbols (%zu file-local, from %s)\n", img.sym.size(), n, lp.c_str());
    }

    for (uint32_t i = 0; i < 0x40000; i += 4) cpu.poke32(SCRATCH + i, 0x01020304u + i);

    /* The workload: a C driver, compiled by the same cross toolchain as the
       Amiga build, calling the two shipped assembly primitives.  A call tree
       three deep with both languages in it, which is the shape a NetX Duo
       profile would have. */
    enter(img["_wl_run"], { reps, SCRATCH, SCRATCH + 0x20000u });

    auto t0 = std::chrono::steady_clock::now();
    profile(img["_tramp_end"], false);
    auto t1 = std::chrono::steady_clock::now();

    report();

    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n-- what it costs to run --------------------------------------\n\n");
    printf("  host wall time            %.3f s\n", secs);
    printf("  instructions per second   %.2f M\n", double(total_instrs) / secs / 1e6);
    printf("  emulated cycles per sec   %.2f M\n", double(total_cycles) / secs / 1e6);
    printf("  emulated-to-real ratio    %.2fx %s a %s\n",
           double(total_cycles) / secs / (is020 ? 14.19e6 : 7.09e6),
           (double(total_cycles) / secs / (is020 ? 14.19e6 : 7.09e6)) > 1 ? "faster than" : "slower than",
           is020 ? "14 MHz 68020" : "7 MHz 68000");

    /* Price the callback route against the outer loop: same run, hook on. */
    per_pc.clear(); per_func.clear(); edges.clear();
    total_instrs = 0; total_cycles = 0;

    enter(img["_wl_run"], { reps, SCRATCH, SCRATCH + 0x20000u });
    auto t2 = std::chrono::steady_clock::now();
    profile(img["_tramp_end"], true);
    auto t3 = std::chrono::steady_clock::now();

    printf("\n  with willExecute() also firing: %.3f s (%.1f%% slower),"
           " %llu callbacks\n",
           std::chrono::duration<double>(t3 - t2).count(),
           100.0 * (std::chrono::duration<double>(t3 - t2).count() / secs - 1.0),
           (unsigned long long)cpu.hookHits);

    return 0;
}
