/*
 * AmiNetXDuo, mbuf chain operations.
 *
 * The seven chain-manipulating vectors. Behaviour follows the documented
 * 4.4BSD semantics for m_adj / m_cat / m_copyback / m_copydata / m_copym /
 * m_prepend / m_pullup, including these two:
 *
 *   - mbuf_prepend and mbuf_pullup free the whole chain when they fail. A
 *     caller that writes `if ((m = mbuf_pullup(m, n)) == NULL) mbuf_freem(m);`
 *     double-frees.
 *   - mbuf_adj never frees anything. Emptied mbufs stay in the chain with
 *     m_len 0.
 *
 * Where 4.4BSD panics on a malformed argument, for example m_copydata past the
 * end of the chain, this returns -1 instead. These are called straight from
 * application code across an LVO, on a machine with no memory protection.
 *
 * No AmigaOS calls here.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mbuf_internal.h"

/* ---------------------------------------------------------------- helpers */

ULONG ami_mbuf_length(const struct mbuf *m)
{
    ULONG total = 0;

    while (m != NULL)
    {
        if (m->m_len > 0)
            total += (ULONG)m->m_len;
        m = m->m_next;
    }

    return total;
}

/*
 * The span of storage that m can move its data within. FALSE when the storage
 * is not exclusively ours: foreign M_EXT, or one of our clusters that
 * mbuf_copym gave a second reference to.
 */
static BOOL ami_mbuf_span(struct mbuf *m, UBYTE **base, UBYTE **end)
{
    if ((m->m_flags & M_EXT) != 0)
    {
        AmiCluster *cl;

        ami_mbuf_lock();
        cl = ami_mbuf_cluster_of(m->m_ext.ext_buf);
        if (cl != NULL && cl->refcnt != 1)
            cl = NULL;
        ami_mbuf_unlock();

        if (cl == NULL)
            return FALSE;

        *base = (UBYTE *)m->m_ext.ext_buf;
        *end  = *base + m->m_ext.ext_size;
        return TRUE;
    }

    *base = AMI_MBUF_BASE(m);
    *end  = AMI_MBUF_END(m);
    return TRUE;
}

/* Bytes available for data in a fresh, empty mbuf. */
static LONG ami_mbuf_room(struct mbuf *m)
{
    if ((m->m_flags & M_EXT) != 0)
        return (LONG)m->m_ext.ext_size;

    return (LONG)(AMI_MBUF_END(m) - AMI_MBUF_BASE(m));
}

/* TRUE when the external storage of m is a cluster we own and can count. */
static BOOL ami_mbuf_ext_is_ours(struct mbuf *m)
{
    AmiCluster *cl;

    if ((m->m_flags & M_EXT) == 0)
        return FALSE;

    ami_mbuf_lock();
    cl = ami_mbuf_cluster_of(m->m_ext.ext_buf);
    ami_mbuf_unlock();

    return (cl != NULL) ? TRUE : FALSE;
}

static struct mbuf *ami_mbuf_get_zeroed(WORD type)
{
    struct mbuf *m = ami_mbuf_get();

    if (m == NULL)
        return NULL;

    m->m_type = type;
    ami_mbuf_zero_bytes(m->m_data, (ULONG)AMI_MLEN);

    return m;
}

/* -------------------------------------------------------------- mbuf_adj */

LONG ami_mbuf_adj(struct mbuf *mp, LONG req_len)
{
    struct mbuf *m;
    LONG         len;

    if (mp == NULL)
        return -1;

    if (req_len >= 0)
    {
        /* Trim from the front. Walk forward and empty each mbuf. */
        len = req_len;

        for (m = mp; m != NULL && len > 0; )
        {
            if (m->m_len <= len)
            {
                len     -= m->m_len;
                m->m_len = 0;
                m        = m->m_next;
            }
            else
            {
                m->m_len -= len;
                m->m_data = (APTR)((UBYTE *)m->m_data + len);
                len       = 0;
            }
        }

        if ((mp->m_flags & M_PKTHDR) != 0)
            mp->m_pkthdr.len -= (req_len - len);
    }
    else
    {
        LONG total = 0;
        LONG keep;

        len = -req_len;

        for (m = mp; m != NULL; m = m->m_next)
            total += m->m_len;

        keep = total - len;
        if (keep < 0)
            keep = 0;

        if ((mp->m_flags & M_PKTHDR) != 0)
            mp->m_pkthdr.len = keep;

        /* Walk forward and give out `keep` bytes. Later mbufs get m_len 0. */
        for (m = mp; m != NULL; m = m->m_next)
        {
            if (m->m_len >= keep)
            {
                m->m_len = keep;
                keep     = 0;
            }
            else
            {
                keep -= m->m_len;
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------- mbuf_cat */

LONG ami_mbuf_cat(struct mbuf *m, struct mbuf *n)
{
    if (m == NULL || n == NULL)
        return -1;

    while (m->m_next != NULL)
        m = m->m_next;

    while (n != NULL)
    {
        /*
         * Compact n into the trailing free space of m while it fits and m owns
         * its storage internally. An M_EXT tail is never compacted into,
         * because the cluster can be shared.
         */
        if ((m->m_flags & M_EXT) != 0 ||
            (UBYTE *)m->m_data + m->m_len + n->m_len > AMI_MBUF_END(m))
        {
            m->m_next = n;
            return 0;
        }

        if (n->m_len > 0)
        {
            ami_mbuf_copy_bytes((UBYTE *)m->m_data + m->m_len,
                                n->m_data, (ULONG)n->m_len);
            m->m_len += n->m_len;
        }

        n = ami_mbuf_free(n);
    }

    m->m_next = NULL;

    return 0;
}

/* --------------------------------------------------------- mbuf_copydata */

LONG ami_mbuf_copydata(struct mbuf *m, LONG off, LONG len, APTR cp)
{
    const struct mbuf *p;
    UBYTE             *dst;
    LONG               o;
    LONG               remaining;

    if (m == NULL || off < 0 || len < 0)
        return -1;

    if (len == 0)
        return 0;

    if (cp == NULL)
        return -1;

    /*
     * Pass one: prove the chain is long enough. Nothing is written to cp
     * unless the whole copy succeeds. 4.4BSD panics here instead, which is
     * not an option across an LVO.
     */
    p = m;
    o = off;
    while (p != NULL && o >= p->m_len)
    {
        o -= p->m_len;
        p  = p->m_next;
    }

    remaining = len;
    {
        const struct mbuf *q = p;
        LONG               qo = o;

        while (q != NULL && remaining > 0)
        {
            LONG avail = q->m_len - qo;

            if (avail > remaining)
                avail = remaining;
            if (avail > 0)
                remaining -= avail;

            qo = 0;
            q  = q->m_next;
        }
    }

    if (remaining > 0)
        return -1;

    /* Pass two: copy. */
    dst       = (UBYTE *)cp;
    remaining = len;

    while (p != NULL && remaining > 0)
    {
        LONG avail = p->m_len - o;

        if (avail > remaining)
            avail = remaining;

        if (avail > 0)
        {
            ami_mbuf_copy_bytes(dst, (const UBYTE *)p->m_data + o, (ULONG)avail);
            dst       += avail;
            remaining -= avail;
        }

        o = 0;
        p = p->m_next;
    }

    return 0;
}

/* --------------------------------------------------------- mbuf_copyback */

LONG ami_mbuf_copyback(struct mbuf *m0, LONG off, LONG len, APTR cp)
{
    struct mbuf *m = m0;
    const UBYTE *src = (const UBYTE *)cp;
    LONG         totlen = 0;
    LONG         result = 0;

    if (m0 == NULL || off < 0 || len < 0)
        return -1;
    if (len > 0 && cp == NULL)
        return -1;

    /* Walk to the offset. Any gap is filled with zeroed mbufs. */
    while (off > m->m_len)
    {
        off    -= m->m_len;
        totlen += m->m_len;

        if (m->m_next == NULL)
        {
            struct mbuf *n = ami_mbuf_get_zeroed(m->m_type);

            if (n == NULL)
            {
                result = -1;
                goto out;
            }

            n->m_len  = (off + len < AMI_MLEN) ? (off + len) : AMI_MLEN;
            m->m_next = n;
        }

        m = m->m_next;
    }

    while (len > 0)
    {
        LONG chunk = m->m_len - off;

        if (chunk > len)
            chunk = len;

        if (chunk > 0)
        {
            ami_mbuf_copy_bytes((UBYTE *)m->m_data + off, src, (ULONG)chunk);
            src += chunk;
            len -= chunk;
        }

        totlen += chunk + off;
        off     = 0;

        if (len == 0)
            break;

        if (m->m_next == NULL)
        {
            struct mbuf *n = ami_mbuf_get();

            if (n == NULL)
            {
                result = -1;
                goto out;
            }

            n->m_type = m->m_type;
            n->m_len  = (len < AMI_MLEN) ? len : AMI_MLEN;
            m->m_next = n;
        }

        m = m->m_next;
    }

out:
    if ((m0->m_flags & M_PKTHDR) != 0 && m0->m_pkthdr.len < totlen)
        m0->m_pkthdr.len = totlen;

    return result;
}

/* ------------------------------------------------------------ mbuf_copym */

struct mbuf *ami_mbuf_copym(struct mbuf *m, LONG off, LONG len)
{
    struct mbuf  *head = NULL;
    struct mbuf **np   = &head;
    BOOL          copy_all;
    BOOL          want_hdr;
    LONG          copied = 0;

    if (m == NULL || off < 0 || len < 0)
        return NULL;

    copy_all = (len == M_COPYALL) ? TRUE : FALSE;
    want_hdr = (off == 0 && (m->m_flags & M_PKTHDR) != 0) ? TRUE : FALSE;

    /* Skip whole mbufs until off lands inside one. */
    while (m != NULL && off >= m->m_len)
    {
        if (m->m_len == 0 && off == 0 && m->m_next == NULL)
            break;                          /* an empty tail is a valid target */
        off -= m->m_len;
        m    = m->m_next;
    }

    if (m == NULL && off > 0)
        return NULL;

    while (m != NULL && (copy_all || len > 0))
    {
        struct mbuf *n;
        LONG         avail = m->m_len - off;
        LONG         chunk;

        if (avail <= 0)
        {
            m   = m->m_next;
            off = 0;
            continue;
        }

        if (!copy_all && avail > len)
            avail = len;

        n = (head == NULL && want_hdr) ? ami_mbuf_gethdr() : ami_mbuf_get();
        if (n == NULL)
            goto fail;

        n->m_type = m->m_type;

        if (head == NULL && want_hdr)
        {
            n->m_pkthdr.rcvif = m->m_pkthdr.rcvif;
            n->m_flags        = (WORD)(n->m_flags |
                                       (m->m_flags & (M_EOR | M_BCAST | M_MCAST)));
        }

        *np = n;
        np  = &n->m_next;

        if ((m->m_flags & M_EXT) != 0 && ami_mbuf_ext_is_ours(m))
        {
            /* Share the cluster rather than copy it. That is why our clusters
               carry a reference count. */
            n->m_ext.ext_buf  = m->m_ext.ext_buf;
            n->m_ext.ext_free = m->m_ext.ext_free;
            n->m_ext.ext_size = m->m_ext.ext_size;
            n->m_flags        = (WORD)(n->m_flags | M_EXT);
            n->m_data         = (APTR)((UBYTE *)m->m_data + off);
            ami_mbuf_ext_ref(n);
            chunk = avail;
        }
        else
        {
            LONG room = ami_mbuf_room(n);

            chunk = (avail < room) ? avail : room;
            ami_mbuf_copy_bytes(n->m_data, (const UBYTE *)m->m_data + off,
                                (ULONG)chunk);
        }

        n->m_len = chunk;
        copied  += chunk;
        if (!copy_all)
            len -= chunk;

        off += chunk;
        if (off >= m->m_len)
        {
            m   = m->m_next;
            off = 0;
        }
    }

    if (!copy_all && len > 0)
        goto fail;

    if (head == NULL)
    {
        /* Legal zero-length copy. Return an empty chain, not NULL. */
        head = want_hdr ? ami_mbuf_gethdr() : ami_mbuf_get();
        if (head == NULL)
            return NULL;
    }

    if (want_hdr)
        head->m_pkthdr.len = copied;

    return head;

fail:
    ami_mbuf_freem(head);
    return NULL;
}

/* ---------------------------------------------------------- mbuf_prepend */

struct mbuf *ami_mbuf_prepend(struct mbuf *m, LONG len)
{
    struct mbuf *mn;
    UBYTE       *base;
    UBYTE       *end;
    LONG         room;
    LONG         offset;

    if (m == NULL)
        return NULL;

    if (len < 0)
    {
        ami_mbuf_freem(m);
        return NULL;
    }

    if (len == 0)
        return m;

    /* Cheap path. There is leading slack in storage we own outright. */
    if (ami_mbuf_span(m, &base, &end))
    {
        LONG lead = (LONG)((UBYTE *)m->m_data - base);

        if (lead >= len)
        {
            m->m_data = (APTR)((UBYTE *)m->m_data - len);
            m->m_len += len;
            if ((m->m_flags & M_PKTHDR) != 0)
                m->m_pkthdr.len += len;
            return m;
        }
    }

    mn = ami_mbuf_get();
    if (mn == NULL)
    {
        ami_mbuf_freem(m);
        return NULL;
    }

    mn->m_type = m->m_type;

    if ((m->m_flags & M_PKTHDR) != 0)
    {
        mn->m_flags       = (WORD)(mn->m_flags | M_PKTHDR);
        mn->m_pkthdr      = m->m_pkthdr;
        mn->m_data        = (APTR)mn->m_pktdat;
        m->m_flags        = (WORD)(m->m_flags & ~M_PKTHDR);
    }

    room = (LONG)(AMI_MBUF_END(mn) - AMI_MBUF_BASE(mn));
    if (len > room)
    {
        (VOID)ami_mbuf_free(mn);
        ami_mbuf_freem(m);
        return NULL;
    }

    /*
     * The new data goes at the far end of the mbuf, longword aligned, so that
     * a second prepend (another protocol header) also gets the cheap path.
     * Same as MH_ALIGN in 4.4BSD.
     */
    offset     = (room - len) & ~((LONG)sizeof(LONG) - 1);
    mn->m_data = (APTR)(AMI_MBUF_BASE(mn) + offset);
    mn->m_len  = len;
    mn->m_next = m;

    if ((mn->m_flags & M_PKTHDR) != 0)
        mn->m_pkthdr.len += len;

    return mn;
}

/* ----------------------------------------------------------- mbuf_pullup */

struct mbuf *ami_mbuf_pullup(struct mbuf *n, LONG len)
{
    struct mbuf *m;
    LONG         space;

    if (n == NULL)
        return NULL;

    if (len < 0)
    {
        ami_mbuf_freem(n);
        return NULL;
    }

    if (len == 0 || n->m_len >= len)
        return n;

    if ((n->m_flags & M_EXT) == 0 && n->m_next != NULL &&
        (UBYTE *)n->m_data + len <= AMI_MBUF_END(n))
    {
        /* The head already has room for the whole run. Gather into it. */
        m    = n;
        n    = n->m_next;
        len -= m->m_len;
    }
    else
    {
        m = ami_mbuf_get();
        if (m == NULL)
        {
            ami_mbuf_freem(n);
            return NULL;
        }

        m->m_type = n->m_type;
        m->m_len  = 0;

        if ((n->m_flags & M_PKTHDR) != 0)
        {
            m->m_flags  = (WORD)(m->m_flags | M_PKTHDR);
            m->m_pkthdr = n->m_pkthdr;
            m->m_data   = (APTR)m->m_pktdat;
            n->m_flags  = (WORD)(n->m_flags & ~M_PKTHDR);
        }

        if (len > (LONG)(AMI_MBUF_END(m) - AMI_MBUF_BASE(m)))
        {
            /* Cannot be made contiguous in one mbuf. */
            m->m_next = n;
            ami_mbuf_freem(m);
            return NULL;
        }
    }

    space = (LONG)(AMI_MBUF_END(m) - ((UBYTE *)m->m_data + m->m_len));

    while (len > 0 && n != NULL)
    {
        LONG count = n->m_len;

        if (count > space)
            count = space;

        if (count <= 0)
            break;

        ami_mbuf_copy_bytes((UBYTE *)m->m_data + m->m_len, n->m_data,
                            (ULONG)count);

        len      -= count;
        m->m_len += count;
        n->m_len -= count;
        space    -= count;

        if (n->m_len > 0)
            n->m_data = (APTR)((UBYTE *)n->m_data + count);
        else
            n = ami_mbuf_free(n);
    }

    if (len > 0)
    {
        m->m_next = n;
        ami_mbuf_freem(m);
        return NULL;
    }

    m->m_next = n;

    return m;
}

/* ------------------------------------------------------------ mbuf_build */

struct mbuf *ami_mbuf_build(const void *src, ULONG len, BOOL want_pkthdr)
{
    struct mbuf  *head      = NULL;
    struct mbuf **np        = &head;
    const UBYTE  *p         = (const UBYTE *)src;
    ULONG         remaining = len;
    BOOL          first     = TRUE;

    do
    {
        struct mbuf *n;
        LONG         room;
        ULONG        chunk;

        n = (first && want_pkthdr) ? ami_mbuf_gethdr() : ami_mbuf_get();
        if (n == NULL)
            goto fail;

        *np = n;
        np  = &n->m_next;

        room = ami_mbuf_room(n);

        if (remaining > (ULONG)room && remaining >= (ULONG)MINCLSIZE)
        {
            /* Worth a cluster. If none is available, continue in plain mbufs. */
            if (ami_mbuf_clget(n) == 0)
                room = AMI_MCLBYTES;
        }

        chunk = (remaining < (ULONG)room) ? remaining : (ULONG)room;

        if (chunk > 0 && p != NULL)
        {
            ami_mbuf_copy_bytes(n->m_data, p, chunk);
            p += chunk;
        }

        n->m_len   = (LONG)chunk;
        remaining -= chunk;
        first      = FALSE;
    }
    while (remaining > 0);

    if (want_pkthdr)
        head->m_pkthdr.len = (LONG)len;

    return head;

fail:
    ami_mbuf_freem(head);
    return NULL;
}
