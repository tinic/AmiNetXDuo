/*
 * AmiNetXDuo -- BPF capture channels: the buffers, the record format, the
 * ioctls and the six data-path vectors.
 *
 * Record format, which is what consumers depend on. Every captured frame is
 * stored as
 *
 *     offset  0  bh_tstamp.tv_sec    ULONG
 *     offset  4  bh_tstamp.tv_usec   ULONG
 *     offset  8  bh_caplen           ULONG   bytes of frame stored here
 *     offset 12  bh_datalen          ULONG   bytes the frame had on the wire
 *     offset 16  bh_hdrlen           UWORD   always 20
 *     offset 18  two bytes of pad    (BPF_WORDALIGN(18) == 20)
 *     offset 20  bh_caplen bytes of frame
 *
 * and the next record starts at BPF_WORDALIGN(bh_hdrlen + bh_caplen) from the
 * start of this one. The trailing alignment of the last record in a read is
 * not included in the returned byte count, as in 4.4BSD; otherwise a consumer
 * that walks records by stride reads one record too many. The six fields are
 * written individually at the offsets above rather than by storing a
 * `struct bpf_hdr`, so no compiler's idea of padding can change what goes on
 * the wire.
 *
 * Buffering: two buffers per channel. The tap appends to `store`, the reader
 * drains `hold`, and they swap when the reader asks for data and `hold` is
 * empty. This is 4.4BSD's design, for the same reason: the reader's copy-out
 * can be the whole buffer, and doing that inside the critical section the tap
 * needs would hold off the SANA-II reader threads for milliseconds and cost
 * packets. The copy-out therefore happens outside the lock, protected by the
 * tap only rotating when `hold` is empty, plus a `reading` flag for the
 * unexpected case of two readers.
 *
 * No AmigaOS calls here; see bpf_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

AmiBpfChan     ami_bpf_chan[AMI_BPF_MAX_CHANNELS];
volatile UWORD ami_bpf_bound_channels;

/* ---------------------------------------------------------------- helpers */

VOID ami_bpf_copy_bytes(void *dst, const void *src, ULONG len)
{
    UBYTE       *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;

    /*
     * Longword copy when both ends agree on alignment. Capture records start
     * word-aligned and the payload starts 20 bytes in, so a frame copied out
     * of an aligned packet buffer takes this path.
     */
    if (len >= 8 &&
        (((unsigned long)d & 3UL) == ((unsigned long)s & 3UL)))
    {
        while (((unsigned long)d & 3UL) != 0)
        {
            *d++ = *s++;
            len--;
        }

        while (len >= 4)
        {
            *(ULONG *)(void *)d = *(const ULONG *)(const void *)s;
            d   += 4;
            s   += 4;
            len -= 4;
        }
    }

    while (len-- > 0)
        *d++ = *s++;
}

VOID ami_bpf_zero_bytes(void *dst, ULONG len)
{
    UBYTE *d = (UBYTE *)dst;

    while (len-- > 0)
        *d++ = 0;
}

/*
 * Record fields are native-order ULONG/UWORD, matching how a consumer's
 * `struct bpf_hdr` reads them. Copying the value's own representation byte by
 * byte keeps that true without assuming the record is aligned.
 */
VOID ami_bpf_put32(UBYTE *p, ULONG value)
{
    ami_bpf_copy_bytes(p, &value, 4);
}

VOID ami_bpf_put16(UBYTE *p, UWORD value)
{
    ami_bpf_copy_bytes(p, &value, 2);
}

ULONG ami_bpf_get32(const UBYTE *p)
{
    ULONG value;

    ami_bpf_copy_bytes(&value, p, 4);

    return value;
}

UWORD ami_bpf_get16(const UBYTE *p)
{
    UWORD value;

    ami_bpf_copy_bytes(&value, p, 2);

    return value;
}

/*
 * Resolve a handle on behalf of its owner. `*status` gets ENXIO for a handle
 * that is not an open channel and EPERM for one belonging to another library
 * base; the autodoc specifies both for every call in the group.
 */
static AmiBpfChan *ami_bpf_chan_get(APTR owner, LONG channel, LONG *status)
{
    AmiBpfChan *ch;

    if (channel < 0 || channel >= AMI_BPF_MAX_CHANNELS ||
        !ami_bpf_chan[channel].open)
    {
        *status = AMI_BPF_ENXIO;
        return NULL;
    }

    ch = &ami_bpf_chan[channel];

    if (ch->owner != owner)
    {
        *status = AMI_BPF_EPERM;
        return NULL;
    }

    return ch;
}

/* Copy `len` bytes starting at `off` out of a scatter view. */
static VOID ami_bpf_view_copy(const AmiBpfView *view, ULONG off, UBYTE *dst,
                              ULONG len)
{
    UWORD i;
    ULONG base = 0;

    for (i = 0; i < view->nsegs && len > 0; i++)
    {
        ULONG seglen = view->seg[i].len;
        ULONG start;
        ULONG take;

        if (off >= base + seglen)
        {
            base += seglen;
            continue;
        }

        start = (off > base) ? (off - base) : 0;
        take  = seglen - start;
        if (take > len)
            take = len;

        ami_bpf_copy_bytes(dst, view->seg[i].base + start, take);
        dst  += take;
        len  -= take;
        off  += take;
        base += seglen;
    }

    /* Short view: leave the rest of the record zeroed rather than stale. */
    if (len > 0)
        ami_bpf_zero_bytes(dst, len);
}

/* ------------------------------------------------------------- lifecycle */

LONG ami_bpf_init(VOID)
{
    UWORD i;

    ami_bpf_lock();
    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
        ami_bpf_zero_bytes(&ami_bpf_chan[i], (ULONG)sizeof(AmiBpfChan));
    ami_bpf_bound_channels = 0;
    ami_bpf_unlock();

    return 0;
}

/* Free the buffers and the filter. Called with the lock not held. */
static VOID ami_bpf_chan_release(AmiBpfChan *ch)
{
    APTR bufbase;
    APTR filter;

    ami_bpf_lock();

    bufbase = ch->bufbase;
    filter  = ch->filter;

    if (ch->iface != NULL && ami_bpf_bound_channels > 0)
        ami_bpf_bound_channels--;

    ami_bpf_zero_bytes(ch, (ULONG)sizeof(AmiBpfChan));

    ami_bpf_unlock();

    ami_free(bufbase);
    ami_free(filter);
}

/*
 * The owner is going away, so its channels go with it -- the autodoc's
 * "automatically closed when the library is closed". Nothing here blocks: the
 * lock is Forbid()/Permit() and the frees are FreeMem().
 */
VOID ami_bpf_close_owner(APTR owner)
{
    LONG i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];

        if (ch->open && ch->owner == owner)
            ami_bpf_chan_release(ch);
    }
}

/*
 * Last resort, at netstack teardown. Anything still open belongs to a base that
 * never closed, so ownership does not enter into it and a copy-out in flight is
 * no reason to leave a channel behind.
 */
VOID ami_bpf_cleanup(VOID)
{
    LONG i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        if (ami_bpf_chan[i].open)
            ami_bpf_chan_release(&ami_bpf_chan[i]);
    }
}

/*
 * A negative channel means any free one, and the channel claimed is the return
 * value. Roadshow's own libpcap calls bpf_open(-1) and passes the returned
 * value back in d0 as the channel for bpf_set_interrupt_mask() and every
 * bpf_ioctl() that follows (docs/RESEARCH.md 55). A client cannot know which
 * channels other programs hold, so asking by number is the exception.
 */
LONG ami_bpf_open(APTR owner, LONG channel)
{
    AmiBpfChan *ch;

    if (channel >= AMI_BPF_MAX_CHANNELS)
        return AMI_BPF_ENXIO;

    /* Before the lock, and on the opener's own Process: ami_bpf_now() is called
       from ami_bpf_capture() with the lock held, and getting the clock ready
       there would mean an OpenDevice() under Forbid() on a reader thread. */
    ami_bpf_time_init();

    ami_bpf_lock();

    if (channel < 0)
    {
        LONG i;

        for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
        {
            if (!ami_bpf_chan[i].open)
                break;
        }

        if (i == AMI_BPF_MAX_CHANNELS)
        {
            ami_bpf_unlock();
            return AMI_BPF_EBUSY;   /* every channel is in use */
        }

        channel = i;
    }

    ch = &ami_bpf_chan[channel];

    if (ch->open)
    {
        ami_bpf_unlock();
        return AMI_BPF_EBUSY;
    }

    ami_bpf_zero_bytes(ch, (ULONG)sizeof(AmiBpfChan));
    ch->open  = TRUE;
    ch->owner = owner;
    ch->blen  = AMI_BPF_DEFAULT_BLEN;
    ch->dlt   = DLT_EN10MB;

    ami_bpf_unlock();

    return channel;
}

LONG ami_bpf_close(APTR owner, LONG channel)
{
    LONG        status;
    AmiBpfChan *ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
        return status;

    if (ch->reading)
        return AMI_BPF_EBUSY;   /* a read is copying out of the buffer */

    ami_bpf_chan_release(ch);

    return 0;
}

/* ------------------------------------------------------- interface binding */

VOID ami_bpf_chan_unbind(AmiBpfIf *ifp)
{
    UWORD i;

    ami_bpf_lock();

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];

        if (ch->open && ch->iface == ifp)
        {
            ch->iface = NULL;       /* ifname is kept, so a rebind can find it */
            if (ami_bpf_bound_channels > 0)
                ami_bpf_bound_channels--;
        }
    }

    ami_bpf_unlock();
}

VOID ami_bpf_chan_rebind(AmiBpfIf *ifp)
{
    UWORD i;
    UWORD n;

    ami_bpf_lock();

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];

        if (!ch->open || ch->iface != NULL || ch->store == NULL)
            continue;

        for (n = 0; n < AMI_BPF_IFNAMSIZ; n++)
        {
            if (ch->ifname[n] != ifp->name[n])
                break;
            if (ch->ifname[n] == '\0')
            {
                ch->iface = ifp;
                ch->dlt   = ifp->dlt;
                ami_bpf_bound_channels++;
                break;
            }
        }
    }

    ami_bpf_unlock();
}

/* --------------------------------------------------------------- the tap */

/* Swap store and hold. Caller holds the lock and has checked hold is empty. */
static VOID ami_bpf_rotate(AmiBpfChan *ch)
{
    UBYTE *tmp = ch->hold;

    ch->hold      = ch->store;
    ch->hold_len  = ch->store_len;
    ch->hold_pos  = 0;
    ch->store     = tmp;
    ch->store_len = 0;
}

VOID ami_bpf_capture(AmiBpfIf *ifp, const AmiBpfView *view)
{
    UWORD i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];
        ULONG       slen;
        ULONG       caplen;
        ULONG       totlen;
        ULONG       curlen;
        UBYTE      *rec;
        ULONG       sec;
        ULONG       usec;
        BOOL        wake = FALSE;

        /* Unlocked screen: the overwhelmingly common answer is "not this one". */
        if (!ch->open || ch->iface != ifp || ch->store == NULL)
            continue;

        ami_bpf_lock();

        /* Re-check under the lock: BIOCSETIF or bpf_close may have run. */
        if (!ch->open || ch->iface != ifp || ch->store == NULL ||
            ch->blen <= AMI_BPF_HDRLEN)
        {
            ami_bpf_unlock();
            continue;
        }

        ch->recv_count++;

        slen = ami_bpf_filter_view(ch->filter, ch->filter_len, view);
        if (slen == 0)
        {
            ami_bpf_unlock();
            continue;
        }

        /* Only now, once the packet is known to be wanted: a filter that
           rejects most traffic should not pay for a timer read per frame.
           GetSysTime() is a short library call and is safe under Forbid(). */
        ami_bpf_now(&sec, &usec);

        caplen = (slen < view->wirelen) ? slen : view->wirelen;
        if (caplen > view->caplen)
            caplen = view->caplen;

        totlen = AMI_BPF_HDRLEN + caplen;
        if (totlen > ch->blen)
        {
            totlen = ch->blen;
            caplen = totlen - AMI_BPF_HDRLEN;
        }

        curlen = BPF_WORDALIGN(ch->store_len);

        if (curlen + totlen > ch->blen)
        {
            if (ch->hold_len == 0 && !ch->reading)
            {
                ami_bpf_rotate(ch);
                curlen = 0;
                wake   = TRUE;
            }
            else
            {
                /* Both buffers full: the reader is not keeping up. */
                ch->drop_count++;
                ami_bpf_unlock();
                continue;
            }
        }

        rec = ch->store + curlen;

        ami_bpf_put32(rec + AMI_BPF_OFF_TSTAMP_SEC,  sec);
        ami_bpf_put32(rec + AMI_BPF_OFF_TSTAMP_USEC, usec);
        ami_bpf_put32(rec + AMI_BPF_OFF_CAPLEN,      caplen);
        ami_bpf_put32(rec + AMI_BPF_OFF_DATALEN,     view->wirelen);
        ami_bpf_put16(rec + AMI_BPF_OFF_HDRLEN,      (UWORD)AMI_BPF_HDRLEN);

        /* The pad between the header and the data, so nothing stale leaks. */
        ami_bpf_zero_bytes(rec + AMI_BPF_HDR_BYTES,
                           (ULONG)(AMI_BPF_HDRLEN - AMI_BPF_HDR_BYTES));

        ami_bpf_view_copy(view, 0, rec + AMI_BPF_HDRLEN, caplen);

        ch->store_len = curlen + totlen;

        if (ch->immediate)
            wake = TRUE;

        ami_bpf_unlock();

        if (wake)
        {
            ami_bpf_notify(ch->notify_task, ch->notify_mask);
            ami_bpf_notify(ch->irq_task, ch->irq_mask);
        }
    }
}

/* ------------------------------------------------------------- bpf_read */

LONG ami_bpf_read(APTR owner, LONG channel, APTR buffer, LONG len)
{
    LONG         status;
    AmiBpfChan  *ch = ami_bpf_chan_get(owner, channel, &status);
    const UBYTE *src;
    ULONG        pos;
    ULONG        end;
    ULONG        nbytes;
    ULONG        budget;
    ULONG        waited = 0;

    if (ch == NULL)
        return status;

    if (buffer == NULL || len < 0)
        return AMI_BPF_EINVAL;

    /*
     * BIOCSRTIMEOUT as 4.4BSD reads it: zero is "do not wait", anything else
     * is how long to wait for the first record. Waited by sleeping in slices
     * on the calling task and looking again -- never on a MsgPort, which would
     * belong to whichever Process happened to call in (544398f), and never
     * while holding the lock the tap needs in order to deliver anything.
     *
     * With no timeout set the loop runs once and returns 0, which is what it
     * did before there was a loop.
     */
    budget = ch->rtimeout_sec * AMI_BPF_TICKS_PER_SEC
           + ch->rtimeout_usec / (1000000UL / AMI_BPF_TICKS_PER_SEC);

    for (;;)
    {
        ami_bpf_lock();

        if (ch->reading)
        {
            ami_bpf_unlock();
            return 0;
        }

        if (ch->hold_len == 0 && ch->store_len > 0)
            ami_bpf_rotate(ch);

        if (ch->hold_len != 0)
            break;                  /* data, and the lock is still held */

        ami_bpf_unlock();

        if (waited >= budget)
            return 0;

        if (ami_bpf_signals_set(ch->irq_mask) != 0)
            return AMI_BPF_EINTR;

        ami_bpf_sleep(AMI_BPF_WAIT_SLICE);
        waited += AMI_BPF_WAIT_SLICE;
    }

    /*
     * Walk records forward while the next one still fits in the caller's
     * buffer. `end` is one past the last byte of the last whole record taken;
     * `pos` is where the next read will start.
     */
    pos = ch->hold_pos;
    end = ch->hold_pos;

    while (pos < ch->hold_len)
    {
        ULONG hdrlen = (ULONG)ami_bpf_get16(ch->hold + pos + AMI_BPF_OFF_HDRLEN);
        ULONG caplen = ami_bpf_get32(ch->hold + pos + AMI_BPF_OFF_CAPLEN);
        ULONG reclen = hdrlen + caplen;

        if ((pos + reclen) - ch->hold_pos > (ULONG)len)
            break;

        end = pos + reclen;
        pos = pos + BPF_WORDALIGN(reclen);
    }

    if (end == ch->hold_pos)
    {
        /* Not even one record fits, so consume nothing: a partial record would
           desynchronise the consumer. The autodoc's EINVAL, "the number of
           bytes to read does not exactly match the filter's buffer size". */
        ami_bpf_unlock();
        return AMI_BPF_EINVAL;
    }

    src    = ch->hold + ch->hold_pos;
    nbytes = end - ch->hold_pos;

    ch->reading = TRUE;

    ami_bpf_unlock();

    /* Outside the lock: this can be the whole buffer. */
    ami_bpf_copy_bytes(buffer, src, nbytes);

    ami_bpf_lock();

    ch->hold_pos = pos;
    if (ch->hold_pos >= ch->hold_len)
    {
        ch->hold_len = 0;
        ch->hold_pos = 0;
    }
    ch->reading = FALSE;

    ami_bpf_unlock();

    return (LONG)nbytes;
}

UWORD ami_bpf_capturing(VOID)
{
    return ami_bpf_bound_channels;
}

/* ------------------------------------------------------ bpf_data_waiting */

/* Bytes buffered across both buffers. Caller holds the lock. */
static ULONG ami_bpf_buffered(const AmiBpfChan *ch)
{
    return (ch->hold_len - ch->hold_pos) + ch->store_len;
}

LONG ami_bpf_data_waiting(APTR owner, LONG channel)
{
    LONG        status;
    AmiBpfChan *ch = ami_bpf_chan_get(owner, channel, &status);
    ULONG       n;

    if (ch == NULL)
        return status;

    ami_bpf_lock();
    n = ami_bpf_buffered(ch);
    ami_bpf_unlock();

    /* "A return value of 0 indicates that there is no data waiting to be read,
       a 1 that there is data waiting." The byte count is FIONREAD's answer. */
    return (n != 0) ? 1 : 0;
}

/* ------------------------------------------------------------ bpf_write */

LONG ami_bpf_write(APTR owner, LONG channel, APTR buffer, LONG len)
{
    LONG         status;
    AmiBpfChan  *ch = ami_bpf_chan_get(owner, channel, &status);
    AmiBpfIf    *ifp;
    const UBYTE *frame = (const UBYTE *)buffer;
    UWORD        ether_type;

    if (ch == NULL)
        return status;

    if (buffer == NULL || len <= 0)
        return AMI_BPF_EINVAL;

    /* The autodoc folds "not attached to an interface" into ENXIO, and an
       interface registered without an injector cannot transmit either. */
    ifp = ch->iface;
    if (ifp == NULL || ifp->inject == NULL)
        return AMI_BPF_ENXIO;

    if (ch->dlt != DLT_EN10MB)
    {
        /* No link header to take apart; hand the bytes over as they are. */
        if (ifp->mtu != 0 && (ULONG)len > ifp->mtu)
            return AMI_BPF_EMSGSIZE;

        status = ifp->inject(ifp->cookie, 0, NULL, frame, (ULONG)len);

        return (status < 0) ? AMI_BPF_ENOBUFS : len;
    }

    if (len < AMI_BPF_ETH_HDR_LEN)
        return AMI_BPF_EINVAL;      /* too short to be a link-layer frame */

    /*
     * Cooked SANA-II builds the link header itself, so the 14 bytes the caller
     * supplied have to be taken apart rather than sent: destination address
     * and EtherType become ios2_DstAddr and ios2_PacketType, and only the
     * remainder is the payload. Sending the header as payload would put two
     * Ethernet headers on the wire.
     */
    ether_type = (UWORD)(((UWORD)frame[12] << 8) | (UWORD)frame[13]);

    if (ifp->mtu != 0 &&
        (ULONG)(len - AMI_BPF_ETH_HDR_LEN) > ifp->mtu)
        return AMI_BPF_EMSGSIZE;

    status = ifp->inject(ifp->cookie, ether_type, frame,
                         frame + AMI_BPF_ETH_HDR_LEN,
                         (ULONG)(len - AMI_BPF_ETH_HDR_LEN));

    return (status < 0) ? AMI_BPF_ENOBUFS : len;
}

/* ------------------------------------------------------------ the masks */

LONG ami_bpf_set_notify_mask(APTR owner, LONG channel, ULONG signal_mask)
{
    LONG        status;
    AmiBpfChan *ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
        return status;

    ami_bpf_lock();
    ch->notify_mask = signal_mask;
    ch->notify_task = (signal_mask != 0) ? ami_bpf_current_task() : NULL;
    ami_bpf_unlock();

    return 0;
}

LONG ami_bpf_set_interrupt_mask(APTR owner, LONG channel, ULONG signal_mask)
{
    LONG        status;
    AmiBpfChan *ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
        return status;

    ami_bpf_lock();
    ch->irq_mask = signal_mask;
    ch->irq_task = (signal_mask != 0) ? ami_bpf_current_task() : NULL;
    ami_bpf_unlock();

    return 0;
}

/* ------------------------------------------------------------ bpf_ioctl */

/*
 * Dispatch on direction + group + number, dropping the parameter-length field
 * of the encoding. BIOCGBLEN and BIOCSBLEN share a number and differ only in
 * direction, so direction stays; the length must go, or a caller built against
 * a differently sized struct ifreq reaches no handler at all.
 */
#define AMI_BPF_CMD(c)  ((ULONG)(c) & (IOC_DIRMASK | 0x0000FFFFUL))

static LONG ami_bpf_ioctl_setif(AmiBpfChan *ch, const char *name)
{
    AmiBpfIf *ifp;
    UBYTE    *base;
    UBYTE    *stale;
    ULONG     blen;
    UWORD     i;

    ifp = ami_bpf_iface_by_name(name);
    if (ifp == NULL)
        return AMI_BPF_EINVAL;      /* the name is argp, not the handle */

    blen = ch->blen;
    if (blen < (ULONG)BPF_MINBUFSIZE)
        blen = (ULONG)BPF_MINBUFSIZE;
    if (blen > (ULONG)BPF_MAXBUFSIZE)
        blen = (ULONG)BPF_MAXBUFSIZE;
    blen = BPF_WORDALIGN(blen);

    base = NULL;
    if (ch->bufbase == NULL)
    {
        base = (UBYTE *)ami_alloc(2UL * blen);
        if (base == NULL)
            return AMI_BPF_ENOBUFS;
    }

    stale = NULL;

    ami_bpf_lock();

    if (base != NULL)
    {
        /* The bufbase test above was outside the lock -- ami_alloc() is not
           something to call under Forbid(). A second BIOCSETIF on this channel
           can have installed one since, and overwriting it here would drop the
           only reference to that block. */
        if (ch->bufbase != NULL)
        {
            stale = base;
        }
        else
        {
            ch->bufbase   = base;
            ch->store     = base;
            ch->hold      = base + blen;
            ch->blen      = blen;
            ch->store_len = 0;
            ch->hold_len  = 0;
            ch->hold_pos  = 0;
        }
    }

    if (ch->iface == NULL)
        ami_bpf_bound_channels++;

    ch->iface = ifp;
    ch->dlt   = ifp->dlt;

    for (i = 0; i < AMI_BPF_IFNAMSIZ; i++)
        ch->ifname[i] = ifp->name[i];

    ami_bpf_unlock();

    ami_free(stale);

    return 0;
}

static LONG ami_bpf_ioctl_setf(AmiBpfChan *ch, const struct bpf_program *prog)
{
    struct bpf_insn *copy = NULL;
    struct bpf_insn *old;
    ULONG            count;
    ULONG            i;

    if (prog == NULL)
        return AMI_BPF_EINVAL;

    count = prog->bf_len;

    if (count != 0)
    {
        if (count > (ULONG)BPF_MAXINSNS || prog->bf_insns == NULL)
            return AMI_BPF_EINVAL;

        copy = (struct bpf_insn *)ami_alloc(count *
                                            (ULONG)sizeof(struct bpf_insn));
        if (copy == NULL)
            return AMI_BPF_ENOBUFS;

        /*
         * Copy before validating. Validating the caller's array and then
         * running from it would leave a window in which another task could
         * rewrite it into something the validator rejected.
         */
        for (i = 0; i < count; i++)
            copy[i] = prog->bf_insns[i];

        if (ami_bpf_validate(copy, count) != 0)
        {
            ami_free(copy);
            return AMI_BPF_EINVAL;
        }
    }

    ami_bpf_lock();
    old            = ch->filter;
    ch->filter     = copy;
    ch->filter_len = count;
    /* A new filter invalidates what is already buffered, as in 4.4BSD. */
    ch->store_len  = 0;
    ch->hold_len   = 0;
    ch->hold_pos   = 0;
    ami_bpf_unlock();

    ami_free(old);

    return 0;
}

LONG ami_bpf_ioctl(APTR owner, LONG channel, ULONG command, APTR buffer)
{
    LONG        status;
    AmiBpfChan *ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
        return status;

    /*
     * Every command below either reads or writes a ULONG or a UWORD through
     * `buffer`, which is the caller's, and an odd one is an address error on a
     * 68000. Refused rather than faulted on. The three ifreq commands are the
     * exception -- they touch bytes only, including the sockaddr_in
     * SIOCGIFADDR writes by hand -- and refusing an odd one would be a
     * restriction with nothing behind it.
     */
    switch (AMI_BPF_CMD(command))
    {
    case AMI_BPF_CMD(BIOCGETIF):
    case AMI_BPF_CMD(BIOCSETIF):
    case AMI_BPF_CMD(AMI_BPF_SIOCGIFADDR):
        break;

    default:
        if (buffer != NULL && (((unsigned long)buffer) & 1UL) != 0UL)
            return AMI_BPF_EINVAL;
        break;
    }

    switch (AMI_BPF_CMD(command))
    {
    case AMI_BPF_CMD(AMI_BPF_FIONREAD):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        ami_bpf_lock();
        *(ULONG *)buffer = ami_bpf_buffered(ch);
        ami_bpf_unlock();
        return 0;

    case AMI_BPF_CMD(BIOCGBLEN):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        *(ULONG *)buffer = ch->blen;
        return 0;

    case AMI_BPF_CMD(BIOCSBLEN):
    {
        ULONG want;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        /* Real BPF refuses this once the interface is set, because the
           buffers are already allocated. So does this. */
        if (ch->bufbase != NULL)
            return AMI_BPF_EINVAL;

        want = *(ULONG *)buffer;
        if (want < (ULONG)BPF_MINBUFSIZE)
            want = (ULONG)BPF_MINBUFSIZE;
        if (want > (ULONG)BPF_MAXBUFSIZE)
            want = (ULONG)BPF_MAXBUFSIZE;

        ch->blen         = BPF_WORDALIGN(want);
        *(ULONG *)buffer = ch->blen;    /* report what was actually set */
        return 0;
    }

    case AMI_BPF_CMD(BIOCSETF):
        return ami_bpf_ioctl_setf(ch, (const struct bpf_program *)buffer);

    case AMI_BPF_CMD(BIOCFLUSH):
        ami_bpf_lock();
        ch->store_len  = 0;
        ch->hold_len   = 0;
        ch->hold_pos   = 0;
        ch->recv_count = 0;
        ch->drop_count = 0;
        ami_bpf_unlock();
        return 0;

    case AMI_BPF_CMD(BIOCPROMISC):
        /*
         * Recorded, not honoured: SANA-II has no promiscuous-mode command, and
         * CMD_READ is per packet type in any case. Success rather than failure
         * because capture tools routinely give up on an interface when this
         * fails, and a partial capture beats none. See the coverage note in
         * include/aminetxduo/bpf.h.
         */
        ch->promisc = TRUE;
        return 0;

    case AMI_BPF_CMD(BIOCGDLT):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        *(ULONG *)buffer = ch->dlt;
        return 0;

    case AMI_BPF_CMD(BIOCGETIF):
    {
        char *name = (char *)buffer;
        UWORD i;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        if (ch->iface == NULL)
            return AMI_BPF_EINVAL;  /* "the filter is not attached to an
                                       interface" */

        /* Only ifr_name is touched: it is the first IFNAMSIZ bytes of
           struct ifreq, and the rest is meaningless for a capture channel. */
        for (i = 0; i < AMI_BPF_IFNAMSIZ; i++)
            name[i] = ch->ifname[i];
        return 0;
    }

    case AMI_BPF_CMD(AMI_BPF_SIOCGIFADDR):
    {
        /*
         * struct ifreq: the name occupies the first IFNAMSIZ bytes and a
         * sockaddr_in follows, which is why the request is 32 bytes. Written
         * by hand rather than through a struct: this layer has no sockaddr,
         * and the socket path's own SIOCGIFADDR fills the same 16 bytes.
         */
        UBYTE *sa = (UBYTE *)buffer + AMI_BPF_IFNAMSIZ;
        ULONG  addr;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        if (ch->iface == NULL)
            return AMI_BPF_EINVAL;

        addr = ami_bpf_iface_address(ch->iface);

        sa[0] = 16;                         /* sin_len                      */
        sa[1] = 2;                          /* sin_family = AF_INET         */
        sa[2] = 0; sa[3] = 0;               /* sin_port                     */
        sa[4] = (UBYTE)(addr >> 24);        /* sin_addr, network order      */
        sa[5] = (UBYTE)(addr >> 16);
        sa[6] = (UBYTE)(addr >> 8);
        sa[7] = (UBYTE)addr;
        sa[8] = 0; sa[9] = 0; sa[10] = 0; sa[11] = 0;   /* sin_zero         */
        sa[12] = 0; sa[13] = 0; sa[14] = 0; sa[15] = 0;
        return 0;
    }

    case AMI_BPF_CMD(BIOCSETIF):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        return ami_bpf_ioctl_setif(ch, (const char *)buffer);

    case AMI_BPF_CMD(BIOCSRTIMEOUT):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        /* How long bpf_read() waits for the first record; 0 is "do not
           wait". Two ULONGs, whichever timeval the caller compiled against. */
        ch->rtimeout_sec  = ((const ULONG *)buffer)[0];
        ch->rtimeout_usec = ((const ULONG *)buffer)[1];
        return 0;

    case AMI_BPF_CMD(BIOCGRTIMEOUT):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        ((ULONG *)buffer)[0] = ch->rtimeout_sec;
        ((ULONG *)buffer)[1] = ch->rtimeout_usec;
        return 0;

    case AMI_BPF_CMD(BIOCGSTATS):
    {
        struct bpf_stat *st = (struct bpf_stat *)buffer;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        st->bs_recv = ch->recv_count;
        st->bs_drop = ch->drop_count;
        ami_bpf_unlock();
        return 0;
    }

    case AMI_BPF_CMD(BIOCIMMEDIATE):
        if (buffer == NULL)
            return AMI_BPF_EINVAL;
        ch->immediate = (*(const ULONG *)buffer != 0) ? TRUE : FALSE;
        return 0;

    case AMI_BPF_CMD(BIOCVERSION):
    {
        struct bpf_version *v = (struct bpf_version *)buffer;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        v->bv_major = BPF_MAJOR_VERSION;
        v->bv_minor = BPF_MINOR_VERSION;
        return 0;
    }

    default:
        return AMI_BPF_EINVAL;
    }
}
