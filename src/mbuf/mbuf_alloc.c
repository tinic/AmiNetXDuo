/*
 * AmiNetXDuo, mbuf and cluster allocator.
 *
 * Slab allocator, because dtom() is part of the published ABI:
 *
 *     #define dtom(x) ((struct mbuf *)((__ULONG)(x) & ~(MSIZE-1)))
 *
 * A caller with a pointer into the internal data area of an mbuf can recover
 * the mbuf that way. That works only if every mbuf starts on an MSIZE
 * boundary, and AllocVec() promises 8-byte alignment, not 128. So mbufs are
 * carved out of over-allocated slabs whose first mbuf is rounded up to MSIZE,
 * and the slab keeps the raw pointer for ami_free().
 *
 * No AmigaOS calls here, see mbuf_internal.h for the three hooks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mbuf_internal.h"

static AmiMbufPool ami_mbuf_pool;

/* --------------------------------------------------------------- plumbing */

VOID ami_mbuf_copy_bytes(void *dst, const void *src, ULONG len)
{
    UBYTE       *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;

    while (len-- > 0)
        *d++ = *s++;
}

VOID ami_mbuf_zero_bytes(void *dst, ULONG len)
{
    UBYTE *d = (UBYTE *)dst;

    while (len-- > 0)
        *d++ = 0;
}

/* -------------------------------------------------------------- lifecycle */

LONG ami_mbuf_init(ULONG max_mbufs, ULONG max_clusters)
{
    LONG result = 0;

    if (max_mbufs == 0)
        max_mbufs = AMI_MBUF_DEFAULT_MBUFS;
    if (max_clusters == 0)
        max_clusters = AMI_MBUF_DEFAULT_CLUSTERS;

    ami_mbuf_lock();

    if (ami_mbuf_pool.inited)
    {
        if (ami_mbuf_pool.max_mbufs != max_mbufs ||
            ami_mbuf_pool.max_clusters != max_clusters)
            result = -1;
    }
    else
    {
        ami_mbuf_pool.max_mbufs    = max_mbufs;
        ami_mbuf_pool.max_clusters = max_clusters;
        ami_mbuf_pool.inited       = TRUE;
    }

    ami_mbuf_unlock();

    return result;
}

static VOID ami_mbuf_ensure_init(VOID)
{
    if (!ami_mbuf_pool.inited)
        (VOID)ami_mbuf_init(0, 0);
}

VOID ami_mbuf_cleanup(VOID)
{
    AmiMbufSlab *slab;
    AmiCluster  *cluster;

    ami_mbuf_lock();

    slab = ami_mbuf_pool.slabs;
    cluster = ami_mbuf_pool.clusters_all;

    ami_mbuf_pool.slabs               = NULL;
    ami_mbuf_pool.free_mbufs          = NULL;
    ami_mbuf_pool.mbufs_total         = 0;
    ami_mbuf_pool.mbufs_free          = 0;
    ami_mbuf_pool.clusters_all        = NULL;
    ami_mbuf_pool.clusters_free       = NULL;
    ami_mbuf_pool.clusters_total      = 0;
    ami_mbuf_pool.clusters_free_count = 0;
    ami_mbuf_pool.clusters_cached     = 0;
    ami_mbuf_pool.drops               = 0;
    ami_mbuf_pool.inited              = FALSE;

    ami_mbuf_unlock();

    /* Outside the lock. ami_free() must not be called under Forbid(). */
    while (slab != NULL)
    {
        AmiMbufSlab *next = slab->next;
        APTR         raw  = slab->raw;

        ami_free(raw);
        slab = next;
    }

    while (cluster != NULL)
    {
        AmiCluster *next = cluster->all_next;

        ami_free(cluster);
        cluster = next;
    }
}

VOID ami_mbuf_stats(struct mbstat *out)
{
    if (out == NULL)
        return;

    ami_mbuf_lock();
    out->m_mbufs    = ami_mbuf_pool.mbufs_total;
    out->m_clusters = ami_mbuf_pool.clusters_total;
    out->m_spare    = 0;
    out->m_clfree   = ami_mbuf_pool.clusters_free_count;
    out->m_drops    = ami_mbuf_pool.drops;
    out->m_wait     = 0;      /* no blocking allocation path exists */
    out->m_drain    = 0;      /* nothing to drain: no protocol owns mbufs */
    ami_mbuf_unlock();
}

ULONG ami_mbuf_outstanding(VOID)
{
    ULONG n;

    ami_mbuf_lock();
    n = ami_mbuf_pool.mbufs_total - ami_mbuf_pool.mbufs_free;
    ami_mbuf_unlock();

    return n;
}

ULONG ami_mbuf_clusters_outstanding(VOID)
{
    ULONG n;

    ami_mbuf_lock();
    n = ami_mbuf_pool.clusters_total - ami_mbuf_pool.clusters_free_count;
    ami_mbuf_unlock();

    return n;
}

/* ------------------------------------------------------------------ slabs */

/*
 * Carve AMI_MBUF_SLAB_COUNT MSIZE-aligned mbufs and push them onto the free
 * list. Called with the lock held. It drops the lock across ami_alloc(), so it
 * re-checks the ceiling afterwards.
 */
static BOOL ami_mbuf_grow(VOID)
{
    AmiMbufSlab *slab;
    APTR         raw;
    UBYTE       *base;
    ULONG        want;
    ULONG        bytes;
    UWORD        i;
    unsigned long misalign;

    want = ami_mbuf_pool.max_mbufs - ami_mbuf_pool.mbufs_total;
    if (want == 0)
        return FALSE;
    if (want > AMI_MBUF_SLAB_COUNT)
        want = AMI_MBUF_SLAB_COUNT;

    /* Slab header, then up to MSIZE-1 bytes of alignment slack, then the
       mbufs. The header is placed at the start of the raw block so that
       ami_free() gets the pointer ami_alloc() returned. */
    bytes = (ULONG)sizeof(AmiMbufSlab) + (MSIZE - 1) + want * MSIZE;

    ami_mbuf_unlock();
    raw = ami_alloc(bytes);
    ami_mbuf_lock();

    if (raw == NULL)
    {
        ami_mbuf_pool.drops++;
        return FALSE;
    }

    /* Another task can grow the pool while the lock is down. */
    if (ami_mbuf_pool.mbufs_total + want > ami_mbuf_pool.max_mbufs)
    {
        want = ami_mbuf_pool.max_mbufs - ami_mbuf_pool.mbufs_total;
        if (want == 0)
        {
            ami_mbuf_unlock();
            ami_free(raw);
            ami_mbuf_lock();
            return FALSE;
        }
    }

    slab       = (AmiMbufSlab *)raw;
    slab->raw  = raw;
    slab->next = ami_mbuf_pool.slabs;
    ami_mbuf_pool.slabs = slab;

    base     = (UBYTE *)raw + sizeof(AmiMbufSlab);
    misalign = (unsigned long)base & (MSIZE - 1);
    if (misalign != 0)
        base += (MSIZE - misalign);

    for (i = 0; i < (UWORD)want; i++)
    {
        struct mbuf *m = (struct mbuf *)(void *)(base + (ULONG)i * MSIZE);

        m->m_type = MT_FREE;
        m->m_next = ami_mbuf_pool.free_mbufs;
        ami_mbuf_pool.free_mbufs = m;
    }

    ami_mbuf_pool.mbufs_total += want;
    ami_mbuf_pool.mbufs_free  += want;

    return TRUE;
}

/* Pops one raw mbuf off the free list. If the list is empty, the pool grows. */
static struct mbuf *ami_mbuf_raw_get(VOID)
{
    struct mbuf *m;

    ami_mbuf_ensure_init();

    ami_mbuf_lock();

    if (ami_mbuf_pool.free_mbufs == NULL)
    {
        if (!ami_mbuf_grow())
        {
            ami_mbuf_pool.drops++;
            ami_mbuf_unlock();
            return NULL;
        }
    }

    m = ami_mbuf_pool.free_mbufs;
    if (m == NULL)
    {
        ami_mbuf_pool.drops++;
        ami_mbuf_unlock();
        return NULL;
    }

    ami_mbuf_pool.free_mbufs = m->m_next;
    ami_mbuf_pool.mbufs_free--;

    ami_mbuf_unlock();

    return m;
}

/* --------------------------------------------------------------- clusters */

AmiCluster *ami_mbuf_cluster_of(const void *ext_buf)
{
    AmiCluster *cl;

    if (ext_buf == NULL)
        return NULL;

    for (cl = ami_mbuf_pool.clusters_all; cl != NULL; cl = cl->all_next)
    {
        if ((const void *)AMI_CLUSTER_DATA(cl) == ext_buf)
            return cl;
    }

    return NULL;
}

static AmiCluster *ami_mbuf_cluster_get(VOID)
{
    AmiCluster *cl;

    ami_mbuf_ensure_init();

    ami_mbuf_lock();

    cl = ami_mbuf_pool.clusters_free;
    if (cl != NULL)
    {
        ami_mbuf_pool.clusters_free = cl->free_next;
        ami_mbuf_pool.clusters_free_count--;
        if (ami_mbuf_pool.clusters_cached > 0)
            ami_mbuf_pool.clusters_cached--;
        cl->free_next = NULL;
        cl->refcnt    = 1;
        ami_mbuf_unlock();
        return cl;
    }

    if (ami_mbuf_pool.clusters_total >= ami_mbuf_pool.max_clusters)
    {
        ami_mbuf_pool.drops++;
        ami_mbuf_unlock();
        return NULL;
    }

    ami_mbuf_unlock();
    cl = (AmiCluster *)ami_alloc((ULONG)sizeof(AmiCluster) + MCLBYTES);
    ami_mbuf_lock();

    if (cl == NULL)
    {
        ami_mbuf_pool.drops++;
        ami_mbuf_unlock();
        return NULL;
    }

    cl->refcnt    = 1;
    cl->free_next = NULL;
    cl->all_next  = ami_mbuf_pool.clusters_all;
    ami_mbuf_pool.clusters_all = cl;
    ami_mbuf_pool.clusters_total++;

    ami_mbuf_unlock();

    return cl;
}

/*
 * Drop one reference. Freed clusters go back on the cache up to
 * AMI_MBUF_CLUSTER_CACHE. Beyond that they stay on the `all` list and are
 * still counted as free, so only ami_mbuf_cleanup() releases the memory. That
 * keeps ami_mbuf_cluster_of() correct. The address of a cluster never becomes
 * reusable for something else while the pool is up.
 */
static VOID ami_mbuf_cluster_put(AmiCluster *cl)
{
    if (cl == NULL)
        return;

    ami_mbuf_lock();

    if (cl->refcnt > 0)
        cl->refcnt--;

    if (cl->refcnt == 0)
    {
        cl->free_next = ami_mbuf_pool.clusters_free;
        ami_mbuf_pool.clusters_free = cl;
        ami_mbuf_pool.clusters_free_count++;
        if (ami_mbuf_pool.clusters_cached < AMI_MBUF_CLUSTER_CACHE)
            ami_mbuf_pool.clusters_cached++;
    }

    ami_mbuf_unlock();
}

LONG ami_mbuf_clget(struct mbuf *m)
{
    AmiCluster *cl;

    if (m == NULL || (m->m_flags & M_EXT) != 0)
        return -1;

    cl = ami_mbuf_cluster_get();
    if (cl == NULL)
        return -1;

    m->m_ext.ext_buf  = (APTR)AMI_CLUSTER_DATA(cl);
    m->m_ext.ext_free = NULL;
    m->m_ext.ext_size = MCLBYTES;
    m->m_data         = m->m_ext.ext_buf;
    m->m_len          = 0;
    m->m_flags       |= M_EXT;

    return 0;
}

struct mbuf *ami_mbuf_getcl(VOID)
{
    struct mbuf *m = ami_mbuf_get();

    if (m == NULL)
        return NULL;

    if (ami_mbuf_clget(m) != 0)
    {
        (VOID)ami_mbuf_free(m);
        return NULL;
    }

    return m;
}

/*
 * Take a second reference to the M_EXT storage of m, for ami_mbuf_copym().
 * Only meaningful for clusters we own. The caller must check that with
 * ami_mbuf_cluster_of() first.
 */
VOID ami_mbuf_ext_ref(struct mbuf *m)
{
    AmiCluster *cl;

    if (m == NULL || (m->m_flags & M_EXT) == 0)
        return;

    ami_mbuf_lock();
    cl = ami_mbuf_cluster_of(m->m_ext.ext_buf);
    if (cl != NULL)
        cl->refcnt++;
    ami_mbuf_unlock();
}

static VOID ami_mbuf_ext_unref(struct mbuf *m)
{
    AmiCluster *cl;
    APTR        buf;
    APTR        freefn;
    ULONG       size;

    if (m == NULL || (m->m_flags & M_EXT) == 0)
        return;

    buf    = m->m_ext.ext_buf;
    freefn = m->m_ext.ext_free;
    size   = m->m_ext.ext_size;

    m->m_ext.ext_buf  = NULL;
    m->m_ext.ext_free = NULL;
    m->m_ext.ext_size = 0;

    ami_mbuf_lock();
    cl = ami_mbuf_cluster_of(buf);
    ami_mbuf_unlock();

    if (cl != NULL)
    {
        /* Ours. ext_free on one of our clusters is a caller error. The
           reference count is the authority, so the hook is ignored. */
        ami_mbuf_cluster_put(cl);
        return;
    }

    if (freefn != NULL)
    {
        /*
         * Foreign storage with a release hook. The 4.4BSD signature is
         * void (*)(caddr_t, u_int) with stack arguments, which is what
         * __stdargs gives here. Inferred from 4.4BSD. No Amiga header
         * documents the calling convention for ext_free.
         */
        void (*fn)(APTR, ULONG) = (void (*)(APTR, ULONG))freefn;

        fn(buf, size);
    }

    /* Foreign storage with no hook: the caller owns it. Nothing to do. */
}

/* ------------------------------------------------------- the four vectors */

struct mbuf *ami_mbuf_get(VOID)
{
    struct mbuf *m = ami_mbuf_raw_get();

    if (m == NULL)
        return NULL;

    m->m_next    = NULL;
    m->m_nextpkt = NULL;
    m->m_data    = (APTR)m->m_dat;
    m->m_len     = 0;
    m->m_type    = MT_DATA;
    m->m_flags   = 0;

    return m;
}

struct mbuf *ami_mbuf_gethdr(VOID)
{
    struct mbuf *m = ami_mbuf_raw_get();

    if (m == NULL)
        return NULL;

    m->m_next          = NULL;
    m->m_nextpkt       = NULL;
    m->m_data          = (APTR)m->m_pktdat;
    m->m_len           = 0;
    m->m_type          = MT_HEADER;
    m->m_flags         = M_PKTHDR;
    m->m_pkthdr.rcvif  = NULL;
    m->m_pkthdr.len    = 0;

    return m;
}

struct mbuf *ami_mbuf_free(struct mbuf *m)
{
    struct mbuf *next;

    if (m == NULL)
        return NULL;

    next = m->m_next;

    if ((m->m_flags & M_EXT) != 0)
        ami_mbuf_ext_unref(m);

    m->m_type    = MT_FREE;
    m->m_flags   = 0;
    m->m_len     = 0;
    m->m_data    = NULL;
    m->m_nextpkt = NULL;

    ami_mbuf_lock();
    m->m_next = ami_mbuf_pool.free_mbufs;
    ami_mbuf_pool.free_mbufs = m;
    ami_mbuf_pool.mbufs_free++;
    ami_mbuf_unlock();

    return next;
}

VOID ami_mbuf_freem(struct mbuf *m)
{
    while (m != NULL)
        m = ami_mbuf_free(m);
}
