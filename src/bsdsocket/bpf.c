/*
 * bsdsocket.library -- the eight bpf_* LVOs.
 *
 * Thin wrappers: the work lives in src/bpf/, and what is here is the m68k
 * register convention, the SocketBase argument every vector carries, and the
 * errno the caller reads back.
 *
 * The slots exist whatever AMINETXDUO_BPF says. A build without it compiles
 * eight ENOSYS bodies, so the vector table has the same shape either way and a
 * caller gets a documented failure instead of a jump into a slot that means
 * something else in the next build.
 *
 * bpf_set_notify_mask takes (d1, d0) -- channel in d1, mask in d0 -- the
 * reverse of every other call in the group, including bpf_set_interrupt_mask.
 * Both pragmas/bsdsocket_pragmas.h and the .fd agree, so it is real.
 * tools/gen_vectors.py reads the order from the pragma, so the declaration in
 * bsdsocket_vectors.h follows automatically.
 *
 * No ThreadX bracket here, unlike every other vector that touches the stack:
 * those go through bsd_nx_enter() because NetX Duo will suspend the caller.
 * src/bpf/ never calls NetX Duo, never allocates from the packet pool and
 * never blocks. It guards its own table with Forbid()/Permit(), which suits a
 * structure shared with the SANA-II reader Tasks and the IP thread. An
 * adoption bracket would cost more than the capture read itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#ifdef AMINETXDUO_BPF
#include "aminetxduo/bpf.h"
#endif

#ifdef AMINETXDUO_BPF

/*
 * src/bpf/ returns a negative AMI_BPF_* status; the LVOs report -1 and set the
 * errno the autodoc names. -1 is AMI_BPF_EINVAL, so an unannotated failure
 * still lands on EINVAL. An out-of-range status would index past the table, so
 * the range is checked rather than assumed.
 */
static const LONG bsd_bpf_errno[] = {
    0,                  /* unused: 0 is success  */
    AMI_EINVAL,         /* AMI_BPF_EINVAL    */
    AMI_ENXIO,          /* AMI_BPF_ENXIO     */
    AMI_EPERM,          /* AMI_BPF_EPERM     */
    AMI_EBUSY,          /* AMI_BPF_EBUSY     */
    AMI_EINTR,          /* AMI_BPF_EINTR     */
    AMI_EIO,            /* AMI_BPF_EIO       */
    AMI_ENOBUFS,        /* AMI_BPF_ENOBUFS   */
    AMI_EMSGSIZE        /* AMI_BPF_EMSGSIZE  */
};

static LONG bsd_bpf_result(struct AmiSocketBase *SocketBase, LONG status)
{
    LONG index;

    if (status >= 0)
        return status;

    index = -status;
    if (index >= (LONG)(sizeof(bsd_bpf_errno) / sizeof(bsd_bpf_errno[0])))
        index = -AMI_BPF_EINVAL;

    return bsd_fail(SocketBase, bsd_bpf_errno[index]);
}

/*
 * The autodoc: the channel "will be associated with the library base ... It
 * will be automatically closed when the library is closed". library.c calls
 * this from bsd_child_destroy(); it frees memory and takes Forbid()/Permit(),
 * and does not block.
 */
VOID bsd_bpf_close_all(struct AmiSocketBase *SocketBase)
{
    ami_bpf_close_owner((APTR)SocketBase);
}

LONG bsd_bpf_open(register LONG channel __asm("d0"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_open((APTR)SocketBase, channel));
}

LONG bsd_bpf_close(register LONG channel __asm("d0"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_close((APTR)SocketBase, channel));
}

LONG bsd_bpf_read(register LONG channel __asm("d0"),
                  register APTR buffer __asm("a0"),
                  register LONG len __asm("d1"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_read((APTR)SocketBase, channel, buffer,
                                       len));
}

LONG bsd_bpf_write(register LONG channel __asm("d0"),
                   register APTR buffer __asm("a0"),
                   register LONG len __asm("d1"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_write((APTR)SocketBase, channel, buffer,
                                        len));
}

LONG bsd_bpf_set_notify_mask(register LONG channel __asm("d1"),
                             register ULONG signal_mask __asm("d0"),
                             register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_set_notify_mask((APTR)SocketBase, channel,
                                                  signal_mask));
}

LONG bsd_bpf_set_interrupt_mask(register LONG channel __asm("d0"),
                                register ULONG signal_mask __asm("d1"),
                                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_set_interrupt_mask((APTR)SocketBase, channel,
                                                     signal_mask));
}

LONG bsd_bpf_ioctl(register LONG channel __asm("d0"),
                   register ULONG command __asm("d1"),
                   register APTR buffer __asm("a0"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_ioctl((APTR)SocketBase, channel, command,
                                        buffer));
}

LONG bsd_bpf_data_waiting(register LONG channel __asm("d0"),
                          register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_bpf_result(SocketBase,
                          ami_bpf_data_waiting((APTR)SocketBase, channel));
}

#else /* !AMINETXDUO_BPF */

LONG bsd_bpf_open(register LONG channel __asm("d0"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_close(register LONG channel __asm("d0"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_read(register LONG channel __asm("d0"),
                  register APTR buffer __asm("a0"),
                  register LONG len __asm("d1"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel; (VOID)buffer; (VOID)len;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_write(register LONG channel __asm("d0"),
                   register APTR buffer __asm("a0"),
                   register LONG len __asm("d1"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel; (VOID)buffer; (VOID)len;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_set_notify_mask(register LONG channel __asm("d1"),
                             register ULONG signal_mask __asm("d0"),
                             register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel; (VOID)signal_mask;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_set_interrupt_mask(register LONG channel __asm("d0"),
                                register ULONG signal_mask __asm("d1"),
                                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel; (VOID)signal_mask;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_ioctl(register LONG channel __asm("d0"),
                   register ULONG command __asm("d1"),
                   register APTR buffer __asm("a0"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel; (VOID)command; (VOID)buffer;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

LONG bsd_bpf_data_waiting(register LONG channel __asm("d0"),
                          register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)channel;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

VOID bsd_bpf_close_all(struct AmiSocketBase *SocketBase)
{
    (VOID)SocketBase;
}

#endif /* AMINETXDUO_BPF */
