/*
 * AmiNetXDuo, BPF capture channels: the buffers, the record format, the
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
 * not included in the returned byte count, as in 4.4BSD. Otherwise a consumer
 * that walks records by stride reads one record too many. The six fields are
 * written individually at the offsets above, and not by a store of a
 * `struct bpf_hdr`, so no compiler padding can change what goes on the wire.
 *
 * Buffering: two buffers per channel. The tap appends to `store`, the reader
 * drains `hold`, and they swap when the reader asks for data and `hold` is
 * empty. This is the 4.4BSD design, for the same reason. The copy-out of the
 * reader can be the whole buffer, and inside the critical section that the tap
 * needs it holds off the SANA-II reader threads for milliseconds and costs
 * packets. The copy-out therefore happens outside the lock. The tap rotates
 * only when `hold` is empty, and a `reading` flag covers the unexpected case
 * of two readers.
 *
 * No AmigaOS calls here. See bpf_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

static AmiBpfChan ami_bpf_chan[AMI_BPF_MAX_CHANNELS];
volatile UWORD ami_bpf_bound_channels;

/* ---------------------------------------------------------------- helpers */

static VOID ami_bpf_copy_bytes(void *dst, const void *src, ULONG len)
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
 * Record fields are native-order ULONG and UWORD, the way a consumer reads
 * them through `struct bpf_hdr`. The copy goes byte by byte, so the record
 * needs no alignment.
 */
static VOID ami_bpf_put32(UBYTE *p, ULONG value)
{
    ami_bpf_copy_bytes(p, &value, 4);
}

static VOID ami_bpf_put16(UBYTE *p, UWORD value)
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
 * that is not an open channel, and EPERM for one that belongs to another
 * library base. The autodoc specifies both for every call in the group.
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

    /*
     * The interface table too. Both are file-scope, so a detach that the last
     * teardown missed leaves a row with `used` set. That row holds a cookie
     * into the AmiSana2If of the old stack, and at the next start
     * ami_bpf_iface_by_cookie() hands it to an injector.
     */
    for (i = 0; i < AMI_BPF_MAX_IFACES; i++)
        ami_bpf_zero_bytes(&ami_bpf_iface[i], (ULONG)sizeof(AmiBpfIf));

    ami_bpf_unlock();

    return 0;
}

/*
 * Free the buffers and the filter. Called with the lock not held.
 *
 * `force` says what to do when a copy-out is in flight. ami_bpf_read() drops
 * the lock for the copy and reads `hold` from a pointer captured before that
 * drop, so a free here hands the reader freed memory. There is no MMU to catch
 * it. Without force, the channel leaves its owner at once and only the two
 * frees are deferred. `release_pending` marks the buffers as the ones for the
 * reader to release when it finishes with them.
 */
static VOID ami_bpf_chan_release(AmiBpfChan *ch, BOOL force)
{
    APTR bufbase;
    APTR filter;

    ami_bpf_lock();

    if (ch->iface != NULL && ami_bpf_bound_channels > 0)
        ami_bpf_bound_channels--;

    if (ch->reading && !force)
    {
        /* Closed for every purpose the owner can see: ami_bpf_chan_get()
           matches on `owner`, and no base is NULL. `open` stays set so that
           ami_bpf_open(-1) does not hand the slot out while a reader still
           reads the buffer that the slot points at. */
        ch->owner           = NULL;
        ch->iface           = NULL;
        ch->release_pending = TRUE;

        ami_bpf_unlock();
        return;
    }

    bufbase = ch->bufbase;
    filter  = ch->filter;

    ami_bpf_zero_bytes(ch, (ULONG)sizeof(AmiBpfChan));

    ami_bpf_unlock();

    ami_free(bufbase);
    ami_free(filter);
}

/*
 * The owner goes away, so its channels go with it: the autodoc
 * "automatically closed when the library is closed". Nothing here blocks. The
 * lock is Forbid()/Permit() and the frees are FreeMem().
 */
VOID ami_bpf_close_owner(APTR owner)
{
    LONG i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];

        if (ch->open && ch->owner == owner)
            ami_bpf_chan_release(ch, FALSE);
    }
}

/*
 * Last resort, at netstack teardown. Anything still open belongs to a base
 * that never closed, so ownership does not apply. A copy-out in flight is no
 * reason to leave a channel behind: a deferred free here leaves the work to a
 * reader that the teardown outlives.
 */
VOID ami_bpf_cleanup(VOID)
{
    LONG i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        if (ami_bpf_chan[i].open)
            ami_bpf_chan_release(&ami_bpf_chan[i], TRUE);
    }
}

/*
 * A negative channel means any free one, and the claimed channel is the return
 * value. The Roadshow libpcap calls bpf_open(-1) and passes the returned value
 * back in d0 as the channel for bpf_set_interrupt_mask() and every
 * bpf_ioctl() that follows (docs/RESEARCH.md 55). A client cannot know which
 * channels other programs hold, so a request by number is the exception.
 */
LONG ami_bpf_open(APTR owner, LONG channel)
{
    AmiBpfChan *ch;

    if (channel >= AMI_BPF_MAX_CHANNELS)
        return AMI_BPF_ENXIO;

    /* Before the lock, and on the Process of the opener: ami_bpf_capture()
       calls ami_bpf_now() with the lock held, and a clock open there means an
       OpenDevice() under Forbid() on a reader thread. */
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

    ami_bpf_chan_release(ch, FALSE);

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

        /* release_pending: the channel has no owner left, and its buffers go
           as soon as the reader finishes. A rebind here brings back a channel
           that is already closed, and loses the bound count with it. */
        if (!ch->open || ch->release_pending || ch->iface != NULL ||
            ch->store == NULL)
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

/* Swap store and hold. The caller holds the lock, and hold is already empty. */
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

        /* Unlocked screen: the common answer by far is "not this one". */
        if (!ch->open || ch->iface != ifp || ch->store == NULL)
            continue;

        ami_bpf_lock();

        /* Re-check under the lock: BIOCSETIF or bpf_close can have run. */
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

        /* Only now, when the packet is known to be wanted: a filter that
           rejects most traffic must not pay for a timer read per frame.
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
                /* Both buffers full: the reader does not keep up. */
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
    AmiBpfChan  *ch;
    const UBYTE *src;
    ULONG        pos;
    ULONG        end;
    ULONG        nbytes;
    ULONG        budget;
    ULONG        waited = 0;
    ULONG        irq_mask;
    BOOL         pending;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
    {
        ami_bpf_unlock();
        return status;
    }

    if (buffer == NULL || len < 0)
    {
        ami_bpf_unlock();
        return AMI_BPF_EINVAL;
    }

    budget = ch->rtimeout_sec * AMI_BPF_TICKS_PER_SEC
           + ch->rtimeout_usec / (1000000UL / AMI_BPF_TICKS_PER_SEC);

    /*
     * BIOCSRTIMEOUT as 4.4BSD reads it: zero is "do not wait", anything else
     * is how long to wait for the first record. The wait is a sleep in slices
     * on the calling task, with a new look after each slice. Never on a
     * MsgPort, which belongs to whichever Process called in (544398f), and
     * never with the lock that the tap needs in order to deliver anything.
     *
     * With no timeout set the loop runs once and returns 0, as it did before
     * the loop existed.
    */
    for (;;)
    {
        /* The timeout wait drops the table lock. The owner may close this
           slot and another base may reopen it while the task sleeps. Never
           consume that replacement channel's buffers under the old handle. */
        if (ami_bpf_chan_get(owner, channel, &status) != ch)
        {
            ami_bpf_unlock();
            return status;
        }

        if (ch->reading)
        {
            ami_bpf_unlock();
            return 0;
        }

        if (ch->hold_len == 0 && ch->store_len > 0)
            ami_bpf_rotate(ch);

        if (ch->hold_len != 0)
            break;                  /* data, and the lock is still held */

        irq_mask = ch->irq_mask;
        ami_bpf_unlock();

        if (waited >= budget)
            return 0;

        if (ami_bpf_signals_set(irq_mask) != 0)
            return AMI_BPF_EINTR;

        ami_bpf_sleep(AMI_BPF_WAIT_SLICE);
        waited += AMI_BPF_WAIT_SLICE;

        ami_bpf_lock();
    }

    /*
     * Walk records forward while the next one still fits in the buffer of the
     * caller. `end` is one past the last byte of the last whole record taken.
     * `pos` is where the next read starts.
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
        /* Not one record fits, so consume nothing: a partial record
           desynchronises the consumer. This is the autodoc EINVAL, "the number
           of bytes to read does not exactly match the filter's buffer size". */
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
    pending     = ch->release_pending;

    ami_bpf_unlock();

    /* The owner closed the library during the copy above, so the buffers were
       left for this task to free. The caller still gets the bytes it asked
       for, copied out of memory that was still valid. */
    if (pending)
        ami_bpf_chan_release(ch, FALSE);

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
       a 1 that there is data waiting." FIONREAD returns the byte count. */
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
        /* No link header to take apart. Send the bytes as they are. */
        if (ifp->mtu != 0 && (ULONG)len > ifp->mtu)
            return AMI_BPF_EMSGSIZE;

        status = ifp->inject(ifp->cookie, 0, NULL, frame, (ULONG)len);

        return (status < 0) ? AMI_BPF_ENOBUFS : len;
    }

    if (len < AMI_BPF_ETH_HDR_LEN)
        return AMI_BPF_EINVAL;      /* too short to be a link-layer frame */

    /*
     * Cooked SANA-II builds the link header itself, so the 14 bytes from the
     * caller must be taken apart and not sent. The destination address and the
     * EtherType become ios2_DstAddr and ios2_PacketType, and only the
     * remainder is the payload. A header sent as payload puts two Ethernet
     * headers on the wire.
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
 * Dispatch on direction, group and number, and drop the parameter-length field
 * of the encoding. BIOCGBLEN and BIOCSBLEN share a number and differ only in
 * direction, so direction stays. The length must go, or a caller built against
 * a struct ifreq of a different size reaches no handler at all.
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
        /* The bufbase test above was outside the lock, because ami_alloc()
           must not run under Forbid(). A second BIOCSETIF on this channel can
           have installed a buffer since then, and an overwrite here drops the
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
         * Copy before the validation. A validation of the caller array,
         * followed by a run from that same array, leaves a window in which
         * another task can rewrite it into something the validator rejected.
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
     * Every command below reads or writes a ULONG or a UWORD through
     * `buffer`, which belongs to the caller. An odd address is an address
     * error on a 68000, so it is refused here instead. The three ifreq
     * commands are the exception. They touch bytes only, including the
     * sockaddr_in that SIOCGIFADDR writes by hand, so a refusal of an odd
     * address is a restriction with nothing behind it.
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

        /* When the interface is set, real BPF refuses this, because the
           buffers are already allocated. So does this. */
        if (ch->bufbase != NULL)
            return AMI_BPF_EINVAL;

        want = *(ULONG *)buffer;
        if (want < (ULONG)BPF_MINBUFSIZE)
            want = (ULONG)BPF_MINBUFSIZE;
        if (want > (ULONG)BPF_MAXBUFSIZE)
            want = (ULONG)BPF_MAXBUFSIZE;

        ch->blen         = BPF_WORDALIGN(want);
        *(ULONG *)buffer = ch->blen;    /* report what was set */
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
         * CMD_READ is per packet type in any case. The return is success and
         * not failure, because capture tools abandon an interface when this
         * fails, and a partial capture is better than no capture. See the
         * coverage note in include/aminetxduo/bpf.h.
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
         * sockaddr_in follows, so the request is 32 bytes. Written by hand and
         * not through a struct: this layer has no sockaddr, and the
         * SIOCGIFADDR of the socket path fills the same 16 bytes.
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
        /* How long bpf_read() waits for the first record. 0 is "do not wait".
           Two ULONGs, whichever timeval the caller compiled against. */
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
