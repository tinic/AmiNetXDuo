/*
 * AmiNetXDuo, mbuf emulation internals.
 *
 * Private to src/mbuf/. mbuf_alloc.c and mbuf_ops.c contain no AmigaOS calls
 * at all; everything platform-specific is behind the three hooks at the bottom
 * of this file, which mbuf_amiga.c implements for the Amiga and the host test
 * stubs out. That is the same split src/config/ uses.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_MBUF_INTERNAL_H
#define AMINETXDUO_MBUF_INTERNAL_H

#include "aminetxduo/mbuf.h"
#include "aminetxduo/compat.h"

/* --------------------------------------------------------------- tunables */

/*
 * Ceilings, not reservations: slabs are carved on demand and the cluster pool
 * grows one cluster at a time. 256 mbufs is 32 KB and 16 clusters is 32 KB, so
 * the worst case is 64 KB of the 1 MB floor (docs/RESEARCH.md 81), and only
 * if something actually uses them, which on a normal system nothing does.
 */
#ifndef AMI_MBUF_DEFAULT_MBUFS
#define AMI_MBUF_DEFAULT_MBUFS      256
#endif

#ifndef AMI_MBUF_DEFAULT_CLUSTERS
#define AMI_MBUF_DEFAULT_CLUSTERS   16
#endif

/* mbufs carved per slab allocation. 32 * 128 = 4 KB plus alignment slack. */
#ifndef AMI_MBUF_SLAB_COUNT
#define AMI_MBUF_SLAB_COUNT         32
#endif

/* Free clusters kept cached rather than handed back to ami_free(). */
#ifndef AMI_MBUF_CLUSTER_CACHE
#define AMI_MBUF_CLUSTER_CACHE      4
#endif

/* MLEN/MHLEN are sizeof-derived, hence unsigned. Signed forms for the ops. */
#define AMI_MLEN            ((LONG)MLEN)
#define AMI_MHLEN           ((LONG)MHLEN)
#define AMI_MCLBYTES        ((LONG)MCLBYTES)

/* ------------------------------------------------------------ slab layout */

typedef struct AmiMbufSlab
{
    struct AmiMbufSlab *next;
    APTR                raw;        /* what ami_free() must be given        */
} AmiMbufSlab;

/*
 * One MCLBYTES cluster. The reference count lives here rather than in
 * `struct m_ext` because that struct has no room for one and is ABI. We never
 * find this header by walking backwards from an arbitrary ext_buf, clusters
 * we own are identified by searching the `all` list, so a foreign ext_buf can
 * never make us read memory we do not own.
 */
typedef struct AmiCluster
{
    struct AmiCluster *all_next;    /* every cluster we ever allocated      */
    struct AmiCluster *free_next;   /* free list                            */
    ULONG              refcnt;      /* 0 == on the free list                */
    ULONG              pad;         /* keeps the payload 8-byte aligned     */
    /* MCLBYTES of payload follow */
} AmiCluster;

#define AMI_CLUSTER_DATA(cl)    ((UBYTE *)(void *)((AmiCluster *)(cl) + 1))

typedef struct AmiMbufPool
{
    BOOL           inited;

    AmiMbufSlab   *slabs;
    struct mbuf   *free_mbufs;
    ULONG          mbufs_total;
    ULONG          mbufs_free;
    ULONG          max_mbufs;

    AmiCluster    *clusters_all;
    AmiCluster    *clusters_free;
    ULONG          clusters_total;
    ULONG          clusters_free_count;
    ULONG          clusters_cached;
    ULONG          max_clusters;

    ULONG          drops;           /* mbstat.m_drops                       */
} AmiMbufPool;


/* -------------------------------------------------------- shared internals */

/* mbuf_alloc.c */
AmiCluster *ami_mbuf_cluster_of(const void *ext_buf);
VOID        ami_mbuf_ext_ref(struct mbuf *m);

/*
 * Start of the internal data area for m, m_pktdat when M_PKTHDR is set,
 * m_dat otherwise. Both end at (UBYTE *)m + MSIZE.
 */
#define AMI_MBUF_BASE(m) \
    (((m)->m_flags & M_PKTHDR) ? (UBYTE *)(m)->m_pktdat : (UBYTE *)(m)->m_dat)
#define AMI_MBUF_END(m)  ((UBYTE *)(void *)(m) + MSIZE)

/* ---------------------------------------------------------- platform hooks */

/*
 * Guard the free lists and the counters. Short, non-blocking, and reentrant
 * from any Exec task, Forbid()/Permit() on the Amiga, nothing on the host.
 * Never call anything that can block inside the bracket.
 */
VOID ami_mbuf_lock(VOID);
VOID ami_mbuf_unlock(VOID);

/* memcpy without dragging in a libc dependency. */
VOID ami_mbuf_copy_bytes(void *dst, const void *src, ULONG len);
VOID ami_mbuf_zero_bytes(void *dst, ULONG len);

#endif /* AMINETXDUO_MBUF_INTERNAL_H */
