/*
 * tls.library -- TLS 1.2 session resumption.
 *
 * WHY THIS IS THE MOST VALUABLE THING IN THE TLS TREE
 *
 *   A full handshake on a 14 MHz 68020 costs 6.8 s for a two-certificate RSA
 *   chain and 23.3 s for a two-certificate ECDSA one, and essentially all of
 *   that is public-key arithmetic -- three signature verifications, one ECDHE
 *   key generation, one ECDHE shared secret.  Cloudflare gives up on a
 *   handshake somewhere between 11 and 20 seconds, so a three-deep chain does
 *   not complete at all.
 *
 *   A RESUMED handshake does none of that arithmetic.  It is one ClientHello,
 *   one ServerHello, two ChangeCipherSpecs and two Finished messages -- four
 *   PRF invocations and a pair of hashes.  The master secret comes out of a
 *   cache instead of out of a key exchange.  Nothing else in this tree turns
 *   twenty-three seconds into under one.
 *
 * WHICH MECHANISM, AND THE MEASUREMENT THAT DECIDED IT
 *
 *   There are two.  RFC 5077 session TICKETS: the server hands the client an
 *   opaque blob encrypted under a key only the server knows, and the client
 *   presents it next time.  Session IDs: the classic mechanism, where the
 *   server keeps the session in its own memory and the client presents a
 *   32-byte handle.
 *
 *   Both were probed against the four hosts this library is developed against,
 *   ten trials each, first connection then immediate second connection:
 *
 *       host                    session ID      ticket
 *       www.iana.org            2/10 resumed    10/10 resumed
 *       tls-v1-2.badssl.com     0/10            10/10
 *       ecc256.badssl.com       0/10            10/10
 *       example.com             0/10            10/10
 *
 *   That is not a close call.  A modern CDN front end is a farm of machines
 *   behind one address, and a session ID is state on ONE of them; a ticket is
 *   stateless and every machine in the farm can decrypt it.  The two hits on
 *   iana are the second connection happening to land on the same edge node.
 *   The probe method is sound -- the same script against a local
 *   `openssl s_server -no_ticket` resumes every time, which is the control.
 *
 *   So: TICKETS ARE THE MECHANISM.  Session IDs come along for free, because
 *   the acceptance signal for a ticket is the server echoing back the session
 *   ID we offered, and offering the previous session's ID (RFC 5077 3.4) means
 *   a server with a working ID cache resumes us too.  That is where those two
 *   iana hits go, and it is what makes resumption work against an intranet
 *   server that has never heard of tickets.
 *
 *   One risk was checked before building rather than after: RFC 7627's
 *   extended_master_secret extension.  nx_secure does not implement it, so
 *   every session this library creates is a non-EMS session, and RFC 7627 5.3
 *   says a server SHOULD refuse to resume one abbreviated.  Measured with
 *   `openssl s_client -no_ems`: 5/5 resumed on all four hosts.  Nobody
 *   enforces the SHOULD.
 *
 * HOW THIS IS BUILT WITHOUT TOUCHING third_party/
 *
 *   nx_secure has no resumption of any kind.  nx_secure_tls_send_clienthello.c
 *   sets the session ID length to zero unconditionally with a comment saying
 *   session resumption is not implemented, there is no session_ticket
 *   extension anywhere in the tree, and _nx_secure_tls_client_handshake()
 *   fails a handshake outright on a NewSessionTicket message because its
 *   switch has no case for one.
 *
 *   The established pattern in this project for changing vendored behaviour is
 *   to define the symbol in our own archive and let the linker resolve it
 *   there -- src/net68k/ for the IP checksum, src/tlslib/tls_netx.c for twelve
 *   NetX Duo entry points.  That pattern works here but costs more than it
 *   should: replacing _nx_secure_tls_client_handshake() outright means
 *   copying 450 lines of state machine that has nothing to do with
 *   resumption, and every one of those lines then has to be re-merged by hand
 *   at the next submodule bump.
 *
 *   So this uses the linker's other tool for the same job: --wrap.
 *   `-Wl,--wrap=_nx_secure_tls_client_handshake` sends every call from every
 *   other object to __wrap__nx_secure_tls_client_handshake() here, and leaves
 *   __real__nx_secure_tls_client_handshake() pointing at the vendored
 *   function, which is still linked and still does all the work it always did.
 *   Two functions are wrapped and nothing else:
 *
 *     _nx_secure_tls_send_clienthello -- we let the vendored one build the
 *         whole message and then splice in a session ID and a session_ticket
 *         extension.  The splice is TLS wire format, not nx_secure internals,
 *         so a submodule bump cannot silently invalidate it.
 *
 *     _nx_secure_tls_client_handshake -- three handshake message types need
 *         behaviour the vendored state machine does not have.  Everything else
 *         is handed to the vendored function byte for byte, in the same record
 *         boundaries it would have seen.
 *
 *   Verified to work on this toolchain before anything was written: a
 *   three-object test with the caller in a separate archive member disassembles
 *   to `jsr ___wrap_vendored`.
 *
 * THE THREE MESSAGES
 *
 *   ServerHello.  Handed to the vendored function.  Afterwards its echoed
 *   session ID is compared against the one we offered; equal and non-empty
 *   means the server has resumed.  Then the cached master secret goes into the
 *   key material, the session keys are derived from it through the session's
 *   own generate_session_keys pointer, and the client state is advanced to
 *   SERVERHELLO_DONE -- which is what _nx_secure_tls_process_changecipherspec()
 *   demands to see before it will accept the server's ChangeCipherSpec.
 *
 *   NewSessionTicket.  The vendored switch has no case for it and its
 *   `default:` leaves status at NX_SECURE_TLS_HANDSHAKE_FAILURE, so merely
 *   ASKING for a ticket would break every full handshake.  Handled here: hash
 *   it into the transcript (RFC 5077 3.3 -- it counts), keep the blob.
 *
 *   Finished, on a resumed handshake only.  The abbreviated handshake reverses
 *   the order: the server finishes first.  So the server's Finished has to go
 *   INTO the transcript before ours is generated, and our ChangeCipherSpec and
 *   Finished have to be sent afterwards -- neither of which the vendored state
 *   machine does, and it also destroys the SHA-256 handshake hash context
 *   immediately after processing a Finished, which is exactly the context our
 *   own Finished needs.  So this whole message is handled here.
 *
 * WHAT IS DELIBERATELY NOT DONE
 *
 *   No renegotiation interaction: this library never renegotiates.  No TLS 1.3
 *   (nx_secure's TLS 1.3 is impractical here anyway -- its GCM is a bit-serial
 *   GHASH).  No attempt to resume a session whose ciphersuite the server then
 *   changes: that is a broken server, and the cache entry is dropped so the
 *   next attempt is a clean full handshake.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* RFC 5077.  nx_secure has no name for it because it has no support for it. */
#define TLS_EXT_SESSION_TICKET      0x0023

/*
 * -DTLS_RESUME_TRACE puts a running commentary on the serial port: what went
 * into the ClientHello, every handshake message type seen, and what the
 * ServerHello echoed back against what was offered.  Off in every shipping
 * build -- it is here because a resumption that silently does not happen is
 * indistinguishable from one the server declined, and the difference is the
 * only thing worth knowing when this goes wrong.
 */
#ifdef TLS_RESUME_TRACE

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

/* The on-disk mirror.  See "THE DISK MIRROR" below. */
#define TLS_SESSIONS_MAGIC          0x41545331UL    /* 'ATS1' */
#define TLS_SESSIONS_HEADER         16UL
#define TLS_SESSIONS_RECORD         420UL   /* 164 + TLS_RESUME_TICKET_MAX */

/* Ceiling on how long a cached session is offered when the server gave no
   lifetime hint, and on how far a server's hint is believed.  Cloudflare says
   64800 s, badssl says 300; a client that offers a stale session loses one
   round trip and gets a full handshake, so this is a comfort limit and not a
   security boundary. */
#define TLS_RESUME_MAX_AGE          86400UL

/*
 * The direct entry points, not the nx_/tx_ spellings.  Those map to the
 * argument-checking nxe_/txe_ wrappers, whose archive members reference
 * ThreadX DATA symbols that tls_netx.c cannot forward -- see the note there.
 * The rest of this library calls the underscore forms for the same reason.
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

/* ------------------------------------------------------------- helpers --- */

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
 * Case-insensitive, because both things compared with this are: DNS names are
 * case-insensitive by definition, and so are AmigaOS file names.  The same
 * host reached as "Example.COM" and "example.com" is one session, and
 * "DEVS:Internet/tlssessions" and "devs:internet/TLSsessions" are one file.
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

/* ---------------------------------------------------------- the cache --- */

/*
 * THE CACHE, AND WHERE IT LIVES
 *
 *   In the LIBRARY BASE, and mirrored to a file.  Both, and for two different
 *   reasons.
 *
 *   The base, because a shared library on AmigaOS outlives the programs that
 *   open it: `fetch` runs, exits, and tls.library stays in memory with its
 *   open count at zero until something needs the RAM.  A cache with the
 *   library's lifetime therefore already answers the case that matters --
 *   somebody running curl twice -- and it answers it with no disk access at
 *   all, which on a floppy-based machine is worth having.
 *
 *   The file, because "until something needs the RAM" is not a guarantee.
 *   AllocMem() failure or `avail flush` expunges the library and the cache
 *   goes with it, as does a reboot.  DEVS:Internet/tlssessions sits beside
 *   DEVS:Internet/certificates in the same Roadshow configuration drawer the
 *   rest of this stack uses, is read once per library lifetime and written
 *   only when the cache actually changes -- once per full handshake, against a
 *   handshake that just spent seconds on arithmetic.
 *
 *   Shared between programs, deliberately.  A ticket obtained by `fetch` is a
 *   ticket curl can use; they are talking to the same server on behalf of the
 *   same user.
 *
 * SECURITY, STATED ONCE AND WITHOUT AGONISING
 *
 *   Each entry holds a 48-byte TLS master secret and a session ticket in the
 *   clear.  Anyone who can read the library's memory can decrypt any session
 *   resumed from it, and this is a machine with no memory protection where
 *   every task can read every other task's memory anyway.  Anyone who can read
 *   DEVS:Internet/tlssessions can do the same, offline, until the entry ages
 *   out -- so the file is exactly as sensitive as the sessions it stands for
 *   and no more, and it is not protected because on AmigaOS there is nothing
 *   to protect it with.  Forward secrecy is lost for the resumed sessions
 *   specifically: an attacker who takes the file can decrypt captured traffic
 *   from those sessions, which the ECDHE full handshake would not have allowed.
 *   That is the price, it is the same price every TLS session cache has paid
 *   since 1996, and this stack exists so a classic Amiga can read the web, not
 *   to protect anything valuable.  TLSA_NoResume turns the whole thing off and
 *   TLSA_SessionFile with an empty string keeps it in RAM only.
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
 * A connection's half of the verification match.  A session established with
 * TLSA_NoVerify and one established with the chain and host name checked are
 * two different populations and must not be confused for one another.
 */
static UBYTE tls_resume_flags(const TLSConnection *conn)
{
    return (UBYTE)(((conn->tc_Flags & TLSF_VERIFY) != 0) ? TLSRE_VERIFIED : 0);
}

/* Has this entry aged out?  A machine with no clock cannot answer, and says
   so by never expiring anything: a stale ticket costs one round trip and a
   full handshake, which is what would have happened anyway. */
static BOOL tls_resume_expired(const TLSResumeEntry *e, ULONG now)
{
    ULONG age;
    ULONG limit;

    if (now == 0 || e->re_Stamp == 0 || now < e->re_Stamp)
        return FALSE;

    age   = now - e->re_Stamp;
    limit = (e->re_Lifetime != 0 && e->re_Lifetime < TLS_RESUME_MAX_AGE)
            ? e->re_Lifetime : TLS_RESUME_MAX_AGE;

    return (BOOL)((age > limit) ? TRUE : FALSE);
}

static TLSResumeEntry *tls_resume_find(TLSResumeEntry *table, const char *host,
                                       UWORD port, UBYTE flags)
{
    ULONG i;

    for (i = 0; i < TLS_RESUME_SLOTS; i++)
    {
        if (table[i].re_Valid == 0)
            continue;
        if (table[i].re_Port != port)
            continue;
        if (((table[i].re_Flags ^ flags) & TLSRE_VERIFIED) != 0)
            continue;
        if (tls_r_host_equal(table[i].re_Host, host))
            return &table[i];
    }

    return NULL;
}

/* A free slot, the matching slot, or the least recently used one. */
static TLSResumeEntry *tls_resume_slot(TLSResumeEntry *table, const char *host,
                                       UWORD port, UBYTE flags)
{
    TLSResumeEntry *victim;
    ULONG           i;

    victim = tls_resume_find(table, host, port, flags);
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

/* ------------------------------------------------------ the disk mirror -- */

/*
 * 'ATS1', big-endian, fixed 484-byte records so a truncated or corrupt file
 * cannot be mis-parsed into a wild pointer -- it is either a whole number of
 * records or it is rejected:
 *
 *    0   'A' 'T' 'S' '1'
 *    4   ULONG count
 *    8   ULONG reserved (0)
 *   12   ULONG reserved (0)
 *   16   count x {
 *          0  host[64]        NUL-padded
 *         64  UWORD port
 *         66  UWORD ciphersuite
 *         68  UWORD protocol version
 *         70  UBYTE session id length
 *         71  UBYTE flags (bit 0: the chain and host name were verified)
 *         72  session id[32]
 *        104  ULONG stamp, UNIX seconds (0 when the clock was unset)
 *        108  ULONG server lifetime hint, seconds
 *        112  master secret[48]
 *        160  UWORD ticket length
 *        162  UWORD reserved
 *        164  ticket[320]
 *        }
 *
 * Eight entries is 3,888 bytes on disk.  There is no index and no laziness --
 * unlike the trust store this is small, written as a whole, and read exactly
 * once per library lifetime.
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
    tls_r_copy(&rec[112], e->re_Master, TLS_MASTER_SECRET_SIZE);
    tls_r_put16(&rec[160], e->re_TicketLength);
    tls_r_copy(&rec[164], e->re_Ticket, TLS_RESUME_TICKET_MAX);

    /* The record layout and the structure must not drift apart; the file is
       read back by a different build of this same library. */
    _Static_assert((164UL + TLS_RESUME_TICKET_MAX) == TLS_SESSIONS_RECORD,
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
    tls_r_copy(e->re_Master, &rec[112], TLS_MASTER_SECRET_SIZE);
    e->re_TicketLength = tls_r_be16(&rec[160]);
    tls_r_copy(e->re_Ticket, &rec[164], TLS_RESUME_TICKET_MAX);

    /* Anything the file claims that the structure cannot hold is a corrupt
       file, not a session.  Refuse the record rather than clamping it. */
    if (e->re_Host[0] == '\0' ||
        e->re_SidLength > TLS_RESUME_SID_MAX ||
        e->re_TicketLength > TLS_RESUME_TICKET_MAX)
    {
        return FALSE;
    }

    if (e->re_SidLength == 0 && e->re_TicketLength == 0)
        return FALSE;

    e->re_Valid = 1;

    return TRUE;
}

/*
 * Read the mirror into the resident cache.  Once per library lifetime for a
 * given path -- and again if a connection names a DIFFERENT path, because
 * TLSA_SessionFile has to mean the same thing on the way in as on the way out.
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

/* ---------------------------------------------------- the API this side -- */

VOID tls_resume_prepare(TLSConnection *conn)
{
    struct TLSLibBase *base;
    TLSResumeEntry    *table;
    TLSResumeEntry    *entry;
    ULONG              now;

    if (conn == NULL || (conn->tc_ResumeFlags & TLSR_ENABLED) == 0)
        return;

    /*
     * No host name, no resumption.  The cache is keyed by the name the caller
     * asked for, and reusing a master secret against a DIFFERENT host would be
     * a genuine security bug rather than a missed optimisation.  TLSA_NoVerify
     * without TLSA_HostName is the only way to get here, and it is rare.
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
    now  = tls_time_now();

    ObtainSemaphore(&base->tb_Lock);

    tls_resume_load(base, conn->tc_SessionPath);

    table = tls_resume_table(base);
    if (table == NULL)
    {
        ReleaseSemaphore(&base->tb_Lock);
        return;
    }

    entry = tls_resume_find(table, (const char *)conn->tc_HostName,
                            conn->tc_Port, tls_resume_flags(conn));

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

        conn->tc_TicketLength = entry->re_TicketLength;
        if (conn->tc_TicketLength > 0 && conn->tc_Ticket != NULL)
            tls_r_copy(conn->tc_Ticket, entry->re_Ticket, conn->tc_TicketLength);
        else
            conn->tc_TicketLength = 0;

        /*
         * A SESSION ID WE INVENT, when the cached session has none.
         *
         * This is not an optimisation, it is the difference between resumption
         * working and not, and it was found the hard way.  A server that issues
         * a ticket returns an EMPTY session ID in the ServerHello -- RFC 5077
         * 3.4, and measured: nginx does exactly that, so the session recorded
         * from a first handshake has no session ID at all.  Offering nothing
         * then means the server has nothing to echo, and "the ServerHello
         * echoed what we offered" -- the only acceptance signal a TLS 1.2
         * client gets -- can never fire.  The handshake is abbreviated, the
         * client does not notice, and it fails at the ChangeCipherSpec.
         *
         * RFC 5077 3.4 provides for exactly this: a client presenting a ticket
         * MAY generate a session ID, and a server accepting the ticket MUST
         * echo it.  Thirty-two random bytes, from the machine's own pool, per
         * attempt and never cached -- it is a correlation handle, not a secret,
         * and reusing one would let an observer link two connections.
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
                                tls_resume_flags(conn));
        if (entry != NULL)
        {
            tls_bzero(entry, sizeof(TLSResumeEntry));

            if ((conn->tc_ResumeFlags & TLSR_PERSIST) != 0)
                tls_resume_save(base, conn->tc_SessionPath);
        }
    }

    ReleaseSemaphore(&base->tb_Lock);
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
         * The session was resumed.  Its master secret is unchanged and its
         * session ID is the one we offered, so the only thing worth writing
         * back is a ticket the server chose to replace ours with -- and the
         * timestamp, so a session that keeps being used keeps being offered.
         */
        entry = tls_resume_find(table, (const char *)conn->tc_HostName,
                                conn->tc_Port, tls_resume_flags(conn));
        if (entry != NULL)
        {
            entry->re_Stamp  = tls_time_now();
            entry->re_Serial = ++base->tb_SessionSerial;

            if ((conn->tc_ResumeFlags & TLSR_TICKET_NEW) != 0 &&
                conn->tc_TicketLength > 0 && conn->tc_Ticket != NULL)
            {
                tls_r_copy(entry->re_Ticket, conn->tc_Ticket,
                           conn->tc_TicketLength);
                entry->re_TicketLength = conn->tc_TicketLength;
                entry->re_Lifetime     = conn->tc_TicketLifetime;
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

        /* Nothing to remember: some servers offer neither, and that is a
           server that will never resume us.  Do not keep a useless entry. */
        if (have_ticket || have_sid)
        {
            entry = tls_resume_slot(table, (const char *)conn->tc_HostName,
                                    conn->tc_Port, tls_resume_flags(conn));

            tls_bzero(entry, sizeof(TLSResumeEntry));

            tls_strncpy(entry->re_Host, (const char *)conn->tc_HostName,
                        TLS_RESUME_HOST_MAX);
            entry->re_Port        = conn->tc_Port;
            entry->re_CipherSuite = (UWORD)conn->tc_CipherSuite;
            entry->re_Protocol    = (UWORD)conn->tc_Protocol;
            entry->re_Stamp       = tls_time_now();
            entry->re_Serial      = ++base->tb_SessionSerial;
            entry->re_Valid       = 1;
            entry->re_Flags       = tls_resume_flags(conn);

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

/* ------------------------------------------------ __wrap send_clienthello -- */

/*
 * The vendored function builds the whole ClientHello.  This splices two things
 * into what it produced:
 *
 *   the session ID, into the one-byte-length slot the vendored code always
 *   writes as zero.  RFC 5077 3.4: a client presenting a ticket SHOULD send
 *   the session ID from the session the ticket came from, and the server
 *   echoing it back is how the client learns the ticket was accepted;
 *
 *   the session_ticket extension, appended after every extension the vendored
 *   code wrote.  Sent even when EMPTY, because an empty session_ticket
 *   extension is how a client says "I understand tickets, please issue me
 *   one" -- without it the first handshake never produces anything to resume
 *   from.
 *
 * The layout being patched is the TLS 1.2 wire format, fixed by RFC 5246 and
 * not by nx_secure, which is the whole reason this is a splice rather than a
 * copy of the vendored function.
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
       length, so anything shorter is not a ClientHello and this splice has no
       business touching it. */
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
     * Two ceilings, and the second one is the dangerous one.
     *
     * The packet has to hold what we are about to write, which is the ordinary
     * check the vendored code makes for its own content.
     *
     * And the finished handshake MESSAGE -- four header bytes plus this body --
     * has to fit in nx_secure_tls_handshake_cache[500], because
     * _nx_secure_tls_send_handshake_record() copies a ClientHello into that
     * fixed array with no bounds check at all.  Overrunning it would write
     * through the middle of NX_SECURE_TLS_SESSION.  Rather than trip that, drop
     * the ticket and fall back to a full handshake, which is slow and correct.
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

        /* The cached ticket cannot be presented, so the cached master secret
           must not be either -- otherwise the ServerHello echo test could
           match a session-ID resumption we are not prepared for. */
        if ((conn->tc_ResumeFlags & TLSR_OFFERED) != 0 && sid_length == 0)
            conn->tc_ResumeFlags &= ~TLSR_OFFERED;
    }

    /*
     * ---- find the extensions length field, BEFORE touching anything ------
     *
     * The walk comes first and the splice second, so that every "this is not
     * the shape I expected" exit leaves the message exactly as the vendored
     * code wrote it.  Doing the session-ID insert first and then discovering
     * the layout was unfamiliar would leave thirty-two bytes shifted with the
     * packet's length fields still describing the old message -- a corrupt
     * ClientHello, which is a much worse outcome than no resumption.
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

    /* ---- the session ID, inserted in the middle -------------------------- */

    if (sid_length > 0)
    {
        /* Shift everything after the length byte up, then fill the gap. */
        for (i = length; i-- > 35; )
            base[i + sid_length] = base[i];

        base[34] = (UCHAR)sid_length;
        tls_r_copy(&base[35], conn->tc_OfferSid, sid_length);

        length += sid_length;
        offset += sid_length;
    }

    ext_offset = offset;

    /* ---- the session_ticket extension, appended ------------------------- */

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

/* ------------------------------------------------ __wrap client_handshake -- */

/* The ServerHello echoed our session ID: restore the cached session. */
static UINT tls_resume_accept(TLSConnection *conn, NX_SECURE_TLS_SESSION *s)
{
    const NX_SECURE_TLS_CIPHERSUITE_INFO *ciphersuite;
    const NX_CRYPTO_METHOD               *prf;
    UINT                                  status;

    ciphersuite = s->nx_secure_tls_session_ciphersuite;
    if (ciphersuite == NX_NULL)
        return NX_SECURE_TLS_UNKNOWN_CIPHERSUITE;

    /*
     * TLS 1.2 says the server MUST choose the same ciphersuite it used for
     * the session it just agreed to resume.  A server that does not is broken
     * in a way this code cannot recover from mid-handshake, so fail -- and
     * TLSOpenA drops the cache entry on failure, so the retry is a clean full
     * handshake rather than the same failure forever.
     */
    if (conn->tc_CipherSuite != 0 &&
        (ULONG)ciphersuite->nx_secure_tls_ciphersuite != conn->tc_CipherSuite)
    {
        return NX_SECURE_TLS_UNKNOWN_CIPHERSUITE;
    }

    tls_r_copy(s->nx_secure_tls_key_material.nx_secure_tls_master_secret,
               conn->tc_Master, TLS_MASTER_SECRET_SIZE);

    /*
     * Derive the record keys straight from the restored master secret.  This
     * is _nx_secure_tls_generate_keys() minus the master-secret half, and it
     * goes through the session's OWN function pointer so a caller that
     * replaced it keeps its replacement.
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
     * Two pieces of state the abbreviated handshake needs and the vendored
     * code has no path to set.
     *
     * received_remote_credentials, because _nx_secure_tls_process_finished()
     * refuses a Finished from a peer that sent no credentials -- and a resumed
     * handshake sends no certificate BY DESIGN.  The credentials were checked
     * when this session was created; that is what resumption means.
     *
     * SERVERHELLO_DONE, because _nx_secure_tls_process_changecipherspec()
     * rejects a client ChangeCipherSpec in any other state, and the server's
     * CCS is the very next thing on the wire.
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

    /* An empty ticket is the server saying "no ticket after all", and a ticket
       larger than the ClientHello budget could never be offered back, so
       keeping it would only produce a cache entry that never resumes. */
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

    /* 1. Verify theirs against the transcript as it stands: ClientHello,
          ServerHello and any NewSessionTicket, and nothing after. */
    status = _nx_secure_tls_process_finished(s, packet_start + header_bytes,
                                             message_length);
    if (status != NX_SUCCESS)
        return status;

    /* 2. Ours covers theirs.  The vendored code never does this because in a
          full handshake the client finishes first. */
    status = _nx_secure_tls_handshake_hash_update(s, packet_start,
                                                  message_length + header_bytes);
    if (status != NX_SUCCESS)
        return status;

    /* 3. ChangeCipherSpec.  The mutex dance around the allocation is the
          vendored one: _nx_secure_tls_packet_allocate() can suspend. */
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

    /* 4. Our Finished, encrypted under the keys just set. */
    status = _nx_secure_tls_allocate_handshake_packet(s, pool, &send_packet,
                                                      wait_option);
    if (status != NX_SUCCESS)
        return status;

    status = _nx_secure_tls_send_finished(s, send_packet);
    if (status != NX_SUCCESS)
        return status;

    status = _nx_secure_tls_send_handshake_record(s, send_packet,
                                                  NX_SECURE_TLS_FINISHED,
                                                  wait_option);
    if (status != NX_SUCCESS)
        return status;

    /* 5. Now, and not before, the handshake hash context can go.  The vendored
          state machine cleans it up the instant it processes a Finished, which
          is why this message could not simply be handed to it: our own
          Finished needs the context it would have destroyed. */
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
     * Pass one: what is in this record?
     *
     * The point of scanning first is to leave the common case ALONE.  A record
     * carrying ServerHello, Certificate, ServerKeyExchange and ServerHelloDone
     * together goes to the vendored function exactly as it would have, with the
     * same data_length -- which matters, because that length is what
     * _nx_secure_tls_process_remote_certificate() bounds itself with.  Only a
     * record containing a message the vendored code cannot handle is taken
     * apart, and such a record never contains a certificate.
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

    /* Pass two, message by message. */
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
