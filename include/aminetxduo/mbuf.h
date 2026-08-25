/*
 * AmiNetXDuo, 4.4BSD mbuf emulation behind the mbuf_* LVOs.  An mbuf never
 * owns, wraps or references an NX_PACKET; conversion is an explicit copy.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_MBUF_H
#define AMINETXDUO_MBUF_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ assertions */

#ifndef AMI_STATIC_ASSERT
#  if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#    define AMI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  else
#    define AMI_SA_CAT2(a, b) a##b
#    define AMI_SA_CAT(a, b)  AMI_SA_CAT2(a, b)
#    define AMI_STATIC_ASSERT(cond, msg) \
        typedef char AMI_SA_CAT(ami_static_assert_, __LINE__)[(cond) ? 1 : -1]
#  endif
#endif

/* ------------------------------------------------------------- the layout */

#ifdef AMI_MBUF_REPLICA

/*
 * Host-test replica: the NDK struct field for field. MSIZE scales with pointer
 * width here, so the constants differ from 68k's; the exact offsets are
 * asserted only on the Amiga, where they are the ABI.
 */

#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#  define MSIZE         256         /* 64-bit host replica                  */
#else
#  define MSIZE         128         /* size of an mbuf (the real value)     */
#endif
#define MCLBYTES        2048        /* large enough for ether MTU           */
#define MCLSHIFT        11
#define MCLOFSET        (MCLBYTES - 1)

struct ifnet;
struct mbuf;

struct m_hdr {
    struct mbuf *mh_next;           /* next buffer in chain                 */
    struct mbuf *mh_nextpkt;        /* next chain in queue/record           */
    APTR         mh_data;           /* location of data                     */
    LONG         mh_len;            /* amount of data in this mbuf          */
    WORD         mh_type;           /* type of data in this mbuf            */
    WORD         mh_flags;          /* flags; see below                     */
};

struct pkthdr {
    struct ifnet *rcvif;            /* rcv interface                        */
    LONG          len;              /* total packet length                  */
};

struct m_ext {
    APTR  ext_buf;                  /* start of buffer                      */
    APTR  ext_free;                 /* free routine if not the usual        */
    ULONG ext_size;                 /* size of buffer, for ext_free         */
};

#define MLEN            (MSIZE - sizeof(struct m_hdr))
#define MHLEN           (MLEN - sizeof(struct pkthdr))
#define MINCLSIZE       (MHLEN + 1)
#define M_MAXCOMPRESS   (MHLEN / 2)

struct mbuf {
    struct m_hdr m_hdr;
    union {
        struct {
            struct pkthdr MH_pkthdr;
            union {
                struct m_ext MH_ext;
                UBYTE        MH_databuf[MHLEN];
            } MH_dat;
        } MH;
        UBYTE M_databuf[MLEN];
    } M_dat;
};

#define m_next          m_hdr.mh_next
#define m_len           m_hdr.mh_len
#define m_data          m_hdr.mh_data
#define m_type          m_hdr.mh_type
#define m_flags         m_hdr.mh_flags
#define m_nextpkt       m_hdr.mh_nextpkt
#define m_act           m_nextpkt
#define m_pkthdr        M_dat.MH.MH_pkthdr
#define m_ext           M_dat.MH.MH_dat.MH_ext
#define m_pktdat        M_dat.MH.MH_dat.MH_databuf
#define m_dat           M_dat.M_databuf

#define M_EXT           0x0001
#define M_PKTHDR        0x0002
#define M_EOR           0x0004
#define M_BCAST         0x0100
#define M_MCAST         0x0200
#define M_COPYFLAGS     (M_PKTHDR | M_EOR | M_BCAST | M_MCAST)
#define M_COPYALL       1000000000

#define mtod(m, t)      ((t)((m)->m_data))
/* Same result as the NDK's `(struct mbuf *)((ULONG)(x) & ~(MSIZE-1))`, phrased
   so it also works where a pointer is wider than ULONG. */
#define dtom(x)         ((struct mbuf *)(void *)((char *)(x) -              \
                            (long)((unsigned long)(x) & (MSIZE - 1))))

struct mbstat {
    ULONG m_mbufs;
    ULONG m_clusters;
    ULONG m_spare;
    ULONG m_clfree;
    ULONG m_drops;
    ULONG m_wait;
    ULONG m_drain;
};

#else /* !AMI_MBUF_REPLICA, the real thing */

/* <sys/mbuf.h> reaches <sys/socket.h>, which uses ssize_t without declaring
   it on this toolchain: <sys/types.h> first, or the include fails. */
#include <sys/types.h>
#include <sys/mbuf.h>

#endif /* AMI_MBUF_REPLICA */

/* ------------------------------------------------------- pinned constants */

/* The 68k ABI. Both the replica above and mbuf_abi_check.c assert on these. */
#define AMI_MBUF_SIZE           128     /* == MSIZE                          */
#define AMI_MBUF_HDR_SIZE        20
#define AMI_MBUF_PKTHDR_SIZE      8
#define AMI_MBUF_EXT_SIZE        12
#define AMI_MBUF_MLEN           108
#define AMI_MBUF_MHLEN          100

#define AMI_MBUF_OFF_NEXT         0
#define AMI_MBUF_OFF_NEXTPKT      4
#define AMI_MBUF_OFF_DATA         8
#define AMI_MBUF_OFF_LEN         12
#define AMI_MBUF_OFF_TYPE        16
#define AMI_MBUF_OFF_FLAGS       18
#define AMI_MBUF_OFF_PKTHDR      20
#define AMI_MBUF_OFF_DAT         20
#define AMI_MBUF_OFF_EXT         28
#define AMI_MBUF_OFF_PKTDAT      28

/*
 * MSIZE must be a power of two or dtom() is meaningless, and the internal data
 * area must end exactly at the end of the mbuf: cat/prepend/pullup use
 * ((UBYTE *)m + MSIZE) as the limit for both m_dat and m_pktdat.
 */
AMI_STATIC_ASSERT((MSIZE & (MSIZE - 1)) == 0,            "MSIZE must be a power of two");
AMI_STATIC_ASSERT(sizeof(struct mbuf) == MSIZE,          "mbuf must be exactly MSIZE");
AMI_STATIC_ASSERT(sizeof(struct m_hdr) + MLEN == MSIZE,  "m_dat must end at MSIZE");
AMI_STATIC_ASSERT(sizeof(struct m_hdr) + sizeof(struct pkthdr) + MHLEN == MSIZE,
                  "m_pktdat must end at MSIZE");
AMI_STATIC_ASSERT(MCLBYTES == 2048,                      "MCLBYTES");

#ifndef AMI_MBUF_REPLICA
/* Run against the NDK <sys/mbuf.h> itself, so a silent header substitution
   cannot get past this point. */
AMI_STATIC_ASSERT(sizeof(struct mbuf)   == AMI_MBUF_SIZE,        "mbuf size");
AMI_STATIC_ASSERT(sizeof(struct m_hdr)  == AMI_MBUF_HDR_SIZE,    "m_hdr size");
AMI_STATIC_ASSERT(sizeof(struct pkthdr) == AMI_MBUF_PKTHDR_SIZE, "pkthdr size");
AMI_STATIC_ASSERT(MSIZE == AMI_MBUF_SIZE,   "MSIZE");
AMI_STATIC_ASSERT(MLEN  == AMI_MBUF_MLEN,   "MLEN");
AMI_STATIC_ASSERT(MHLEN == AMI_MBUF_MHLEN,  "MHLEN");
#endif

/* ------------------------------------------------------------ mbuf types */

/*
 * The NDK header defines mh_type but not these. 4.4BSD values, inferred and
 * not confirmed against any Amiga header.
 */
#define MT_FREE         0
#define MT_DATA         1
#define MT_HEADER       2
#define MT_SOCKET       3
#define MT_PCB          4
#define MT_RTABLE       5
#define MT_HTABLE       6
#define MT_ATABLE       7
#define MT_SONAME       8
#define MT_SOOPTS      10
#define MT_FTABLE      11
#define MT_RIGHTS      12
#define MT_IFADDR      13
#define MT_CONTROL     14
#define MT_OOBDATA     15

/* ------------------------------------------------------------- lifecycle */

/*
 * Optional; every entry point below self-initialises with the defaults. Pass 0
 * for either argument to keep the default. Returns 0, or -1 if the pool is
 * already up with different ceilings.
 */
LONG ami_mbuf_init(ULONG max_mbufs, ULONG max_clusters);

/* Release every slab and cluster. Only safe when no mbuf is outstanding; it
   does not chase live chains. */
VOID ami_mbuf_cleanup(VOID);

/* Fill in a 4.4BSD mbstat. m_spare, m_wait and m_drain are always 0. */
VOID ami_mbuf_stats(struct mbstat *out);

/* Outstanding (allocated but not freed) mbufs and clusters, for leak tests. */
ULONG ami_mbuf_outstanding(VOID);
ULONG ami_mbuf_clusters_outstanding(VOID);

/* ------------------------------------------------------ the eleven vectors */

/*
 * 1:1 with the bsdsocket.library LVOs; each vector is a one-line forward to
 * the function below, argument order and return type already matching.
 */

/* Plain mbuf, MT_DATA, m_len 0, data at m_dat. NULL when the pool is out. */
struct mbuf *ami_mbuf_get(VOID);

/* Packet-header mbuf: M_PKTHDR set, data at m_pktdat, pkthdr zeroed. */
struct mbuf *ami_mbuf_gethdr(VOID);

/* Free one mbuf, return its successor (4.4BSD m_free). NULL in, NULL out. */
struct mbuf *ami_mbuf_free(struct mbuf *m);

/* Free a whole chain (4.4BSD m_freem). Does not follow m_nextpkt. */
VOID ami_mbuf_freem(struct mbuf *m);

/*
 * Trim req_len bytes off the front (> 0) or the tail (< 0). Never frees mbufs:
 * emptied ones stay in the chain with m_len 0. Updates m_pkthdr.len when
 * M_PKTHDR is set. Returns 0, or -1 if mp is NULL.
 */
LONG ami_mbuf_adj(struct mbuf *mp, LONG req_len);

/*
 * 4.4BSD m_cat: n is consumed either way. Returns 0, or -1 if either argument
 * is NULL. Does NOT update m's pkthdr.len; the caller must.
 */
LONG ami_mbuf_cat(struct mbuf *m, struct mbuf *n);

/*
 * Write len bytes from cp into the chain at offset off, extending the chain
 * and zero-filling any gap (4.4BSD m_copyback). Returns 0, or -1 if the chain
 * could not be extended.
 */
LONG ami_mbuf_copyback(struct mbuf *m, LONG off, LONG len, APTR cp);

/*
 * Read len bytes out of the chain at offset off into cp (4.4BSD m_copydata).
 * Returns 0, or -1 if the chain is shorter than off+len, where 4.4BSD would
 * panic. Nothing is written to cp on failure.
 */
LONG ami_mbuf_copydata(struct mbuf *m, LONG off, LONG len, APTR cp);

/*
 * 4.4BSD m_copym; len may be M_COPYALL. Clusters we own are shared by
 * reference count, foreign M_EXT storage deep-copied. Returns NULL on bad
 * arguments or exhaustion; m is left untouched.
 */
struct mbuf *ami_mbuf_copym(struct mbuf *m, LONG off, LONG len);

/*
 * 4.4BSD m_prepend. The new bytes are not initialised. Returns the new head,
 * or NULL; on failure the WHOLE CHAIN is freed.
 */
struct mbuf *ami_mbuf_prepend(struct mbuf *m, LONG len);

/*
 * Make the first len bytes of the chain contiguous in the head mbuf
 * (4.4BSD m_pullup). len must fit in one mbuf's internal storage.
 * Returns the new head, or NULL; on failure the whole chain is freed.
 */
struct mbuf *ami_mbuf_pullup(struct mbuf *m, LONG len);

/* --------------------------------------------------------------- helpers */

/* Total m_len over the chain (does not trust m_pkthdr.len). */
ULONG ami_mbuf_length(const struct mbuf *m);

/* Attach a fresh MCLBYTES cluster to m, moving nothing. 0 on success. */
LONG ami_mbuf_clget(struct mbuf *m);

/* ami_mbuf_get() + ami_mbuf_clget(); frees the mbuf if the cluster fails. */
struct mbuf *ami_mbuf_getcl(VOID);

/* Build a chain holding `len` bytes copied from `src` (NULL leaves the bytes
   uninitialised). Uses clusters above MINCLSIZE. NULL on failure. */
struct mbuf *ami_mbuf_build(const void *src, ULONG len, BOOL want_pkthdr);

/* -------------------------------------------------------- NX_PACKET bridge */

/* Declared only when nx_api.h is already in scope: include "tx_api.h" then
   "nx_api.h", before any exec header, ahead of this one. */
#ifdef NX_API_H

/*
 * Copy [prepend_ptr, append_ptr) across the whole NX_PACKET chain into a
 * fresh mbuf chain. The packet is not consumed. NULL on failure.
 */
struct mbuf *ami_mbuf_from_packet(NX_PACKET *packet, BOOL want_pkthdr);

/*
 * Allocate an NX_PACKET from `pool` and append the mbuf chain's data to it.
 * The mbuf chain is not consumed. Returns NX_SUCCESS, or the NetX Duo error;
 * *out is only written on success.
 */
UINT ami_mbuf_to_packet(struct mbuf *m, NX_PACKET_POOL *pool, ULONG wait_option,
                        NX_PACKET **out);

#endif /* NX_API_H */

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_MBUF_H */
