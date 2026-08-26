/*
 * AmiNetXDuo, BPF program validator.
 *
 * Runs at BIOCSETF time. Stricter than 4.4BSD bpf_validate: anything the
 * interpreter in bpf_filter.c does not implement is refused here rather than
 * left to reject packets silently at run time. No AmigaOS calls here.
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

        /* Instructions that can still be reached from here, that is, how far
           a forward jump at insn i can go. */
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
                /* A variable zero divisor can only be caught at run time. A
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
                   the same test. Backward jumps do not exist. */
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

    /* A fall off the end is a rejection at run time. Make it a load-time one. */
    if (BPF_CLASS(insns[count - 1].code) != BPF_RET)
        return -1;

    return 0;
}
