/*
 * AmiNetXDuo, the BPF filter interpreter.
 *
 * A classic Berkeley packet filter VM: accumulator A, index register X,
 * BPF_MEMWORDS words of scratch, eight instruction classes, and a return value
 * that is the number of bytes of the packet to keep.
 *
 * The interpreter is total: no input can make it trap, read outside the
 * packet, divide by zero, shift by more than 31, loop forever, or step outside
 * the instruction array. This holds for an invalid program and for an invalid
 * packet. Each of these cases is checked, and the packet is rejected:
 *
 *   - packet reads are bounds-checked against the view, in arithmetic that
 *     cannot wrap (`off > caplen - size` after `size <= caplen` is proved)
 *   - X + k for indexed loads is checked for wraparound
 *   - every jump is checked against the remaining instruction count, and only
 *     forward jumps exist in the encoding, so pc strictly increases and the
 *     loop always terminates
 *   - scratch indices are checked against BPF_MEMWORDS
 *   - unknown encodings reject instead of a fall-through
 *
 * ami_bpf_validate() still runs at BIOCSETF time, so a bad program is rejected
 * at load time and does not silently drop every packet afterwards. The
 * interpreter does not depend on that validation.
 *
 * Packet bytes are assembled big-endian, the order that BPF specifies (network
 * order). On 68k a direct load gives the same order.
 *
 * No AmigaOS calls here.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

/* ----------------------------------------------------------------- views */

VOID ami_bpf_view_linear(AmiBpfView *view, const UBYTE *frame, ULONG len)
{
    view->wirelen = len;
    view->caplen  = len;
    view->nsegs   = 1;
    view->seg[0].base = frame;
    view->seg[0].len  = len;
}

LONG ami_bpf_view_add(AmiBpfView *view, const UBYTE *base, ULONG len)
{
    if (view->nsegs >= AMI_BPF_MAX_SEGS)
        return -1;

    if (len == 0)
        return 0;

    view->seg[view->nsegs].base = base;
    view->seg[view->nsegs].len  = len;
    view->nsegs++;
    view->caplen  += len;
    view->wirelen += len;

    return 0;
}

/*
 * Point at offset `off` and say how many contiguous bytes follow it. NULL when
 * off is past the end of the view.
 */
static const UBYTE *ami_bpf_view_at(const AmiBpfView *view, ULONG off,
                                    ULONG *avail)
{
    ULONG base = 0;
    UWORD i;

    for (i = 0; i < view->nsegs; i++)
    {
        ULONG len = view->seg[i].len;

        if (off < base + len)
        {
            *avail = base + len - off;
            return view->seg[i].base + (off - base);
        }

        base += len;
    }

    return NULL;
}

/* Load 1, 2 or 4 bytes big-endian. FALSE means "out of range, reject". */
static BOOL ami_bpf_load(const AmiBpfView *view, ULONG off, UWORD size,
                         ULONG *out)
{
    const UBYTE *p;
    ULONG        avail;
    ULONG        value;
    UWORD        i;

    if (size == 0)
        return FALSE;

    /* Written this way so it cannot overflow for any off, including ~0UL. */
    if ((ULONG)size > view->caplen || off > view->caplen - (ULONG)size)
        return FALSE;

    p = ami_bpf_view_at(view, off, &avail);
    if (p == NULL)
        return FALSE;

    if (avail >= (ULONG)size)
    {
        /* The whole access is inside one segment: the common case by far. */
        value = 0;
        for (i = 0; i < size; i++)
            value = (value << 8) | (ULONG)p[i];
        *out = value;
        return TRUE;
    }

    /* Straddles a segment boundary, only possible on the transmit path,
       between the synthesised link header and the packet payload. */
    value = 0;
    for (i = 0; i < size; i++)
    {
        p = ami_bpf_view_at(view, off + i, &avail);
        if (p == NULL)
            return FALSE;
        value = (value << 8) | (ULONG)*p;
    }

    *out = value;

    return TRUE;
}

UWORD ami_bpf_size_bytes(UWORD code)
{
    switch (BPF_SIZE(code))
    {
    case BPF_W:  return 4;
    case BPF_H:  return 2;
    case BPF_B:  return 1;
    default:     return 0;      /* 0x18 is not a defined size */
    }
}

/* ------------------------------------------------------------ interpreter */

ULONG ami_bpf_filter_view(const struct bpf_insn *insns, ULONG count,
                          const AmiBpfView *view)
{
    ULONG mem[BPF_MEMWORDS];
    ULONG A  = 0;
    ULONG X  = 0;
    ULONG pc = 0;
    UWORD i;

    /* No program means no filtering: keep everything. Same as 4.4BSD. */
    if (insns == NULL || count == 0)
        return (ULONG)-1;

    if (view == NULL)
        return 0;

    for (i = 0; i < BPF_MEMWORDS; i++)
        mem[i] = 0;

    while (pc < count)
    {
        const struct bpf_insn *p    = &insns[pc];
        UWORD                  code = p->code;
        ULONG                  k    = (ULONG)p->k;
        ULONG                  tmp;

        pc++;

        switch (BPF_CLASS(code))
        {
        case BPF_LD:
            switch (BPF_MODE(code))
            {
            case BPF_IMM:
                A = k;
                break;

            case BPF_LEN:
                A = view->wirelen;
                break;

            case BPF_MEM:
                if (k >= BPF_MEMWORDS)
                    return 0;
                A = mem[k];
                break;

            case BPF_ABS:
                if (!ami_bpf_load(view, k, ami_bpf_size_bytes(code), &A))
                    return 0;
                break;

            case BPF_IND:
                tmp = X + k;
                if (tmp < X)                /* wrapped: certainly out of range */
                    return 0;
                if (!ami_bpf_load(view, tmp, ami_bpf_size_bytes(code), &A))
                    return 0;
                break;

            default:
                return 0;
            }
            break;

        case BPF_LDX:
            switch (BPF_MODE(code))
            {
            case BPF_IMM:
                X = k;
                break;

            case BPF_LEN:
                X = view->wirelen;
                break;

            case BPF_MEM:
                if (k >= BPF_MEMWORDS)
                    return 0;
                X = mem[k];
                break;

            case BPF_MSH:
                /* The IP header length idiom: X = (p[k] & 0xf) << 2. */
                if (BPF_SIZE(code) != BPF_B)
                    return 0;
                if (!ami_bpf_load(view, k, 1, &tmp))
                    return 0;
                X = (tmp & 0x0FUL) << 2;
                break;

            default:
                return 0;
            }
            break;

        case BPF_ST:
            if (k >= BPF_MEMWORDS)
                return 0;
            mem[k] = A;
            break;

        case BPF_STX:
            if (k >= BPF_MEMWORDS)
                return 0;
            mem[k] = X;
            break;

        case BPF_ALU:
        {
            ULONG operand = (BPF_SRC(code) == BPF_X) ? X : k;

            switch (BPF_OP(code))
            {
            case BPF_ADD: A += operand; break;
            case BPF_SUB: A -= operand; break;
            case BPF_MUL: A *= operand; break;

            case BPF_DIV:
                if (operand == 0)
                    return 0;
                A /= operand;
                break;

            case BPF_OR:  A |= operand; break;
            case BPF_AND: A &= operand; break;

            /* C leaves a shift of the width or more undefined. BPF programs
               are not trusted to stay under 32. */
            case BPF_LSH: A = (operand > 31) ? 0 : (A << operand); break;
            case BPF_RSH: A = (operand > 31) ? 0 : (A >> operand); break;

            case BPF_NEG: A = (ULONG)(0UL - A); break;

            default:
                return 0;
            }
            break;
        }

        case BPF_JMP:
        {
            ULONG room = count - pc;    /* pc already stepped past this insn */
            ULONG jump;

            if (BPF_OP(code) == BPF_JA)
            {
                if (k > room)
                    return 0;
                pc += k;
                break;
            }

            {
                ULONG operand = (BPF_SRC(code) == BPF_X) ? X : k;
                BOOL  taken;

                switch (BPF_OP(code))
                {
                case BPF_JGT:  taken = (A >  operand) ? TRUE : FALSE; break;
                case BPF_JGE:  taken = (A >= operand) ? TRUE : FALSE; break;
                case BPF_JEQ:  taken = (A == operand) ? TRUE : FALSE; break;
                case BPF_JSET: taken = ((A & operand) != 0) ? TRUE : FALSE; break;
                default:       return 0;
                }

                jump = taken ? (ULONG)p->jt : (ULONG)p->jf;
            }

            if (jump > room)
                return 0;
            pc += jump;
            break;
        }

        case BPF_RET:
            switch (BPF_RVAL(code))
            {
            case BPF_K:  return k;
            case BPF_A:  return A;
            default:     return 0;      /* BPF_RET|BPF_X is not implemented */
            }

        case BPF_MISC:
            switch (BPF_MISCOP(code))
            {
            case BPF_TAX: X = A; break;
            case BPF_TXA: A = X; break;
            default:      return 0;
            }
            break;

        default:
            return 0;
        }
    }

    /* Ran off the end with no return: reject, as 4.4BSD does. */
    return 0;
}

ULONG ami_bpf_filter(const struct bpf_insn *insns, ULONG count,
                     const UBYTE *frame, ULONG wirelen, ULONG caplen)
{
    AmiBpfView view;

    view.wirelen      = wirelen;
    view.caplen       = caplen;
    view.nsegs        = 1;
    view.seg[0].base  = frame;
    view.seg[0].len   = caplen;

    return ami_bpf_filter_view(insns, count, &view);
}
