/*
 * AmiNetXDuo, BPF capture channels: the buffers, the record format, the
 * ioctls and the six data-path vectors.
 *
 * Wire record: tv_sec/tv_usec/caplen/datalen ULONGs, hdrlen UWORD (always 20),
 * 2 pad, then caplen bytes; next record at BPF_WORDALIGN(hdrlen + caplen).
 * The six fields are written individually, never as a `struct bpf_hdr` store,
 * so no compiler padding can change what goes on the wire.  The trailing
 * alignment of the LAST record in a read is not counted in the byte returned.
 *
 * Two buffers per channel: the tap appends to `store`, the reader drains
 * `hold`, and the copy-out happens OUTSIDE the lock.
 *
 * No AmigaOS calls here. See bpf_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

static AmiBpfChan ami_bpf_chan[AMI_BPF_MAX_CHANNELS];
/* Kept outside AmiBpfChan because closing zeroes that structure.  Owner
   equality alone cannot distinguish a replacement channel from the one an
   ioctl started on; the generation can. */
static ULONG ami_bpf_chan_generation[AMI_BPF_MAX_CHANNELS];
volatile UWORD ami_bpf_bound_channels;

/* ---------------------------------------------------------------- helpers */

static VOID ami_bpf_copy_bytes(void *dst, const void *src, ULONG len)
{
    UBYTE       *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;

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

/* Native-order ULONG/UWORD, copied byte by byte so the record needs no
   alignment. */
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
 * Resolve a handle on behalf of its owner.  `*status` gets ENXIO for a handle
 * that is not an open channel, EPERM for one owned by another library base.
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
    {
        ami_bpf_zero_bytes(&ami_bpf_chan[i], (ULONG)sizeof(AmiBpfChan));
        ami_bpf_chan_generation[i] = 0;
    }
    ami_bpf_bound_channels = 0;

    /* The interface table too: a surviving row holds a cookie into the
       AmiSana2If of the old stack. */
    for (i = 0; i < AMI_BPF_MAX_IFACES; i++)
        ami_bpf_zero_bytes(&ami_bpf_iface[i], (ULONG)sizeof(AmiBpfIf));

    ami_bpf_unlock();

    return 0;
}

/*
 * Free the buffers and the filter.  MUST be called with the lock NOT held.
 * Without `force`, a copy-out in flight defers the two frees and marks them
 * `release_pending` for the reader to release.
 */
static VOID ami_bpf_chan_release(AmiBpfChan *ch, BOOL force,
                                 APTR expected_owner)
{
    APTR bufbase;
    APTR filter;

    ami_bpf_lock();

    /* Retire the slot only if it still belongs to the base being destroyed.
       NULL is the unconditional form, for final cleanup and deferred
       release. */
    if (expected_owner != NULL &&
        (!ch->open || ch->owner != expected_owner))
    {
        ami_bpf_unlock();
        return;
    }

    if (ch->iface != NULL && ami_bpf_bound_channels > 0)
        ami_bpf_bound_channels--;

    if (ch->reading && !force)
    {
        /* `open` must stay set so ami_bpf_open(-1) does not hand the slot out
           while a reader still reads the buffer it points at. */
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

/* Channels close with their owning library base.  Nothing here may block:
   the lock is Forbid()/Permit(). */
VOID ami_bpf_close_owner(APTR owner)
{
    LONG i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];

        if (ch->open && ch->owner == owner)
            ami_bpf_chan_release(ch, FALSE, owner);
    }
}

/* Last resort, at netstack teardown: ownership does not apply and a copy-out
   in flight must not defer the free past the teardown. */
VOID ami_bpf_cleanup(VOID)
{
    LONG i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        if (ami_bpf_chan[i].open)
            ami_bpf_chan_release(&ami_bpf_chan[i], TRUE, NULL);
    }
}

/* A negative channel means any free one; the claimed channel is the return
   value. */
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

    ami_bpf_chan_generation[channel]++;
    if (ami_bpf_chan_generation[channel] == 0)
        ami_bpf_chan_generation[channel] = 1;

    ami_bpf_unlock();

    return channel;
}

LONG ami_bpf_close(APTR owner, LONG channel)
{
    LONG        status;
    AmiBpfChan *ch;
    APTR        bufbase;
    APTR        filter;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
    {
        ami_bpf_unlock();
        return status;
    }

    if (ch->reading)
    {
        ami_bpf_unlock();
        return AMI_BPF_EBUSY;   /* a read is copying out of the buffer */
    }

    /* Validate and retire the slot in the SAME critical section, or a stale
       close destroys a replacement channel that reopened the number. */
    if (ch->iface != NULL && ami_bpf_bound_channels > 0)
        ami_bpf_bound_channels--;

    bufbase = ch->bufbase;
    filter  = ch->filter;
    ami_bpf_zero_bytes(ch, (ULONG)sizeof(AmiBpfChan));

    ami_bpf_unlock();

    ami_free(bufbase);
    ami_free(filter);

    return 0;
}

/* ------------------------------------------------------- interface binding */

/* Interface registration and its channel bindings are one table transaction.
   Both helpers are called with the shared BPF lock already held. */
VOID ami_bpf_chan_unbind_locked(AmiBpfIf *ifp)
{
    UWORD i;

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

}

VOID ami_bpf_chan_rebind_locked(AmiBpfIf *ifp)
{
    UWORD i;
    UWORD n;

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

VOID ami_bpf_capture(APTR cookie, const AmiBpfView *view)
{
    UWORD i;

    for (i = 0; i < AMI_BPF_MAX_CHANNELS; i++)
    {
        AmiBpfChan *ch = &ami_bpf_chan[i];
        AmiBpfIf   *ifp;
        ULONG       slen;
        ULONG       caplen;
        ULONG       totlen;
        ULONG       curlen;
        UBYTE      *rec;
        ULONG       sec;
        ULONG       usec;
        BOOL        wake = FALSE;

        /* Unlocked screen: the common answer by far is "not this one". */
        if (!ch->open || ch->store == NULL)
            continue;

        ami_bpf_lock();

        /* Resolve the opaque interface identity under the registry lock. A
           pointer obtained before this lock can name a detached table row
           that has since been reused by an unrelated interface. */
        ifp = ami_bpf_iface_by_cookie(cookie);
        if (ifp == NULL)
        {
            ami_bpf_unlock();
            return;
        }

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

        if (wake)
        {
            /* Keep the channel lock through Signal(): after Permit a
               close/reopen can reuse the slot.  Signal() does not block and
               is safe under Forbid(). */
            ami_bpf_notify(ch->notify_task, ch->notify_mask);
            ami_bpf_notify(ch->irq_task, ch->irq_mask);
        }

        ami_bpf_unlock();
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
     * BIOCSRTIMEOUT as 4.4BSD reads it: 0 is "do not wait".  The wait sleeps
     * in slices on the calling task -- never on a MsgPort (it belongs to
     * whichever Process called in) and never holding the tap's lock.
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
           desynchronises the consumer.  EINVAL, per the autodoc. */
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

    if (pending)
        ami_bpf_chan_release(ch, FALSE, NULL);

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
    AmiBpfChan *ch;
    ULONG       n;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, &status);
    if (ch == NULL)
    {
        ami_bpf_unlock();
        return status;
    }

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
    AmiBpfChan  *ch;
    AmiBpfIf    *ifp;
    AmiBpfInjectFn inject;
    APTR         cookie;
    ULONG        dlt;
    ULONG        mtu;
    const UBYTE *frame = (const UBYTE *)buffer;
    UWORD        ether_type;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, &status);

    if (ch == NULL)
    {
        ami_bpf_unlock();
        return status;
    }

    if (buffer == NULL || len <= 0)
    {
        ami_bpf_unlock();
        return AMI_BPF_EINVAL;
    }

    /* The autodoc folds "not attached to an interface" into ENXIO, and an
       interface registered without an injector cannot transmit either. */
    ifp = ch->iface;
    if (ifp == NULL || ifp->inject == NULL)
    {
        ami_bpf_unlock();
        return AMI_BPF_ENXIO;
    }

    /* Keep NO pointers into the channel or interface table across the
       injector call: it can block, and RemoveInterface() may run on another
       task while it does. */
    inject = ifp->inject;
    cookie = ifp->cookie;
    dlt    = ch->dlt;
    mtu    = ifp->mtu;
    ami_bpf_unlock();

    if (dlt != DLT_EN10MB)
    {
        /* No link header to take apart. Send the bytes as they are. */
        if (mtu != 0 && (ULONG)len > mtu)
            return AMI_BPF_EMSGSIZE;

        status = inject(cookie, 0, NULL, frame, (ULONG)len);

        return (status < 0) ? AMI_BPF_ENOBUFS : len;
    }

    if (len < AMI_BPF_ETH_HDR_LEN)
        return AMI_BPF_EINVAL;      /* too short to be a link-layer frame */

    /* Cooked SANA-II builds the link header itself: the caller's 14 bytes
       must become ios2_DstAddr + ios2_PacketType, with only the remainder
       sent as payload. */
    ether_type = (UWORD)(((UWORD)frame[12] << 8) | (UWORD)frame[13]);

    if (mtu != 0 && (ULONG)(len - AMI_BPF_ETH_HDR_LEN) > mtu)
        return AMI_BPF_EMSGSIZE;

    status = inject(cookie, ether_type, frame,
                    frame + AMI_BPF_ETH_HDR_LEN,
                    (ULONG)(len - AMI_BPF_ETH_HDR_LEN));

    return (status < 0) ? AMI_BPF_ENOBUFS : len;
}

/* ------------------------------------------------------------ the masks */

LONG ami_bpf_set_notify_mask(APTR owner, LONG channel, ULONG signal_mask)
{
    LONG        status;
    AmiBpfChan *ch;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, &status);
    if (ch == NULL)
    {
        ami_bpf_unlock();
        return status;
    }

    ch->notify_mask = signal_mask;
    ch->notify_task = (signal_mask != 0) ? ami_bpf_current_task() : NULL;
    ami_bpf_unlock();

    return 0;
}

LONG ami_bpf_set_interrupt_mask(APTR owner, LONG channel, ULONG signal_mask)
{
    LONG        status;
    AmiBpfChan *ch;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, &status);
    if (ch == NULL)
    {
        ami_bpf_unlock();
        return status;
    }

    ch->irq_mask = signal_mask;
    ch->irq_task = (signal_mask != 0) ? ami_bpf_current_task() : NULL;
    ami_bpf_unlock();

    return 0;
}

/* ------------------------------------------------------------ bpf_ioctl */

/* Dispatch on direction, group and number.  The parameter-length field MUST
   be dropped, or a caller built against a differently sized struct ifreq
   reaches no handler at all. */
#define AMI_BPF_CMD(c)  ((ULONG)(c) & (IOC_DIRMASK | 0x0000FFFFUL))

/* Take the identity of a channel across an allocation.  The GENERATION is the
   identity; the returned pointer is only a convenience. */
static AmiBpfChan *ami_bpf_ioctl_snapshot(APTR owner, LONG channel,
                                           ULONG *generation, ULONG *blen,
                                           BOOL *has_buffer, LONG *status)
{
    AmiBpfChan *ch;

    ami_bpf_lock();
    ch = ami_bpf_chan_get(owner, channel, status);
    if (ch != NULL)
    {
        *generation = ami_bpf_chan_generation[channel];
        if (blen != NULL)
            *blen = ch->blen;
        if (has_buffer != NULL)
            *has_buffer = (ch->bufbase != NULL) ? TRUE : FALSE;
    }
    ami_bpf_unlock();

    return ch;
}

/* Called with the channel lock held. */
static AmiBpfChan *ami_bpf_ioctl_revalidate(APTR owner, LONG channel,
                                             ULONG generation, LONG *status)
{
    AmiBpfChan *ch = ami_bpf_chan_get(owner, channel, status);

    if (ch != NULL && ami_bpf_chan_generation[channel] != generation)
    {
        *status = AMI_BPF_ENXIO;
        return NULL;
    }

    return ch;
}

static LONG ami_bpf_ioctl_setif(APTR owner, LONG channel, const char *name)
{
    char      ifname[AMI_BPF_IFNAMSIZ];
    AmiBpfChan *ch;
    AmiBpfIf *ifp;
    UBYTE    *base;
    UBYTE    *stale;
    ULONG     blen;
    ULONG     generation = 0;
    LONG      status;
    BOOL      has_buffer = FALSE;
    UWORD     i;

    /* struct ifreq begins with exactly IFNAMSIZ bytes. Copy them before any
       lock is held, both to keep caller memory out of Forbid() and so a second
       task cannot change which interface this operation commits. */
    for (i = 0; i < AMI_BPF_IFNAMSIZ - 1 && name[i] != '\0'; i++)
        ifname[i] = name[i];
    for (; i < AMI_BPF_IFNAMSIZ; i++)
        ifname[i] = '\0';

    ch = ami_bpf_ioctl_snapshot(owner, channel, &generation, &blen,
                                &has_buffer, &status);
    if (ch == NULL)
        return status;

    if (blen < (ULONG)BPF_MINBUFSIZE)
        blen = (ULONG)BPF_MINBUFSIZE;
    if (blen > (ULONG)BPF_MAXBUFSIZE)
        blen = (ULONG)BPF_MAXBUFSIZE;
    blen = BPF_WORDALIGN(blen);

    base = NULL;
    if (!has_buffer)
    {
        base = (UBYTE *)ami_alloc(2UL * blen);
        if (base == NULL)
            return AMI_BPF_ENOBUFS;
    }

    stale = NULL;

    ami_bpf_lock();

    ch = ami_bpf_ioctl_revalidate(owner, channel, generation, &status);
    if (ch == NULL)
    {
        ami_bpf_unlock();
        ami_free(base);
        return status;
    }

    /* The interface registry uses the same lock. Resolve the copied name only
       now, so detach/reuse cannot turn an old AmiBpfIf pointer into a binding
       to a different interface. */
    ifp = ami_bpf_iface_by_name(ifname);
    if (ifp == NULL)
    {
        ami_bpf_unlock();
        ami_free(base);
        return AMI_BPF_EINVAL;      /* the name is argp, not the handle */
    }

    if (base != NULL)
    {
        /* The bufbase test above was outside the lock (ami_alloc() must not
           run under Forbid()), so a second BIOCSETIF may have installed a
           buffer since. */
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

static LONG ami_bpf_ioctl_setf(APTR owner, LONG channel,
                               const struct bpf_program *prog)
{
    AmiBpfChan     *ch;
    struct bpf_insn *copy = NULL;
    struct bpf_insn *old;
    ULONG            count;
    ULONG            generation = 0;
    ULONG            i;
    LONG             status;

    if (prog == NULL)
        return AMI_BPF_EINVAL;

    ch = ami_bpf_ioctl_snapshot(owner, channel, &generation, NULL, NULL,
                                &status);
    if (ch == NULL)
        return status;

    count = prog->bf_len;

    if (count != 0)
    {
        if (count > (ULONG)BPF_MAXINSNS || prog->bf_insns == NULL)
            return AMI_BPF_EINVAL;

        copy = (struct bpf_insn *)ami_alloc(count *
                                            (ULONG)sizeof(struct bpf_insn));
        if (copy == NULL)
            return AMI_BPF_ENOBUFS;

        /* Copy BEFORE validating: validating the caller's array and then
           running from it lets another task rewrite it in between. */
        for (i = 0; i < count; i++)
            copy[i] = prog->bf_insns[i];

        if (ami_bpf_validate(copy, count) != 0)
        {
            ami_free(copy);
            return AMI_BPF_EINVAL;
        }
    }

    ami_bpf_lock();
    ch = ami_bpf_ioctl_revalidate(owner, channel, generation, &status);
    if (ch == NULL)
    {
        ami_bpf_unlock();
        ami_free(copy);
        return status;
    }

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

    /* An odd `buffer` is an address error on a 68000, so word/long commands
       refuse it here.  The three ifreq commands touch bytes only and are
       exempt. */
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
    {
        ULONG n;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        n = ami_bpf_buffered(ch);
        ami_bpf_unlock();

        *(ULONG *)buffer = n;
        return 0;
    }

    case AMI_BPF_CMD(BIOCGBLEN):
    {
        ULONG blen;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        blen = ch->blen;
        ami_bpf_unlock();

        *(ULONG *)buffer = blen;
        return 0;
    }

    case AMI_BPF_CMD(BIOCSBLEN):
    {
        ULONG want;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        want = *(ULONG *)buffer;
        if (want < (ULONG)BPF_MINBUFSIZE)
            want = (ULONG)BPF_MINBUFSIZE;
        if (want > (ULONG)BPF_MAXBUFSIZE)
            want = (ULONG)BPF_MAXBUFSIZE;
        want = BPF_WORDALIGN(want);

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }

        /* Refused once the interface is set (the buffers exist).  The check
           must be under the SAME lock as the update. */
        if (ch->bufbase != NULL)
        {
            ami_bpf_unlock();
            return AMI_BPF_EINVAL;
        }

        ch->blen = want;
        ami_bpf_unlock();

        *(ULONG *)buffer = want;        /* report what was set */
        return 0;
    }

    case AMI_BPF_CMD(BIOCSETF):
        return ami_bpf_ioctl_setf(owner, channel,
                                  (const struct bpf_program *)buffer);

    case AMI_BPF_CMD(BIOCFLUSH):
        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        ch->store_len  = 0;
        ch->hold_len   = 0;
        ch->hold_pos   = 0;
        ch->recv_count = 0;
        ch->drop_count = 0;
        ami_bpf_unlock();
        return 0;

    case AMI_BPF_CMD(BIOCPROMISC):
        /* Recorded, not honoured: SANA-II has no promiscuous-mode command.
           Must return success -- capture tools abandon an interface when this
           fails. */
        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        ch->promisc = TRUE;
        ami_bpf_unlock();
        return 0;

    case AMI_BPF_CMD(BIOCGDLT):
    {
        ULONG dlt;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        dlt = ch->dlt;
        ami_bpf_unlock();

        *(ULONG *)buffer = dlt;
        return 0;
    }

    case AMI_BPF_CMD(BIOCGETIF):
    {
        char *name = (char *)buffer;
        char  ifname[AMI_BPF_IFNAMSIZ];
        UWORD i;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        if (ch->iface == NULL)
        {
            ami_bpf_unlock();
            return AMI_BPF_EINVAL;  /* "the filter is not attached to an
                                       interface" */
        }

        for (i = 0; i < AMI_BPF_IFNAMSIZ; i++)
            ifname[i] = ch->ifname[i];
        ami_bpf_unlock();

        /* Only ifr_name is touched: it is the first IFNAMSIZ bytes of
           struct ifreq, and the rest is meaningless for a capture channel. */
        for (i = 0; i < AMI_BPF_IFNAMSIZ; i++)
            name[i] = ifname[i];
        return 0;
    }

    case AMI_BPF_CMD(AMI_BPF_SIOCGIFADDR):
    {
        /* struct ifreq: IFNAMSIZ name bytes then a sockaddr_in, 32 bytes
           total, written by hand because this layer has no sockaddr. */
        UBYTE *sa;
        APTR   cookie;
        ULONG  addr;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        if (ch->iface == NULL)
        {
            ami_bpf_unlock();
            return AMI_BPF_EINVAL;
        }

        /* Keep no interface-table pointer after the registry lock drops: a
           detach can clear and reuse its row while the address hook runs. */
        cookie = ch->iface->cookie;
        ami_bpf_unlock();

        sa = (UBYTE *)buffer + AMI_BPF_IFNAMSIZ;

        addr = ami_bpf_cookie_address(cookie);

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
        return ami_bpf_ioctl_setif(owner, channel, (const char *)buffer);

    case AMI_BPF_CMD(BIOCSRTIMEOUT):
    {
        ULONG sec;
        ULONG usec;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        /* How long bpf_read() waits for the first record. 0 is "do not wait".
           Two ULONGs, whichever timeval the caller compiled against. */
        sec  = ((const ULONG *)buffer)[0];
        usec = ((const ULONG *)buffer)[1];

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        ch->rtimeout_sec  = sec;
        ch->rtimeout_usec = usec;
        ami_bpf_unlock();
        return 0;
    }

    case AMI_BPF_CMD(BIOCGRTIMEOUT):
    {
        ULONG sec;
        ULONG usec;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        sec  = ch->rtimeout_sec;
        usec = ch->rtimeout_usec;
        ami_bpf_unlock();

        ((ULONG *)buffer)[0] = sec;
        ((ULONG *)buffer)[1] = usec;
        return 0;
    }

    case AMI_BPF_CMD(BIOCGSTATS):
    {
        struct bpf_stat *st = (struct bpf_stat *)buffer;
        ULONG recv;
        ULONG drop;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        recv = ch->recv_count;
        drop = ch->drop_count;
        ami_bpf_unlock();

        st->bs_recv = recv;
        st->bs_drop = drop;
        return 0;
    }

    case AMI_BPF_CMD(BIOCIMMEDIATE):
    {
        BOOL immediate;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        immediate = (*(const ULONG *)buffer != 0) ? TRUE : FALSE;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        if (ch == NULL)
        {
            ami_bpf_unlock();
            return status;
        }
        ch->immediate = immediate;
        ami_bpf_unlock();
        return 0;
    }

    case AMI_BPF_CMD(BIOCVERSION):
    {
        struct bpf_version *v = (struct bpf_version *)buffer;

        if (buffer == NULL)
            return AMI_BPF_EINVAL;

        ami_bpf_lock();
        ch = ami_bpf_chan_get(owner, channel, &status);
        ami_bpf_unlock();
        if (ch == NULL)
            return status;

        v->bv_major = BPF_MAJOR_VERSION;
        v->bv_minor = BPF_MINOR_VERSION;
        return 0;
    }

    default:
        return AMI_BPF_EINVAL;
    }
}
