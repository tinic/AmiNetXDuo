/*
 * tls.library, TLS 1.2 session resumption: RFC 5077 tickets, RFC 5246 session
 * IDs, RFC 7627 extended master secret required for anything cached.
 *
 * Two vendored symbols are redirected with -Wl,--wrap.  This translation unit
 * must be compiled WITHOUT LTO: GNU ld only redirects undefined references,
 * and GCC's WPA otherwise binds and internalizes the pair before ld sees it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* RFC 5077.  nx_secure has no name for it because it has no support for it. */
#define TLS_EXT_SESSION_TICKET      0x0023

#define TLS_R_FNV_OFFSET            2166136261UL
#define TLS_R_FNV_PRIME             16777619UL

/*
 * -DTLS_RESUME_TRACE puts a running commentary on the serial port.  Off in
 * every shipping build.
 */
#ifdef TLS_RESUME_TRACE

#include <inline/macros.h>

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#include <stdarg.h>

static VOID tls_trace_char(register UBYTE c      __asm("d0"),
                           register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        RawPutChar(c);
}

VOID tls_trace(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) tls_trace_char, NULL);
    va_end(args);

    RawPutChar('\n');
}

#endif  /* TLS_RESUME_TRACE */

/*
 * The on-disk mirror.  The magic is the format version: 'ATS2' held sessions
 * negotiated without the extended master secret, which are exactly the ones
 * now refused.  An unrecognised magic is ignored, not an error.
 */
#define TLS_SESSIONS_MAGIC          0x41545333UL    /* 'ATS3' */
#define TLS_SESSIONS_HEADER         16UL
#define TLS_SESSIONS_RECORD         424UL   /* 168 + TLS_RESUME_TICKET_MAX */

/*
 * The direct entry points, not the nx_/tx_ spellings: those map to the
 * argument-checking nxe_/txe_ wrappers, whose archive members reference
 * ThreadX data symbols that tls_netx.c cannot forward.
 */
extern UINT _nx_packet_release(NX_PACKET *packet_ptr);

/* The machine's entropy pool, borrowed from bsdsocket.library through
   src/tlslib/tls_netx.c.  One pool per machine, not two. */
extern int ami_random_rand(void);

extern UINT __real__nx_secure_tls_send_clienthello(NX_SECURE_TLS_SESSION *tls_session,
                                                   NX_PACKET *send_packet);
extern UINT __real__nx_secure_tls_client_handshake(NX_SECURE_TLS_SESSION *tls_session,
                                                   UCHAR *packet_buffer,
                                                   UINT data_length,
                                                   ULONG wait_option);

static ULONG tls_r_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] <<  8) |  (ULONG)p[3];
}

static UWORD tls_r_be16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

static VOID tls_r_put32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

static VOID tls_r_put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)v;
}

static VOID tls_r_copy(UBYTE *dst, const UBYTE *src, ULONG n)
{
    while (n-- > 0)
        *dst++ = *src++;
}

static BOOL tls_r_equal(const UBYTE *a, const UBYTE *b, ULONG n)
{
    while (n-- > 0)
    {
        if (*a++ != *b++)
            return FALSE;
    }

    return TRUE;
}

/*
 * Case-insensitive, because both things compared with this are: DNS names by
 * definition, AmigaOS file names by the filesystem.
 */
static BOOL tls_r_str_equal(const char *a, const char *b, ULONG limit)
{
    ULONG i;

    for (i = 0; i < limit; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)
            return FALSE;
        if (ca == '\0')
            return TRUE;
    }

    return TRUE;
}

static BOOL tls_r_host_equal(const char *a, const char *b)
{
    return tls_r_str_equal(a, b, TLS_RESUME_HOST_MAX);
}

static BOOL tls_r_path_equal(const char *a, const char *b)
{
    return tls_r_str_equal(a, b, TLS_STORE_PATH_MAX);
}

/*
 * In the library base, and mirrored to DEVS:Internet/tlssessions.  Each entry
 * holds a 48-byte master secret and a ticket in the clear, so the file is as
 * sensitive as the sessions it stands for; TLSA_NoResume turns it off.
 */

static TLSResumeEntry *tls_resume_table(struct TLSLibBase *base)
{
    if (base->tb_Sessions == NULL)
    {
        base->tb_Sessions = (TLSResumeEntry *)
            tls_alloc(TLS_RESUME_SLOTS * (ULONG)sizeof(TLSResumeEntry));
    }

    return base->tb_Sessions;
}

/*
 * A resumed handshake checks nothing -- no certificate, no signature, no host
 * name -- so the cache key must name the trust decision completely: host,
 * port, trust-store fingerprint, TLSRE_VERIFIED, TLSRE_DATED, TLSA_MaxChain.
 */
static UBYTE tls_resume_flags(const TLSConnection *conn)
{
    UBYTE flags = 0;

    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
        flags = (UBYTE)(flags | TLSRE_VERIFIED);
    if (conn->tc_ExpiryChecked)
        flags = (UBYTE)(flags | TLSRE_DATED);

    return flags;
}

static ULONG tls_resume_trust_key(const TLSConnection *conn)
{
    ULONG hash = TLS_R_FNV_OFFSET;
    ULONG store = (conn->tc_Store != NULL) ? conn->tc_Store->ts_Fingerprint : 0;

    hash ^= (ULONG)tls_resume_flags(conn);  hash *= TLS_R_FNV_PRIME;
    hash ^= (ULONG)conn->tc_RemoteCount;    hash *= TLS_R_FNV_PRIME;
    hash ^= store;                          hash *= TLS_R_FNV_PRIME;

    /* Zero means "not set" in a decoded record, so it must not be a real key.
       Otherwise a stale or truncated file matches a live one. */
    return (hash == 0) ? TLS_R_FNV_PRIME : hash;
}

/* Has this entry aged out?  The rule is tls_expiry.c, which is host-tested. */
static BOOL tls_resume_expired(const TLSResumeEntry *e, ULONG now)
{
    return tls_expired((unsigned long)e->re_Stamp,
                       (unsigned long)e->re_Lifetime,
                       (unsigned long)now) ? TRUE : FALSE;
}

/*
 * Host and port name who, and the trust key names under what.  Both must
 * agree, because a resumed handshake re-checks neither; re_Flags is compared
 * as well as re_TrustKey, because the rule is to lean toward a refusal.
 */
static TLSResumeEntry *tls_resume_find(TLSResumeEntry *table, const char *host,
                                       UWORD port, UBYTE flags, ULONG trust_key)
{
    ULONG i;

    for (i = 0; i < TLS_RESUME_SLOTS; i++)
    {
        if (table[i].re_Valid == 0)
            continue;
        if (table[i].re_Port != port)
            continue;
        if (table[i].re_TrustKey != trust_key)
            continue;
        if (table[i].re_Flags != flags)
            continue;
        if (tls_r_host_equal(table[i].re_Host, host))
            return &table[i];
    }

    return NULL;
}

/* A free slot, the matching slot, or the least recently used one. */
static TLSResumeEntry *tls_resume_slot(TLSResumeEntry *table, const char *host,
                                       UWORD port, UBYTE flags, ULONG trust_key)
{
    TLSResumeEntry *victim;
    ULONG           i;

    victim = tls_resume_find(table, host, port, flags, trust_key);
    if (victim != NULL)
        return victim;

    for (i = 0; i < TLS_RESUME_SLOTS; i++)
    {
        if (table[i].re_Valid == 0)
            return &table[i];
    }

    victim = &table[0];
    for (i = 1; i < TLS_RESUME_SLOTS; i++)
    {
        if (table[i].re_Serial < victim->re_Serial)
            victim = &table[i];
    }

    return victim;
}

/*
 * 'ATS3', big-endian, fixed 424-byte records so a truncated or corrupt file
 * cannot be mis-parsed into a wild pointer: it is either a whole number of
 * records or it is rejected.
 */

static VOID tls_resume_encode(const TLSResumeEntry *e, UBYTE *rec)
{
    ULONG i;

    for (i = 0; i < TLS_SESSIONS_RECORD; i++)
        rec[i] = 0;

    for (i = 0; i < TLS_RESUME_HOST_MAX; i++)
        rec[i] = (UBYTE)e->re_Host[i];

    tls_r_put16(&rec[64], e->re_Port);
    tls_r_put16(&rec[66], e->re_CipherSuite);
    tls_r_put16(&rec[68], e->re_Protocol);
    rec[70] = e->re_SidLength;
    rec[71] = e->re_Flags;
    tls_r_copy(&rec[72], e->re_Sid, TLS_RESUME_SID_MAX);
    tls_r_put32(&rec[104], e->re_Stamp);
    tls_r_put32(&rec[108], e->re_Lifetime);
    tls_r_put32(&rec[112], e->re_TrustKey);
    rec[116] = e->re_MaxChain;
    tls_r_put16(&rec[118], e->re_TicketLength);
    tls_r_copy(&rec[120], e->re_Master, TLS_MASTER_SECRET_SIZE);
    tls_r_copy(&rec[168], e->re_Ticket, TLS_RESUME_TICKET_MAX);

    /* The record layout and the structure must not drift apart.  The file is
       read back by a different build of this same library. */
    _Static_assert((168UL + TLS_RESUME_TICKET_MAX) == TLS_SESSIONS_RECORD,
                   "TLS_SESSIONS_RECORD must match the ticket size");
}

static BOOL tls_resume_decode(TLSResumeEntry *e, const UBYTE *rec)
{
    ULONG i;

    for (i = 0; i < TLS_RESUME_HOST_MAX; i++)
        e->re_Host[i] = (char)rec[i];
    e->re_Host[TLS_RESUME_HOST_MAX - 1] = '\0';

    e->re_Port         = tls_r_be16(&rec[64]);
    e->re_CipherSuite  = tls_r_be16(&rec[66]);
    e->re_Protocol     = tls_r_be16(&rec[68]);
    e->re_SidLength    = rec[70];
    e->re_Flags        = rec[71];
    tls_r_copy(e->re_Sid, &rec[72], TLS_RESUME_SID_MAX);
    e->re_Stamp        = tls_r_be32(&rec[104]);
    e->re_Lifetime     = tls_r_be32(&rec[108]);
    e->re_TrustKey     = tls_r_be32(&rec[112]);
    e->re_MaxChain     = rec[116];
    e->re_TicketLength = tls_r_be16(&rec[118]);
    tls_r_copy(e->re_Master, &rec[120], TLS_MASTER_SECRET_SIZE);
    tls_r_copy(e->re_Ticket, &rec[168], TLS_RESUME_TICKET_MAX);

    /* Anything the file claims that the structure cannot hold is a corrupt
       file, not a session.  Refuse the record rather than clamping it. */
    if (e->re_Host[0] == '\0' ||
        e->re_SidLength > TLS_RESUME_SID_MAX ||
        e->re_TicketLength > TLS_RESUME_TICKET_MAX)
    {
        return FALSE;
    }

    /*
     * The live trust key is never zero, so a zero-key record is a truncated or
     * zero-filled file rather than a session.
     */
    if (e->re_TrustKey == 0)
        return FALSE;

    if (e->re_SidLength == 0 && e->re_TicketLength == 0)
        return FALSE;

    e->re_Valid = 1;

    return TRUE;
}

/*
 * Read the mirror into the resident cache.  Once per library lifetime for a
 * given path, and again if a connection names a different path, because
 * TLSA_SessionFile must mean the same thing on the read as on the write.
 */
static VOID tls_resume_load(struct TLSLibBase *base, const char *path)
{
    TLSResumeEntry *table;
    UBYTE           header[TLS_SESSIONS_HEADER];
    UBYTE          *rec;
    BPTR            fh;
    ULONG           count;
    ULONG           i;
    ULONG           used = 0;

    if (path == NULL)
        path = "";

    if (base->tb_SessionsLoaded && tls_r_path_equal(base->tb_SessionPath, path))
        return;

    table = tls_resume_table(base);
    if (table == NULL)
        return;

    /* A different file is a different cache, not an addition to this one. */
    if (base->tb_SessionsLoaded)
    {
        for (i = 0; i < TLS_RESUME_SLOTS; i++)
            tls_bzero(&table[i], sizeof(TLSResumeEntry));
    }

    base->tb_SessionsLoaded = TRUE;
    tls_strncpy(base->tb_SessionPath, path, sizeof(base->tb_SessionPath));

    if (path[0] == '\0' || DOSBase == NULL)
        return;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
        return;

    if (Read(fh, header, (LONG)TLS_SESSIONS_HEADER) != (LONG)TLS_SESSIONS_HEADER ||
        tls_r_be32(header) != TLS_SESSIONS_MAGIC)
    {
        Close(fh);
        return;
    }

    count = tls_r_be32(&header[4]);
    if (count > TLS_RESUME_SLOTS)
        count = TLS_RESUME_SLOTS;

    rec = (UBYTE *)tls_alloc(TLS_SESSIONS_RECORD);
    if (rec == NULL)
    {
        Close(fh);
        return;
    }

    for (i = 0; i < count; i++)
    {
        if (Read(fh, rec, (LONG)TLS_SESSIONS_RECORD) != (LONG)TLS_SESSIONS_RECORD)
            break;

        if (!tls_resume_decode(&table[used], rec))
        {
            tls_bzero(&table[used], sizeof(TLSResumeEntry));
            continue;
        }

        table[used].re_Serial = ++base->tb_SessionSerial;
        used++;
    }

    tls_free(rec);
    Close(fh);
}

static VOID tls_resume_save(struct TLSLibBase *base, const char *path)
{
    TLSResumeEntry *table = base->tb_Sessions;
    UBYTE           header[TLS_SESSIONS_HEADER];
    UBYTE          *rec;
    BPTR            fh;
    ULONG           count = 0;
    ULONG           i;

    if (table == NULL || path == NULL || path[0] == '\0' || DOSBase == NULL)
        return;

    for (i = 0; i < TLS_RESUME_SLOTS; i++)
    {
        if (table[i].re_Valid != 0)
            count++;
    }

    rec = (UBYTE *)tls_alloc(TLS_SESSIONS_RECORD);
    if (rec == NULL)
        return;

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        tls_free(rec);
        return;
    }

    for (i = 0; i < TLS_SESSIONS_HEADER; i++)
        header[i] = 0;
    tls_r_put32(&header[0], TLS_SESSIONS_MAGIC);
    tls_r_put32(&header[4], count);

    if (Write(fh, header, (LONG)TLS_SESSIONS_HEADER) == (LONG)TLS_SESSIONS_HEADER)
    {
        for (i = 0; i < TLS_RESUME_SLOTS; i++)
        {
            if (table[i].re_Valid == 0)
                continue;

            tls_resume_encode(&table[i], rec);

            if (Write(fh, rec, (LONG)TLS_SESSIONS_RECORD) != (LONG)TLS_SESSIONS_RECORD)
                break;
        }
    }

    Close(fh);
    tls_free(rec);
}

ULONG tls_resume_count(struct TLSLibBase *base)
{
    ULONG count = 0;
    ULONG i;

    if (base == NULL || base->tb_Sessions == NULL)
        return 0;

    ObtainSemaphore(&base->tb_Lock);
    for (i = 0; i < TLS_RESUME_SLOTS; i++)
    {
        if (base->tb_Sessions[i].re_Valid != 0)
            count++;
    }
    ReleaseSemaphore(&base->tb_Lock);

    return count;
}

VOID tls_resume_prepare(TLSConnection *conn)
{
    struct TLSLibBase *base;
    TLSResumeEntry    *table;
    TLSResumeEntry    *entry;
    ULONG              now;

    if (conn == NULL || (conn->tc_ResumeFlags & TLSR_ENABLED) == 0)
        return;

    /*
     * No host name, no resumption: the cache is keyed by the name the caller
     * asked for, and a master secret reused against a different host is a
     * security bug rather than a missed optimisation.
     */
    if (conn->tc_HostNameLength == 0)
    {
        conn->tc_ResumeFlags &= ~TLSR_ENABLED;
        return;
    }

    if (conn->tc_HostNameLength >= TLS_RESUME_HOST_MAX)
    {
        conn->tc_ResumeFlags &= ~TLSR_ENABLED;
        return;
    }

    base = conn->tc_Base;
    now  = tls_time_monotonic();

    ObtainSemaphore(&base->tb_Lock);

    tls_resume_load(base, conn->tc_SessionPath);

    table = tls_resume_table(base);
    if (table == NULL)
    {
        ReleaseSemaphore(&base->tb_Lock);
        return;
    }

    entry = tls_resume_find(table, (const char *)conn->tc_HostName,
                            conn->tc_Port, tls_resume_flags(conn),
                            tls_resume_trust_key(conn));

    if (entry != NULL && tls_resume_expired(entry, now))
    {
        tls_bzero(entry, sizeof(TLSResumeEntry));
        entry = NULL;
    }

    if (entry != NULL)
    {
        tls_r_copy(conn->tc_OfferSid, entry->re_Sid, TLS_RESUME_SID_MAX);
        conn->tc_OfferSidLength = entry->re_SidLength;
        tls_r_copy(conn->tc_Master, entry->re_Master, TLS_MASTER_SECRET_SIZE);
        conn->tc_CipherSuite = (ULONG)entry->re_CipherSuite;
        conn->tc_Protocol    = (ULONG)entry->re_Protocol;

        conn->tc_TicketLength = entry->re_TicketLength;
        if (conn->tc_TicketLength > 0 && conn->tc_Ticket != NULL)
            tls_r_copy(conn->tc_Ticket, entry->re_Ticket, conn->tc_TicketLength);
        else
            conn->tc_TicketLength = 0;

        /*
         * RFC 5077 3.4: a client presenting a ticket MAY generate a session ID and a
         * server accepting it MUST echo it, and that echo is the only acceptance
         * signal a TLS 1.2 client gets.  Never cached: a reused handle is linkable.
         */
        if (conn->tc_OfferSidLength == 0 && conn->tc_TicketLength > 0)
        {
            ULONG i;

            for (i = 0; i < TLS_RESUME_SID_MAX; i += 2)
            {
                int r = ami_random_rand();

                conn->tc_OfferSid[i]     = (UBYTE)r;
                conn->tc_OfferSid[i + 1] = (UBYTE)(r >> 8);
            }

            conn->tc_OfferSidLength = TLS_RESUME_SID_MAX;
            conn->tc_ResumeFlags   |= TLSR_SID_GEN;
        }

        entry->re_Serial = ++base->tb_SessionSerial;

        conn->tc_ResumeFlags |= TLSR_OFFERED;

        tls_trace("[resume] offering %s: sid %ld ticket %ld",
                  (LONG)conn->tc_HostName, (LONG)conn->tc_OfferSidLength,
                  (LONG)conn->tc_TicketLength);
    }

    ReleaseSemaphore(&base->tb_Lock);
}

VOID tls_resume_evict(TLSConnection *conn)
{
    struct TLSLibBase *base;
    TLSResumeEntry    *entry;

    if (conn == NULL || conn->tc_HostNameLength == 0)
        return;

    base = conn->tc_Base;

    ObtainSemaphore(&base->tb_Lock);

    if (base->tb_Sessions != NULL)
    {
        entry = tls_resume_find(base->tb_Sessions,
                                (const char *)conn->tc_HostName, conn->tc_Port,
                                tls_resume_flags(conn),
                                tls_resume_trust_key(conn));
        if (entry != NULL)
        {
            tls_bzero(entry, sizeof(TLSResumeEntry));

            if ((conn->tc_ResumeFlags & TLSR_PERSIST) != 0)
                tls_resume_save(base, conn->tc_SessionPath);
        }
    }

    ReleaseSemaphore(&base->tb_Lock);
}

/*
 * In TLS 1.2 a master secret is bound to its handshake only by RFC 7627's
 * extended master secret.  TLS 1.3 derives everything through the transcript
 * already, so there is nothing to negotiate and nothing to refuse.
 */
static BOOL tls_resume_secret_bound(const NX_SECURE_TLS_SESSION *s)
{
#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    if (s->nx_secure_tls_1_3 == NX_TRUE)
        return TRUE;
#endif
    return (BOOL)(s->nx_secure_tls_extended_master_secret == NX_TRUE);
}

VOID tls_resume_record(TLSConnection *conn)
{
    struct TLSLibBase     *base;
    NX_SECURE_TLS_SESSION *s;
    TLSResumeEntry        *table;
    TLSResumeEntry        *entry;
    BOOL                   changed = FALSE;

    if (conn == NULL || (conn->tc_ResumeFlags & TLSR_ENABLED) == 0)
        return;
    if (conn->tc_HostNameLength == 0)
        return;

    base = conn->tc_Base;
    s    = &conn->tc_Session;

    ObtainSemaphore(&base->tb_Lock);

    table = tls_resume_table(base);
    if (table == NULL)
    {
        ReleaseSemaphore(&base->tb_Lock);
        return;
    }

    if ((conn->tc_ResumeFlags & TLSR_RESUMED) != 0)
    {
        /*
         * A resumed session keeps its master secret and issuance time: only a newly
         * issued replacement ticket may restart the server's lifetime hint.
         */
        entry = tls_resume_find(table, (const char *)conn->tc_HostName,
                                conn->tc_Port, tls_resume_flags(conn),
                                tls_resume_trust_key(conn));
        if (entry != NULL)
        {
            entry->re_Serial = ++base->tb_SessionSerial;

            if ((conn->tc_ResumeFlags & TLSR_TICKET_NEW) != 0 &&
                conn->tc_TicketLength > 0 && conn->tc_Ticket != NULL)
            {
                tls_r_copy(entry->re_Ticket, conn->tc_Ticket,
                           conn->tc_TicketLength);
                entry->re_TicketLength = conn->tc_TicketLength;
                entry->re_Lifetime     = conn->tc_TicketLifetime;
                entry->re_Stamp        = tls_time_monotonic();
                changed = TRUE;
            }
        }
    }
    else
    {
        BOOL have_ticket = (BOOL)(((conn->tc_ResumeFlags & TLSR_TICKET_NEW) != 0 &&
                                   conn->tc_TicketLength > 0 &&
                                   conn->tc_Ticket != NULL) ? TRUE : FALSE);
        BOOL have_sid    = (BOOL)((s->nx_secure_tls_session_id_length > 0 &&
                                   s->nx_secure_tls_session_id_length <=
                                       TLS_RESUME_SID_MAX) ? TRUE : FALSE);

        /*
         * RFC 7627 5.4: a session not negotiated with the extended master secret is
         * not cached, or a resumed handshake carries the authentication of neither
         * connection (the triple handshake).  The other half is tls_resume_accept().
         */
        if ((have_ticket || have_sid) && tls_resume_secret_bound(s))
        {
            entry = tls_resume_slot(table, (const char *)conn->tc_HostName,
                                    conn->tc_Port, tls_resume_flags(conn),
                                    tls_resume_trust_key(conn));

            tls_bzero(entry, sizeof(TLSResumeEntry));

            tls_strncpy(entry->re_Host, (const char *)conn->tc_HostName,
                        TLS_RESUME_HOST_MAX);
            entry->re_Port        = conn->tc_Port;
            entry->re_CipherSuite = (UWORD)conn->tc_CipherSuite;
            entry->re_Protocol    = (UWORD)conn->tc_Protocol;
            entry->re_Stamp       = tls_time_monotonic();
            entry->re_Serial      = ++base->tb_SessionSerial;
            entry->re_Valid       = 1;
            entry->re_Flags       = tls_resume_flags(conn);
            entry->re_MaxChain    = (UBYTE)conn->tc_RemoteCount;
            entry->re_TrustKey    = tls_resume_trust_key(conn);

            tls_r_copy(entry->re_Master,
                       s->nx_secure_tls_key_material.nx_secure_tls_master_secret,
                       TLS_MASTER_SECRET_SIZE);

            if (have_sid)
            {
                entry->re_SidLength = s->nx_secure_tls_session_id_length;
                tls_r_copy(entry->re_Sid, s->nx_secure_tls_session_id,
                           entry->re_SidLength);
            }

            if (have_ticket)
            {
                tls_r_copy(entry->re_Ticket, conn->tc_Ticket,
                           conn->tc_TicketLength);
                entry->re_TicketLength = conn->tc_TicketLength;
                entry->re_Lifetime     = conn->tc_TicketLifetime;
            }

            changed = TRUE;

            tls_trace("[resume] stored %s: sid %ld ticket %ld suite %lx",
                      (LONG)entry->re_Host, (LONG)entry->re_SidLength,
                      (LONG)entry->re_TicketLength,
                      (ULONG)entry->re_CipherSuite);
        }
    }

    if (changed && (conn->tc_ResumeFlags & TLSR_PERSIST) != 0)
        tls_resume_save(base, conn->tc_SessionPath);

    ReleaseSemaphore(&base->tb_Lock);
}

/*
 * Splice a session ID into the one-byte length slot the vendored ClientHello
 * always writes as zero, and append a session_ticket extension -- sent even
 * when empty, because that is how a client asks to be issued one.
 */
UINT __wrap__nx_secure_tls_send_clienthello(NX_SECURE_TLS_SESSION *tls_session,
                                            NX_PACKET *send_packet)
{
    TLSConnection *conn;
    UCHAR         *base;
    UCHAR         *end;
    ULONG          length;
    ULONG          offset;
    ULONG          ext_offset;
    ULONG          need;
    ULONG          sid_length;
    ULONG          ticket_length;
    UWORD          ext_total;
    UINT           status;
    ULONG          i;

    base   = send_packet->nx_packet_append_ptr;
    status = __real__nx_secure_tls_send_clienthello(tls_session, send_packet);

    if (status != NX_SUCCESS)
        return status;

    conn = tls_conn_for_session(tls_session);
    if (conn == NULL || (conn->tc_ResumeFlags & TLSR_ENABLED) == 0)
        return NX_SUCCESS;

    length = (ULONG)(send_packet->nx_packet_append_ptr - base);
    end    = send_packet->nx_packet_data_end;

    /* The vendored code writes at least version + random + a zero session ID
       length, so anything shorter is not a ClientHello and is left alone. */
    if (length < 35 || base[34] != 0)
        return NX_SUCCESS;

    sid_length    = ((conn->tc_ResumeFlags & TLSR_OFFERED) != 0)
                    ? (ULONG)conn->tc_OfferSidLength : 0;
    ticket_length = ((conn->tc_ResumeFlags & TLSR_OFFERED) != 0)
                    ? (ULONG)conn->tc_TicketLength : 0;

    if (sid_length > TLS_RESUME_SID_MAX)
        sid_length = 0;
    if (conn->tc_Ticket == NULL)
        ticket_length = 0;

    need = sid_length + 4 + ticket_length;

    /*
     * Two ceilings: the packet must hold what is written, and the finished
     * handshake message must fit nx_secure_tls_handshake_cache[500], because
     * _nx_secure_tls_send_handshake_record() copies into it with no bounds check.
     */
    if ((base + length + need) > end ||
        (length + need + 4) > TLS_CLIENTHELLO_CACHE_MAX)
    {
        ticket_length = 0;
        need          = sid_length + 4;

        if ((base + length + need) > end ||
            (length + need + 4) > TLS_CLIENTHELLO_CACHE_MAX)
        {
            /* Not even an empty extension fits.  Leave the message alone. */
            conn->tc_ResumeFlags &= ~TLSR_OFFERED;
            return NX_SUCCESS;
        }

        /*
         * A generated session ID is the ticket's echo handle (RFC 5077 3.4) and
         * nothing else, so dropping the ticket must drop the cached master secret
         * too.  A server-issued session ID is left offered; RFC 5246 may still work.
         */
        if ((conn->tc_ResumeFlags & TLSR_SID_GEN) != 0)
            conn->tc_ResumeFlags &= ~TLSR_OFFERED;
    }

    /*
     * The walk comes first and the splice second, so every unexpected-layout exit
     * leaves the message exactly as the vendored code wrote it.
     */
    offset = 2 + 32;                            /* version + random          */
    offset += 1;                                /* the zero session ID length */

    if ((offset + 2) > length)
        return NX_SUCCESS;
    offset += 2 + (ULONG)tls_r_be16(&base[offset]);      /* ciphersuites     */

    if ((offset + 1) > length)
        return NX_SUCCESS;
    offset += 1 + (ULONG)base[offset];                   /* compression      */

    if ((offset + 2) > length)
        return NX_SUCCESS;

    ext_total = tls_r_be16(&base[offset]);

    /* The extensions block must be exactly the rest of the message, or this
       is not the shape assumed above and nothing more is written. */
    if ((offset + 2 + (ULONG)ext_total) != length)
        return NX_SUCCESS;

    if (sid_length > 0)
    {
        for (i = length; i-- > 35; )
            base[i + sid_length] = base[i];

        base[34] = (UCHAR)sid_length;
        tls_r_copy(&base[35], conn->tc_OfferSid, sid_length);

        length += sid_length;
        offset += sid_length;
    }

    ext_offset = offset;

    tls_r_put16(&base[length], TLS_EXT_SESSION_TICKET);
    tls_r_put16(&base[length + 2], (UWORD)ticket_length);
    if (ticket_length > 0)
        tls_r_copy(&base[length + 4], conn->tc_Ticket, ticket_length);

    length    += 4 + ticket_length;
    ext_total  = (UWORD)(ext_total + 4 + ticket_length);
    tls_r_put16(&base[ext_offset], ext_total);

    send_packet->nx_packet_append_ptr = base + length;
    send_packet->nx_packet_length     = (ULONG)(send_packet->nx_packet_length +
                                                sid_length + 4 + ticket_length);

    tls_trace("[resume] clienthello: sid %ld ticket %ld body %ld flags %lx",
              (LONG)sid_length, (LONG)ticket_length, (LONG)length,
              conn->tc_ResumeFlags);

    return NX_SUCCESS;
}

static UINT tls_resume_accept(TLSConnection *conn, NX_SECURE_TLS_SESSION *s)
{
    const NX_SECURE_TLS_CIPHERSUITE_INFO *ciphersuite;
    const NX_CRYPTO_METHOD               *prf;
    UINT                                  status;

    ciphersuite = s->nx_secure_tls_session_ciphersuite;
    if (ciphersuite == NX_NULL)
        return NX_SECURE_TLS_UNKNOWN_CIPHERSUITE;

    /*
     * TLS 1.2: the server MUST choose the same ciphersuite as the session it
     * agreed to resume.  TLSOpenA drops the cache entry on failure, so the
     * retry is a clean full handshake rather than the same failure forever.
     */
    if ((ULONG)ciphersuite->nx_secure_tls_ciphersuite != conn->tc_CipherSuite)
    {
        return NX_SECURE_TLS_UNKNOWN_CIPHERSUITE;
    }

    /*
     * The negotiated version is part of the cached session: an echoed ID under a
     * different version would restore a master secret using parameters from a
     * session the server did not actually resume.
     */
    if ((ULONG)s->nx_secure_tls_protocol_version != conn->tc_Protocol)
        return NX_SECURE_TLS_UNSUPPORTED_TLS_VERSION;

    /*
     * RFC 7627 5.3: only extended-master-secret sessions are ever cached, so a
     * ServerHello that resumes one without the extension is a downgrade and the
     * client MUST abort.
     */
    if (!tls_resume_secret_bound(s))
    {
        tls_trace("[resume] serverhello resumed without RFC 7627, refused");
        return NX_SECURE_TLS_DOWNGRADE_DETECTED;
    }

    tls_r_copy(s->nx_secure_tls_key_material.nx_secure_tls_master_secret,
               conn->tc_Master, TLS_MASTER_SECRET_SIZE);

    /*
     * Derive the record keys from the restored master secret through the
     * session's own generate_session_keys pointer, so a caller that replaced it
     * keeps its replacement.
     */
    prf    = ciphersuite->nx_secure_tls_prf;
    status = s->nx_secure_generate_session_keys(
                 ciphersuite, s->nx_secure_tls_protocol_version, prf,
                 &s->nx_secure_tls_key_material,
                 s->nx_secure_tls_key_material.nx_secure_tls_master_secret,
                 s->nx_secure_tls_prf_metadata_area,
                 s->nx_secure_tls_prf_metadata_size);

    if (status != NX_SUCCESS)
        return status;

    /*
     * received_remote_credentials, because _nx_secure_tls_process_finished()
     * refuses a Finished from a peer that sent no credentials.  SERVERHELLO_DONE,
     * because _nx_secure_tls_process_changecipherspec() rejects a CCS otherwise.
     */
    s->nx_secure_tls_received_remote_credentials = NX_TRUE;
    s->nx_secure_tls_client_state = NX_SECURE_TLS_CLIENT_STATE_SERVERHELLO_DONE;

    conn->tc_ResumeFlags |= TLSR_RESUMED;
    conn->tc_CipherSuite  = (ULONG)ciphersuite->nx_secure_tls_ciphersuite;

    return NX_SUCCESS;
}

/* RFC 5077 3.3: uint32 ticket_lifetime_hint, then opaque ticket<0..2^16-1>. */
static VOID tls_resume_take_ticket(TLSConnection *conn, const UCHAR *body,
                                   UINT length)
{
    ULONG lifetime;
    ULONG ticket_length;

    if (length < 6 || conn->tc_Ticket == NULL)
        return;

    lifetime      = tls_r_be32(body);
    ticket_length = (ULONG)tls_r_be16(&body[4]);

    if ((6UL + ticket_length) > (ULONG)length)
        return;

    if (ticket_length == 0 || ticket_length > TLS_RESUME_TICKET_MAX)
        return;

    tls_r_copy(conn->tc_Ticket, &body[6], ticket_length);
    conn->tc_TicketLength   = (UWORD)ticket_length;
    conn->tc_TicketLifetime = lifetime;
    conn->tc_ResumeFlags   |= TLSR_TICKET_NEW;
}

/*
 * The server's Finished, on a resumed handshake.  The abbreviated handshake
 * reverses the Finished order, so everything here is work the vendored state
 * machine has no path for.
 */
static UINT tls_resume_finish(NX_SECURE_TLS_SESSION *s, UCHAR *packet_start,
                              UINT header_bytes, UINT message_length,
                              ULONG wait_option)
{
    const NX_CRYPTO_METHOD *method_ptr;
    NX_PACKET_POOL         *pool = s->nx_secure_tls_packet_pool;
    NX_PACKET              *send_packet = NX_NULL;
    UINT                    status;

    status = _nx_secure_tls_process_finished(s, packet_start + header_bytes,
                                             message_length);
    if (status != NX_SUCCESS)
        return status;

    status = _nx_secure_tls_handshake_hash_update(s, packet_start,
                                                  message_length + header_bytes);
    if (status != NX_SUCCESS)
        return status;

    /*
     * The mutex dance around the allocation is the vendored one:
     * _nx_secure_tls_packet_allocate() can suspend.
     */
    (VOID)_tx_mutex_put(&_nx_secure_tls_protection);
    status = _nx_secure_tls_packet_allocate(s, pool, &send_packet, wait_option);
    (VOID)_tx_mutex_get(&_nx_secure_tls_protection, TX_WAIT_FOREVER);

    if (status != NX_SUCCESS)
        return status;

    _nx_secure_tls_send_changecipherspec(s, send_packet);

    status = _nx_secure_tls_send_record(s, send_packet,
                                        NX_SECURE_TLS_CHANGE_CIPHER_SPEC,
                                        wait_option);
    if (status != NX_SUCCESS)
    {
        (VOID)_nx_packet_release(send_packet);
        return status;
    }

    NX_SECURE_MEMSET(s->nx_secure_tls_local_sequence_number, 0,
                     sizeof(s->nx_secure_tls_local_sequence_number));

    status = _nx_secure_tls_session_keys_set(s, NX_SECURE_TLS_KEY_SET_LOCAL);
    if (status != NX_SUCCESS)
        return status;

    status = _nx_secure_tls_allocate_handshake_packet(s, pool, &send_packet,
                                                      wait_option);
    if (status != NX_SUCCESS)
        return status;

    status = _nx_secure_tls_send_finished(s, send_packet);
    if (status != NX_SUCCESS)
    {
        (VOID)_nx_packet_release(send_packet);
        return status;
    }

    status = _nx_secure_tls_send_handshake_record(s, send_packet,
                                                  NX_SECURE_TLS_FINISHED,
                                                  wait_option);
    if (status != NX_SUCCESS)
        return status;

    /*
     * The vendored state machine clears the handshake hash the instant it
     * processes a Finished, and the client's Finished needs that context, which
     * is why this message cannot be handed to it.
     */
    method_ptr = s->nx_secure_tls_crypto_table->nx_secure_tls_handshake_hash_sha256_method;
    if (method_ptr != NX_NULL && method_ptr->nx_crypto_cleanup != NX_NULL)
    {
        (VOID)method_ptr->nx_crypto_cleanup(
                  s->nx_secure_tls_handshake_hash.nx_secure_tls_handshake_hash_sha256_metadata);
    }

    return NX_SUCCESS;
}

UINT __wrap__nx_secure_tls_client_handshake(NX_SECURE_TLS_SESSION *tls_session,
                                            UCHAR *packet_buffer,
                                            UINT data_length,
                                            ULONG wait_option)
{
    TLSConnection *conn;
    UCHAR         *scan;
    UINT           remaining;
    UINT           header_bytes;
    UINT           message_length;
    USHORT         message_type;
    UINT           status;
    BOOL           saw_serverhello = FALSE;
    BOOL           special         = FALSE;

    conn = tls_conn_for_session(tls_session);

    if (conn == NULL || (conn->tc_ResumeFlags & TLSR_ENABLED) == 0)
    {
        return __real__nx_secure_tls_client_handshake(tls_session, packet_buffer,
                                                      data_length, wait_option);
    }

    /*
     * The scan comes first, so the common case reaches the vendored function with
     * the same data_length -- which is what _nx_secure_tls_process_remote_
     * certificate() bounds itself with.  Only an unhandleable record is split.
     */
    scan      = packet_buffer;
    remaining = data_length;

    while (remaining > 0)
    {
        header_bytes = remaining;

        if (_nx_secure_tls_process_handshake_header(scan, &message_type,
                                                    &header_bytes,
                                                    &message_length) != NX_SUCCESS)
        {
            break;
        }

        /* A message split across records: the vendored function owns the
           reassembly state machine, so hand the whole thing over. */
        if ((message_length + header_bytes) > remaining)
        {
            special = FALSE;
            break;
        }

        tls_trace("[resume] msg type %ld len %ld", (LONG)message_type,
                  (LONG)message_length);

        if (message_type == NX_SECURE_TLS_SERVER_HELLO)
            saw_serverhello = TRUE;

        if (message_type == NX_SECURE_TLS_NEW_SESSION_TICKET)
            special = TRUE;

        if (message_type == NX_SECURE_TLS_FINISHED &&
            (conn->tc_ResumeFlags & TLSR_RESUMED) != 0)
        {
            special = TRUE;
        }

        scan      += header_bytes + message_length;
        remaining -= header_bytes + message_length;
    }

    if (!special)
    {
        status = __real__nx_secure_tls_client_handshake(tls_session, packet_buffer,
                                                        data_length, wait_option);
        if (status != NX_SUCCESS)
            return status;

        if (saw_serverhello)
        {
            tls_trace("[resume] serverhello: echoed sid %ld, offered %ld",
                      (LONG)tls_session->nx_secure_tls_session_id_length,
                      (LONG)conn->tc_OfferSidLength);
        }

        if (saw_serverhello && (conn->tc_ResumeFlags & TLSR_OFFERED) != 0 &&
            (conn->tc_ResumeFlags & TLSR_RESUMED) == 0)
        {
            if (conn->tc_OfferSidLength > 0 &&
                tls_session->nx_secure_tls_session_id_length ==
                    conn->tc_OfferSidLength &&
                tls_r_equal(tls_session->nx_secure_tls_session_id,
                            conn->tc_OfferSid, conn->tc_OfferSidLength))
            {
                status = tls_resume_accept(conn, tls_session);
                tls_trace("[resume] ACCEPT -> %ld", (LONG)status);
                if (status != NX_SUCCESS)
                    return status;
            }
        }

        return NX_SUCCESS;
    }

    scan      = packet_buffer;
    remaining = data_length;

    while (remaining > 0)
    {
        UCHAR *start = scan;

        header_bytes = remaining;

        status = _nx_secure_tls_process_handshake_header(scan, &message_type,
                                                         &header_bytes,
                                                         &message_length);
        if (status != NX_SUCCESS)
            return status;

        if ((message_length + header_bytes) > remaining)
            return NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH;

        if (message_type == NX_SECURE_TLS_NEW_SESSION_TICKET)
        {
            tls_resume_take_ticket(conn, start + header_bytes, message_length);

            /* RFC 5077 3.3: a NewSessionTicket is part of the handshake
               transcript, so it is hashed like any other message. */
            status = _nx_secure_tls_handshake_hash_update(
                         tls_session, start, message_length + header_bytes);
            if (status != NX_SUCCESS)
                return status;
        }
        else if (message_type == NX_SECURE_TLS_FINISHED &&
                 (conn->tc_ResumeFlags & TLSR_RESUMED) != 0)
        {
            status = tls_resume_finish(tls_session, start, header_bytes,
                                       message_length, wait_option);
            tls_trace("[resume] resumed finish -> %ld", (LONG)status);
            if (status != NX_SUCCESS)
                return status;
        }
        else
        {
            status = __real__nx_secure_tls_client_handshake(
                         tls_session, start, header_bytes + message_length,
                         wait_option);
            if (status != NX_SUCCESS)
                return status;

            if (message_type == NX_SECURE_TLS_SERVER_HELLO &&
                (conn->tc_ResumeFlags & TLSR_OFFERED) != 0 &&
                (conn->tc_ResumeFlags & TLSR_RESUMED) == 0)
            {
                if (conn->tc_OfferSidLength > 0 &&
                    tls_session->nx_secure_tls_session_id_length ==
                        conn->tc_OfferSidLength &&
                    tls_r_equal(tls_session->nx_secure_tls_session_id,
                                conn->tc_OfferSid, conn->tc_OfferSidLength))
                {
                    status = tls_resume_accept(conn, tls_session);
                    if (status != NX_SUCCESS)
                        return status;
                }
            }
        }

        scan      += header_bytes + message_length;
        remaining -= header_bytes + message_length;
    }

    return NX_SUCCESS;
}
