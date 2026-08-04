/*
 * AmiNetXDuo -- 4.4BSD mbuf emulation behind the mbuf_* LVOs.
 *
 * NetX Duo has no mbuf. An NX_PACKET is one header plus one payload window
 * (prepend_ptr..append_ptr) inside a fixed-size pool block, chained through
 * nx_packet_next. An mbuf is a 128-byte object that either carries its data
 * inside itself or points at external storage, and whose head can be trimmed,
 * extended and re-spliced. The two shapes do not overlap, so this is an
 * emulation written from scratch rather than a wrapper.
 *
 * An mbuf does not own, wrap or reference an NX_PACKET.
 *
 * mbufs come out of a private slab allocator (see src/mbuf/mbuf_alloc.c) and
 * carry their own storage: the internal 108/100-byte data area, or an M_EXT
 * cluster of MCLBYTES from a private cluster pool. Three reasons, in order of
 * how load-bearing they are:
 *
 *   1. dtom(x) is `(struct mbuf *)((ULONG)(x) & ~(MSIZE-1))`. It is part of
 *      the published header, so a caller may use it. That only works if every
 *      mbuf is MSIZE-aligned and its data lives inside itself. Data pointing
 *      into an NX_PACKET payload would make dtom() return garbage.
 *   2. NX_PACKETs are a pinned, fixed-count pool resource sized for the 1 MB
 *      floor (AMI_POOL_MIN_PACKETS is 16, and the SANA-II readers already pin
 *      6-8 of them). Handing one to an application that may never free it
 *      would starve the receive path.
 *   3. mbuf_prepend/mbuf_adj/mbuf_pullup/mbuf_cat mutate a chain in ways an
 *      NX_PACKET cannot express -- splitting one buffer into two, growing the
 *      front past data_start, splicing foreign storage in with M_EXT.
 *
 * Conversion is therefore an explicit copy at the boundary:
 * ami_mbuf_from_packet() and ami_mbuf_to_packet() (declared at the bottom of
 * this header when nx_api.h is in scope).
 *
 * Clusters / M_EXT:
 *
 *   Supported:
 *     - Reading, copying, trimming and freeing M_EXT mbufs correctly, because
 *       mbuf_copym / mbuf_copydata / mbuf_adj / mbuf_free must handle them.
 *     - Our own MCLBYTES clusters, reference-counted so mbuf_copym can share
 *       rather than copy them (there is no refcount field in `struct m_ext`,
 *       so the count lives in a private header in front of ext_buf; ours are
 *       identified by pointer identity against the cluster list, never by
 *       reading memory we do not own).
 *     - Foreign M_EXT storage attached by a caller: ext_free is honoured when
 *       the last reference goes, and mbuf_copym deep-copies rather than
 *       sharing, because a foreign buffer has no refcount we may touch.
 *
 *   Not supported:
 *     - There is no MCLGET LVO, so applications cannot ask for a cluster
 *       through the ABI at all. ami_mbuf_clget() exists for our own use
 *       (mostly ami_mbuf_from_packet on a large frame).
 *     - No mbuf "wait" mode. The BSD `how` argument (M_WAIT/M_DONTWAIT) is not
 *       in any of these LVO signatures; every allocation here is non-blocking
 *       and returns NULL on exhaustion. mbstat.m_wait therefore stays 0.
 *     - No m_devget/m_pullup2/m_split/m_dup; not in the ABI.
 *
 * ABI provenance: the layout below is the Roadshow netinclude `<sys/mbuf.h>`
 * (Olaf Barthel, 2001-2016, over the 4.4BSD `@(#)mbuf.h 8.5 (Berkeley)
 * 2/19/95` header), as shipped in this toolchain's ndk-include. That is the
 * Amiga lineage, not newlib's -- newlib has no <sys/mbuf.h> at all, so the
 * pwd.h substitution trap (docs/RESEARCH.md 3.3) cannot happen here.
 *
 * On the Amiga this header includes that NDK header directly, so the ABI is the
 * real one by construction. AMI_MBUF_REPLICA builds a byte-identical replica
 * for the host test instead. Either way every offset is pinned below with
 * _Static_assert, and src/mbuf/mbuf_abi_check.c re-pins them against the NDK
 * header in a translation unit of its own.
 *
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
 * Host-test replica: structurally the NDK struct, field for field, in the same
 * order.
 *
 * It cannot be byte-identical on a 64-bit host, because three of the six m_hdr
 * fields are pointers. MSIZE therefore scales with the pointer width and MLEN /
 * MHLEN follow, which keeps every property the code relies on (MSIZE a power of
 * two, the internal data area ending exactly at the end of the mbuf, dtom()
 * meaningful) while the arithmetic constants differ. The exact 68k offsets are
 * asserted only on the Amiga, where they are the ABI -- see the assert block
 * below and src/mbuf/mbuf_abi_check.c.
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

#else /* !AMI_MBUF_REPLICA -- the real thing */

/*
 * <sys/mbuf.h> reaches <net/if.h> -> <sys/socket.h>, which uses ssize_t
 * without declaring it on this newlib-based toolchain. <sys/types.h> first,
 * or the include fails.
 */
#include <sys/types.h>
#include <sys/mbuf.h>

#endif /* AMI_MBUF_REPLICA */

/* ------------------------------------------------------- pinned constants */

/*
 * Measured 2026-07-24 with m68k-amigaos-gcc 15.2.0 -m68020 against the NDK
 * <sys/mbuf.h>. Both the replica above and mbuf_abi_check.c assert against
 * these.
 */
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
 * These hold in both builds and the implementation leans on all three. MSIZE
 * must be a power of two or dtom() is meaningless, and the internal data area
 * must end exactly at the end of the mbuf, because ami_mbuf_cat/prepend/pullup
 * use ((UBYTE *)m + MSIZE) as the limit for both m_dat and m_pktdat without
 * caring which one m_data points into.
 */
AMI_STATIC_ASSERT((MSIZE & (MSIZE - 1)) == 0,            "MSIZE must be a power of two");
AMI_STATIC_ASSERT(sizeof(struct mbuf) == MSIZE,          "mbuf must be exactly MSIZE");
AMI_STATIC_ASSERT(sizeof(struct m_hdr) + MLEN == MSIZE,  "m_dat must end at MSIZE");
AMI_STATIC_ASSERT(sizeof(struct m_hdr) + sizeof(struct pkthdr) + MHLEN == MSIZE,
                  "m_pktdat must end at MSIZE");
AMI_STATIC_ASSERT(MCLBYTES == 2048,                      "MCLBYTES");

#ifndef AMI_MBUF_REPLICA
/*
 * The real 68k ABI. These run against the NDK <sys/mbuf.h> itself, so a
 * silent header substitution of the kind that bit <pwd.h> (docs/RESEARCH.md
 * 3.3) cannot get past this point.
 */
AMI_STATIC_ASSERT(sizeof(struct mbuf)   == AMI_MBUF_SIZE,        "mbuf size");
AMI_STATIC_ASSERT(sizeof(struct m_hdr)  == AMI_MBUF_HDR_SIZE,    "m_hdr size");
AMI_STATIC_ASSERT(sizeof(struct pkthdr) == AMI_MBUF_PKTHDR_SIZE, "pkthdr size");
AMI_STATIC_ASSERT(MSIZE == AMI_MBUF_SIZE,   "MSIZE");
AMI_STATIC_ASSERT(MLEN  == AMI_MBUF_MLEN,   "MLEN");
AMI_STATIC_ASSERT(MHLEN == AMI_MBUF_MHLEN,  "MHLEN");
#endif

/* ------------------------------------------------------------ mbuf types */

/*
 * The NDK header defines mh_type but not the MT_* constants that go in it --
 * they live in the 4.4BSD <sys/mbuf.h> section Roadshow dropped. These are the
 * 4.4BSD values; only MT_FREE, MT_DATA and MT_HEADER are used by this code.
 * Inferred from 4.4BSD, not confirmed against any Amiga header.
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
 * Optional. Sets the ceilings and pre-creates nothing; every entry point
 * below self-initialises with the defaults if this was never called. Pass 0
 * for either argument to keep the default (AMI_MBUF_DEFAULT_MBUFS /
 * AMI_MBUF_DEFAULT_CLUSTERS in src/mbuf/mbuf_internal.h).
 *
 * Returns 0, or -1 if the pool is already up with different ceilings.
 */
LONG ami_mbuf_init(ULONG max_mbufs, ULONG max_clusters);

/*
 * Release every slab and cluster. Only safe when no mbuf is outstanding; it
 * does not chase live chains.
 *
 * Nothing in production calls this, and nothing can: the eleven mbuf_* LVOs
 * are bsd_enosys stubs and aminetxduo_mbuf is not linked into
 * bsdsocket.library at all, so the pool never comes up and there is nothing to
 * release. Wiring any of those LVOs to this pool is three steps, not one --
 * link the library, and call this from ami_ns_destroy() after nx_ip_delete()
 * and the SANA-II closes, which is where ami_bpf_cleanup() is called from for
 * the same reason. Do all three or the slabs are resident until reboot.
 */
VOID ami_mbuf_cleanup(VOID);

/* Fill in a 4.4BSD mbstat. m_spare, m_wait and m_drain are always 0. */
VOID ami_mbuf_stats(struct mbstat *out);

/* Outstanding (allocated but not freed) mbufs and clusters -- for leak tests. */
ULONG ami_mbuf_outstanding(VOID);
ULONG ami_mbuf_clusters_outstanding(VOID);

/* ------------------------------------------------------ the eleven vectors */

/*
 * These map 1:1 onto the bsdsocket.library LVOs. Register assignments, from
 * the NDK pragmas/bsdsocket_pragmas.h and confirmed against amitools'
 * bsdsocket_lib.fd and ndk-include/interfaces/bsdsocket.h:
 *
 *   0x28e mbuf_get      ()                       -- no arguments
 *   0x294 mbuf_gethdr   ()                       -- no arguments
 *   0x282 mbuf_free     (m)            a0
 *   0x288 mbuf_freem    (m)            a0
 *   0x2a6 mbuf_adj      (mp,req_len)   a0/d0
 *   0x2a0 mbuf_cat      (m,n)          a0/a1
 *   0x276 mbuf_copyback (m,off,len,cp) a0/d0/d1/a1
 *   0x27c mbuf_copydata (m,off,len,cp) a0/d0/d1/a1
 *   0x270 mbuf_copym    (m,off,len)    a0/d0/d1
 *   0x29a mbuf_prepend  (m,len)        a0/d0
 *   0x2ac mbuf_pullup   (m,len)        a0/d0
 *
 * The bsdsocket vector for each is a one-line forward to the function below;
 * argument order and return type already match.
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
 * Trim req_len bytes off the front (req_len > 0) or the tail (req_len < 0).
 * Never frees mbufs: emptied ones stay in the chain with m_len 0, as 4.4BSD
 * m_adj does. Updates m_pkthdr.len when M_PKTHDR is set.
 * Returns 0, or -1 if mp is NULL.
 */
LONG ami_mbuf_adj(struct mbuf *mp, LONG req_len);

/*
 * Append chain n to chain m, compacting n's data into m's trailing free space
 * where it fits (4.4BSD m_cat). n is consumed either way. Returns 0, or -1 if
 * either argument is NULL. Does not update m's pkthdr.len; 4.4BSD m_cat does
 * not either, and callers that care fix it up themselves.
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
 * Copy len bytes starting at off into a fresh chain (4.4BSD m_copym).
 * len may be M_COPYALL. Clusters we own are shared by reference count;
 * foreign M_EXT storage is deep-copied. If off is 0 and m has M_PKTHDR the
 * copy gets one too, with pkthdr.len set to the number of bytes copied.
 * Returns NULL on bad arguments or exhaustion; m is left untouched.
 */
struct mbuf *ami_mbuf_copym(struct mbuf *m, LONG off, LONG len);

/*
 * Make room for len bytes at the front of the chain (4.4BSD m_prepend), by
 * moving m_data back if there is leading space in a buffer we own, otherwise
 * by pushing a new head mbuf on. The new bytes are not initialised.
 * Returns the new head, or NULL; on failure the whole chain is freed, as in
 * 4.4BSD, which is the usual way callers leak if they forget.
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

/*
 * Build a chain holding `len` bytes copied from `src` (may be NULL to leave
 * the bytes uninitialised). Uses clusters above MINCLSIZE. NULL on failure.
 * This is the workhorse behind ami_mbuf_from_packet().
 */
struct mbuf *ami_mbuf_build(const void *src, ULONG len, BOOL want_pkthdr);

/* -------------------------------------------------------- NX_PACKET bridge */

/*
 * Declared only when nx_api.h is already in scope: include "tx_api.h" and
 * "nx_api.h" (in that order, before any exec header) ahead of this one to get
 * them. Both are copies; see the top of this file.
 */
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
