/*
 * AmiNetXDuo, host fuzz driver for the BPF filter VM, the validator and the
 * capture ring.
 *
 * Same shim arrangement as src/bpf/test/test_bpf.c (-DAMI_BPF_REPLICA plus the
 * <exec/types.h> shim under src/config/test/shim), driven with random filter
 * programs and random frames instead of the handful of libpcap programs the
 * unit test uses.  Two trust boundaries meet in this code and both are
 * covered:
 *
 *   the wire      , ami_bpf_tap_rx() runs the filter over untrusted frame
 *                     bytes on the IP thread;
 *   the library   , bpf_setf() runs a caller-chosen program, and the
 *                     interpreter is documented to be total whether or not
 *                     ami_bpf_validate() accepted it, so both the validated
 *                     and the unvalidated path are exercised here.
 *
 * The channel is also driven through BIOCSBLEN / BIOCSETF / read / rotate so
 * the record framing sees short buffers and oversized captures.
 *
 * Build: see fuzz.sh.  Usage: fuzz_bpf -r SEED COUNT
 */

#include "bpf_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel ownership is a library base on the Amiga; here it is just a token. */
static char t_bpf_base_a;
#define T_BPF_OWNER  ((APTR)&t_bpf_base_a)

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;

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
    (void)level;
    (void)fmt;
}

VOID ami_bpf_lock(VOID)   { }
VOID ami_bpf_unlock(VOID) { }

VOID ami_bpf_time_init(VOID) { }

VOID ami_bpf_now(ULONG *sec, ULONG *usec)
{
    *sec  = 1000;
    *usec = 500000;
}

static APTR stub_task = (APTR)"task";

APTR ami_bpf_current_task(VOID) { return stub_task; }

VOID ami_bpf_sleep(ULONG ticks) { (VOID)ticks; }
ULONG ami_bpf_signals_set(ULONG mask) { (VOID)mask; return 0UL; }

VOID ami_bpf_notify(APTR task, ULONG mask)
{
    (void)task;
    (void)mask;
}

/* ---------------------------------------------------------------- random */

static unsigned long fz_state;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(fz_state >> 33);
}

static unsigned fz_below(unsigned n)
{
    return (n == 0) ? 0 : (fz_rand() % n);
}

/*
 * A random instruction, biased towards the encodings that actually touch
 * memory: uniform 16-bit opcodes are almost all "unknown class", which the
 * interpreter rejects in one line and which therefore tests nothing.
 */
static UWORD fz_opcode(void)
{
    static const UWORD interesting[] =
    {
        BPF_LD  | BPF_W | BPF_ABS, BPF_LD  | BPF_H | BPF_ABS,
        BPF_LD  | BPF_B | BPF_ABS, BPF_LD  | BPF_W | BPF_IND,
        BPF_LD  | BPF_H | BPF_IND, BPF_LD  | BPF_B | BPF_IND,
        BPF_LD  | BPF_W | BPF_LEN, BPF_LD  | BPF_W | BPF_IMM,
        BPF_LD  | BPF_W | BPF_MEM,
        BPF_LDX | BPF_W | BPF_IMM, BPF_LDX | BPF_W | BPF_MEM,
        BPF_LDX | BPF_W | BPF_LEN, BPF_LDX | BPF_B | BPF_MSH,
        BPF_LDX | BPF_W | BPF_MSH,
        BPF_ST, BPF_STX,
        BPF_ALU | BPF_ADD | BPF_K, BPF_ALU | BPF_SUB | BPF_X,
        BPF_ALU | BPF_MUL | BPF_K, BPF_ALU | BPF_DIV | BPF_K,
        BPF_ALU | BPF_DIV | BPF_X, BPF_ALU | BPF_OR  | BPF_K,
        BPF_ALU | BPF_AND | BPF_X, BPF_ALU | BPF_LSH | BPF_K,
        BPF_ALU | BPF_RSH | BPF_X, BPF_ALU | BPF_NEG,
        BPF_JMP | BPF_JA,          BPF_JMP | BPF_JEQ | BPF_K,
        BPF_JMP | BPF_JGT | BPF_X, BPF_JMP | BPF_JGE | BPF_K,
        BPF_JMP | BPF_JSET| BPF_X,
        BPF_RET | BPF_K,           BPF_RET | BPF_A,
        BPF_MISC| BPF_TAX,         BPF_MISC| BPF_TXA
    };

    /* One case in eight is a genuinely arbitrary encoding, so the "unknown"
       branches are reached as well. */
    if (fz_below(8) == 0)
        return (UWORD)fz_rand();

    return interesting[fz_below((unsigned)(sizeof(interesting) /
                                           sizeof(interesting[0])))];
}

static ULONG fz_k(void)
{
    switch (fz_below(6))
    {
    case 0:  return 0;
    case 1:  return 0xFFFFFFFFUL;
    case 2:  return fz_below(80);              /* inside a small frame     */
    case 3:  return 0xFFFFFFFEUL;              /* wraps X + k              */
    case 4:  return (ULONG)fz_below(BPF_MEMWORDS + 2);
    default: return (ULONG)fz_rand();
    }
}

#define FZ_MAX_INSNS    64
#define FZ_MAX_FRAME    256

int main(int argc, char **argv)
{
    struct bpf_insn prog[FZ_MAX_INSNS];
    UBYTE           frame[FZ_MAX_FRAME];
    struct bpf_program bp;
    unsigned long   seed  = 1;
    unsigned long   count = 10000;
    unsigned long   n;
    static APTR     cookie = (APTR)"fuzz-iface";
    int             i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            seed  = strtoul(argv[++i], NULL, 0);
            count = strtoul(argv[++i], NULL, 0);
        }
    }

    fz_state = seed;

    for (n = 0; n < count; n++)
    {
        ULONG insns = (ULONG)fz_below(FZ_MAX_INSNS) + 1;
        ULONG flen  = (ULONG)fz_below(FZ_MAX_FRAME) + 1;
        ULONG blen;
        ULONG j;
        LONG  chan;

        for (j = 0; j < insns; j++)
        {
            prog[j].code = fz_opcode();
            prog[j].jt   = (UBYTE)fz_rand();
            prog[j].jf   = (UBYTE)fz_rand();
            prog[j].k    = fz_k();
        }

        for (j = 0; j < flen; j++)
            frame[j] = (UBYTE)fz_rand();

        /*
         * The interpreter, unvalidated.  bpf_filter.c's header states that no
         * program and no packet can make it read outside the view, so it is
         * run on the raw program before the validator has seen it.
         */
        (void)ami_bpf_filter(prog, insns, frame, flen, flen);

        /* A caplen shorter than the frame, and a wirelen longer than both,
           the shapes ami_bpf_capture() produces from a truncated capture. */
        (void)ami_bpf_filter(prog, insns, frame, flen + 1000, flen / 2 + 1);

        /* A scatter view, which is the transmit tap's shape. */
        {
            AmiBpfView view;
            ULONG      cut = flen / 2;

            ami_bpf_view_linear(&view, frame, cut);
            (void)ami_bpf_view_add(&view, frame + cut, flen - cut);
            (void)ami_bpf_filter_view(prog, insns, &view);
        }

        /* The validator, and then the same program through the channel. */
        bp.bf_len   = (unsigned int)insns;
        bp.bf_insns = prog;

        ami_bpf_init();
        (void)ami_bpf_attach_interface("fz0", cookie, DLT_EN10MB, 1500, NULL);

        chan = ami_bpf_open(T_BPF_OWNER, 0);
        if (chan >= 0)
        {
            blen = (ULONG)fz_below(4096) + 1;

            (void)ami_bpf_ioctl(T_BPF_OWNER, chan, BIOCSBLEN, &blen);
            (void)ami_bpf_ioctl(T_BPF_OWNER, chan, BIOCSETIF, "fz0");
            (void)ami_bpf_ioctl(T_BPF_OWNER, chan, BIOCSETF, &bp);

            /* Several frames, so the ring rotates and the reader path runs. */
            for (j = 0; j < 8; j++)
                ami_bpf_tap_rx(cookie, frame, flen);

            {
                static UBYTE out[8192];

                (void)ami_bpf_read(T_BPF_OWNER, chan, out,
                                   (LONG)fz_below((unsigned)sizeof(out)));
            }

            (void)ami_bpf_close(T_BPF_OWNER, chan);
        }

        ami_bpf_detach_interface(cookie);
        ami_bpf_cleanup();

        if (ami_alloc_count() != 0)
        {
            printf("fuzz_bpf: %lu allocation(s) leaked at case %lu\n",
                   (unsigned long)ami_alloc_count(), n);
            return 1;
        }
    }

    printf("fuzz_bpf: %lu case(s) from seed %lu, clean\n", count, seed);

    return 0;
}
