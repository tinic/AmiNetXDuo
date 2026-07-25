/*
 * AmiNetXDuo -- BPF program validator.
 *
 * Runs at BIOCSETF time, on a program that came from an application over an
 * LVO, on a machine with no memory protection. Strict by design: anything the
 * interpreter in bpf_filter.c does not implement is refused here rather than
 * left to reject packets silently at run time. 4.4BSD's bpf_validate is more
 * permissive -- it lets several undefined encodings through on the grounds
 * that the interpreter treats them as no-ops -- which is a bad trade when the
 * cost of being wrong is the whole machine.
 *
 * Rejected:
 *   - an empty program, or one longer than BPF_MAXINSNS;
 *   - any jump that is backward, or that lands at or past the end (only
 *     forward jumps exist in the encoding, so this also guarantees the
 *     interpreter terminates);
 *   - a scratch-memory index at or past BPF_MEMWORDS;
 *   - division by a zero constant;
 *   - a load, store, ALU, jump, return or misc opcode outside the defined set,
 *     including size/mode combinations that are individually defined but not
 *     legal together (BPF_LDX|BPF_ABS, BPF_LD|BPF_MSH, a 16-bit BPF_MSH, ...);
 *   - a program whose last instruction is not a BPF_RET.
 *
 * No AmigaOS calls here.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

LONG ami_bpf_validate(const struct bpf_insn *insns, ULONG count)
{
    ULONG i;

    if (insns == NULL)
        return -1;

    if (count == 0 || count > (ULONG)BPF_MAXINSNS)
        return -1;

    for (i = 0; i < count; i++)
    {
        const struct bpf_insn *p    = &insns[i];
        UWORD                  code = p->code;
        ULONG                  k    = (ULONG)p->k;

        /* Instructions that may still be reached from here, i.e. how far a
           forward jump at insn i is allowed to go. */
        ULONG room = count - (i + 1);

        switch (BPF_CLASS(code))
        {
        case BPF_LD:
            switch (BPF_MODE(code))
            {
            case BPF_ABS:
            case BPF_IND:
                if (ami_bpf_size_bytes(code) == 0)
                    return -1;
                break;

            case BPF_IMM:
            case BPF_LEN:
                if (BPF_SIZE(code) != BPF_W)
                    return -1;
                break;

            case BPF_MEM:
                if (BPF_SIZE(code) != BPF_W)
                    return -1;
                if (k >= (ULONG)BPF_MEMWORDS)
                    return -1;
                break;

            default:
                return -1;
            }
            break;

        case BPF_LDX:
            switch (BPF_MODE(code))
            {
            case BPF_IMM:
            case BPF_LEN:
                if (BPF_SIZE(code) != BPF_W)
                    return -1;
                break;

            case BPF_MEM:
                if (BPF_SIZE(code) != BPF_W)
                    return -1;
                if (k >= (ULONG)BPF_MEMWORDS)
                    return -1;
                break;

            case BPF_MSH:
                /* Only ever the byte form: X = (p[k] & 0xf) << 2. */
                if (BPF_SIZE(code) != BPF_B)
                    return -1;
                break;

            default:
                return -1;
            }
            break;

        case BPF_ST:
        case BPF_STX:
            /* No size or mode bits are defined for stores. */
            if (BPF_SIZE(code) != 0 || BPF_MODE(code) != 0)
                return -1;
            if (k >= (ULONG)BPF_MEMWORDS)
                return -1;
            break;

        case BPF_ALU:
            switch (BPF_OP(code))
            {
            case BPF_ADD:
            case BPF_SUB:
            case BPF_MUL:
            case BPF_OR:
            case BPF_AND:
            case BPF_LSH:
            case BPF_RSH:
                break;

            case BPF_DIV:
                /* A variable zero divisor can only be caught at run time; a
                   constant one is a bug in the program. */
                if (BPF_SRC(code) == BPF_K && k == 0)
                    return -1;
                break;

            case BPF_NEG:
                break;

            default:
                return -1;
            }
            break;

        case BPF_JMP:
            switch (BPF_OP(code))
            {
            case BPF_JA:
                /* A negative k becomes a huge unsigned one and is caught by
                   the same test -- backward jumps do not exist. */
                if (k >= room)
                    return -1;
                break;

            case BPF_JEQ:
            case BPF_JGT:
            case BPF_JGE:
            case BPF_JSET:
                if ((ULONG)p->jt >= room || (ULONG)p->jf >= room)
                    return -1;
                break;

            default:
                return -1;
            }
            break;

        case BPF_RET:
            switch (BPF_RVAL(code))
            {
            case BPF_K:
            case BPF_A:
                break;
            default:
                return -1;      /* BPF_RET|BPF_X: not implemented */
            }
            break;

        case BPF_MISC:
            switch (BPF_MISCOP(code))
            {
            case BPF_TAX:
            case BPF_TXA:
                break;
            default:
                return -1;
            }
            break;

        default:
            return -1;
        }
    }

    /* Falling off the end is a rejection at run time; make it a load-time one. */
    if (BPF_CLASS(insns[count - 1].code) != BPF_RET)
        return -1;

    return 0;
}
