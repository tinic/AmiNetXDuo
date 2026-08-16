/*
 * The tests for src/tlslib/tls_resume.c, on the host.
 *
 *   Every failure this file looks for is silent.  A resumption that does not
 *   happen looks exactly like a server that declines one, and costs seconds
 *   nobody attributes to this library.  A resumption that happens when it must
 *   not is worse and quieter still.  The library claims a certificate chain
 *   was checked when what happened was a master secret out of a file.  Neither
 *   prints anything.
 *
 *   Three rules here have no other coverage:
 *
 *     RFC 7627 5.4, only a session negotiated with the extended master secret
 *     is cached.  Without it the master secret is a function of two randoms
 *     the attacker also chose, so the same secret can be made to appear on two
 *     connections and the resumed handshake carries the authentication of
 *     neither.  That is the triple handshake.
 *
 *     RFC 7627 5.3, a ServerHello that resumes a cached session without the
 *     extension is refused.  The other half of the same rule.
 *
 *     'ATS3'.  The magic moved because an 'ATS2' file holds exactly the
 *     sessions the two rules above now refuse, and an old file must go quiet
 *     rather than produce a ServerHello mismatch on every connection.
 *
 *   What is exercised is the shipping code: tls_resume.c, tls_runtime.c and
 *   tls_expiry.c are compiled here as they ship, with no test #ifdef in any of
 *   them.  The disk mirror is written to and read back from a real file in a
 *   real directory, because the format is the part a different build of this
 *   same library has to agree with.
 *
 *   The four seams the test owns are the ones that are not arithmetic: the
 *   clock (so expiry can be asked about without waiting), the entropy pool,
 *   the session-to-connection registry, and the vendored nx_secure entry
 *   points the two --wrap functions call through.  Everything else is real.
 *
 *   The compile-time defines must match src/tlslib/CMakeLists.txt's
 *   tls_library target exactly, NX_SECURE_TLS_ENABLE_TLS_1_3 above all:
 *   tls_resume_secret_bound() has a live #if on it, and it changes the layout
 *   of NX_SECURE_TLS_SESSION underneath everything else here.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <exec/memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The two --wrap entry points.  They have no header: on the Amiga the linker
 * is what routes calls to them, so nothing ever names them in C.  Here the
 * test calls them directly, which is the same thing the linker does.
 */
UINT __wrap__nx_secure_tls_send_clienthello(NX_SECURE_TLS_SESSION *tls_session,
                                            NX_PACKET *send_packet);
UINT __wrap__nx_secure_tls_client_handshake(NX_SECURE_TLS_SESSION *tls_session,
                                            UCHAR *packet_buffer,
                                            UINT data_length,
                                            ULONG wait_option);

/* --------------------------------------------------------------- harness -- */

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------ the clock -- */

/*
 * tls_time.c reads DateStamp().  Here the number is set by the test, because
 * half the rules in tls_resume.c are about the difference between two of
 * them and waiting a day is not a test strategy.
 */
static ULONG h_now = TLS_CLOCK_FLOOR + 1000000UL;

ULONG tls_time_monotonic(VOID)
{
    return h_now;
}

/* ---------------------------------------------------------- the entropy -- */

/*
 * tls_resume_prepare() generates a session ID when the cached session has
 * none.  A counter rather than a PRNG: the test needs to know that thirty-two
 * bytes were generated and that two attempts do not produce the same handle,
 * and a fixed sequence says both.
 */
static int h_rand_calls;

int ami_random_rand(void)
{
    h_rand_calls++;
    return h_rand_calls * 0x0101;
}

/* ------------------------------------------------------------------ exec -- */

/*
 * The semaphore is counted, not ignored.  Every entry point in tls_resume.c
 * takes tb_Lock, several of them have early returns underneath it, and a path
 * that returns while still holding it deadlocks the next connection on a
 * machine where no scheduler will ever notice.
 */
static int h_sem_nest;
static int h_sem_max;

VOID ObtainSemaphore(struct SignalSemaphore *sem)
{
    (VOID)sem;
    h_sem_nest++;
    if (h_sem_nest > h_sem_max)
        h_sem_max = h_sem_nest;
}

VOID ReleaseSemaphore(struct SignalSemaphore *sem)
{
    (VOID)sem;
    h_sem_nest--;
}

static ULONG h_allocs;

APTR AllocVec(ULONG size, ULONG requirements)
{
    void *p;

    /* tls_alloc() asks for MEMF_CLEAR and tls_resume.c relies on it: a slot
       is only zeroed explicitly where the code says so. */
    CHECK((requirements & MEMF_CLEAR) != 0);

    p = calloc(1, size);
    if (p != NULL)
        h_allocs++;

    return p;
}

VOID FreeVec(APTR memory)
{
    if (memory != NULL)
    {
        h_allocs--;
        free(memory);
    }
}

static struct Library h_dos_library;

struct Library *OpenLibrary(STRPTR name, ULONG version)
{
    (VOID)name;
    (VOID)version;
    return &h_dos_library;
}

VOID CloseLibrary(struct Library *library)
{
    (VOID)library;
}

/* src/tlslib/tls_runtime.c calls this from tls_runtime_close(). */
VOID ami_tls_timer_close(VOID);
VOID ami_tls_timer_close(VOID)
{
}

/* ------------------------------------------------------------------- dos -- */

/*
 * The disk mirror against a real file.  Nothing is modelled.  The bytes
 * tls_resume_save() writes are the bytes on the disk, and tls_resume_load()
 * reads them back with Read(), so a record stride or an endianness that
 * drifted appears here the way it appears on an Amiga.
 */
static char h_dir[96];

static char h_path_a[160];
static char h_path_b[160];

static ULONG h_opens_failed;

BPTR Open(STRPTR name, LONG mode)
{
    FILE *fh = fopen((const char *)name,
                     (mode == MODE_NEWFILE) ? "wb" : "rb");

    if (fh == NULL)
        h_opens_failed++;

    return (BPTR)fh;
}

VOID Close(BPTR fh)
{
    if (fh != (BPTR)0)
        fclose((FILE *)fh);
}

LONG Read(BPTR fh, APTR buffer, LONG length)
{
    return (LONG)fread(buffer, 1, (size_t)length, (FILE *)fh);
}

LONG Write(BPTR fh, const void *buffer, LONG length)
{
    return (LONG)fwrite(buffer, 1, (size_t)length, (FILE *)fh);
}

/* ------------------------------------------------ the objects under test -- */

static struct TLSLibBase h_base;
static TLSConnection     h_conn;
static UBYTE             h_ticket_store[TLS_RESUME_TICKET_MAX];

/*
 * tls_store.c walks a registry under Forbid().  Here there is one connection,
 * and the wrap functions have to find it from the session they are handed.
 */
TLSConnection *tls_conn_for_session(const NX_SECURE_TLS_SESSION *session)
{
    if (session == &h_conn.tc_Session)
        return &h_conn;

    return NULL;
}

/* ------------------------------------------- the vendored nx_secure side -- */

TX_MUTEX _nx_secure_tls_protection;

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)mutex_ptr;
    (VOID)wait_option;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    (VOID)mutex_ptr;
    return TX_SUCCESS;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (VOID)packet_ptr;
    return NX_SUCCESS;
}

/*
 * The vendored handshake header parser: RFC 5246's four bytes, one type and a
 * 24-bit length.  header_bytes arrives holding what is left in the record and
 * leaves holding the header size, which is the contract the wrap relies on to
 * step from message to message.
 */
UINT _nx_secure_tls_process_handshake_header(UCHAR *packet_buffer,
                                             USHORT *message_type,
                                             UINT *header_bytes,
                                             UINT *message_length)
{
    if (*header_bytes < 4)
        return NX_SECURE_TLS_INCORRECT_MESSAGE_LENGTH;

    *message_type   = packet_buffer[0];
    *message_length = ((UINT)packet_buffer[1] << 16) |
                      ((UINT)packet_buffer[2] << 8) | (UINT)packet_buffer[3];
    *header_bytes   = 4;

    return NX_SUCCESS;
}

/* What the ServerHello the test is about to feed in will echo back. */
static UCHAR  h_echo_sid[TLS_RESUME_SID_MAX];
static UCHAR  h_echo_sid_length;
static UINT   h_real_handshake_status = NX_SUCCESS;
static int    h_real_handshake_calls;

UINT __real__nx_secure_tls_client_handshake(NX_SECURE_TLS_SESSION *tls_session,
                                            UCHAR *packet_buffer,
                                            UINT data_length,
                                            ULONG wait_option);

UINT __real__nx_secure_tls_client_handshake(NX_SECURE_TLS_SESSION *tls_session,
                                            UCHAR *packet_buffer,
                                            UINT data_length,
                                            ULONG wait_option)
{
    (VOID)data_length;
    (VOID)wait_option;

    h_real_handshake_calls++;

    if (data_length >= 1 && packet_buffer[0] == NX_SECURE_TLS_SERVER_HELLO)
    {
        tls_session->nx_secure_tls_session_id_length = h_echo_sid_length;
        memcpy(tls_session->nx_secure_tls_session_id, h_echo_sid,
               sizeof(h_echo_sid));
    }

    return h_real_handshake_status;
}

/*
 * Everything tls_resume_finish() calls.  It is the abbreviated handshake's
 * tail, five vendored calls in a fixed order, and the order is the part this
 * pins.  The client's Finished must be hashed over the server's, and the
 * SHA-256 context destroyed after that and not before.
 */
static char h_finish_log[128];

static void h_finish_note(const char *what)
{
    size_t used = strlen(h_finish_log);

    snprintf(h_finish_log + used, sizeof(h_finish_log) - used, "%s ", what);
}

static UINT h_process_finished_status = NX_SUCCESS;

UINT _nx_secure_tls_process_finished(NX_SECURE_TLS_SESSION *tls_session,
                                     UCHAR *packet_buffer, UINT message_length)
{
    (VOID)tls_session;
    (VOID)packet_buffer;
    (VOID)message_length;
    h_finish_note("verify");
    return h_process_finished_status;
}

/* Every message the transcript hash was fed, in order, so the test can say
   which ones counted.  RFC 5077 3.3 makes a NewSessionTicket one of them. */
static UCHAR h_hashed_types[16];
static UINT  h_hashed_count;

UINT _nx_secure_tls_handshake_hash_update(NX_SECURE_TLS_SESSION *tls_session,
                                          UCHAR *data, UINT length)
{
    (VOID)tls_session;
    (VOID)length;

    if (h_hashed_count < sizeof(h_hashed_types))
        h_hashed_types[h_hashed_count++] = data[0];

    h_finish_note("hash");

    return NX_SUCCESS;
}

static NX_PACKET h_send_packet;

UINT _nx_secure_tls_packet_allocate(NX_SECURE_TLS_SESSION *tls_session,
                                    NX_PACKET_POOL *pool_ptr,
                                    NX_PACKET **packet_ptr, ULONG wait_option)
{
    (VOID)tls_session;
    (VOID)pool_ptr;
    (VOID)wait_option;
    *packet_ptr = &h_send_packet;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_allocate_handshake_packet(NX_SECURE_TLS_SESSION *tls_session,
                                              NX_PACKET_POOL *packet_pool,
                                              NX_PACKET **packet_ptr,
                                              ULONG wait_option)
{
    (VOID)tls_session;
    (VOID)packet_pool;
    (VOID)wait_option;
    *packet_ptr = &h_send_packet;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_changecipherspec(NX_SECURE_TLS_SESSION *tls_session,
                                          NX_PACKET *send_packet)
{
    (VOID)tls_session;
    (VOID)send_packet;
    h_finish_note("ccs");
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_record(NX_SECURE_TLS_SESSION *tls_session,
                                NX_PACKET *send_packet, UCHAR record_type,
                                ULONG wait_option)
{
    (VOID)tls_session;
    (VOID)send_packet;
    (VOID)record_type;
    (VOID)wait_option;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_keys_set(NX_SECURE_TLS_SESSION *tls_session,
                                     USHORT key_set)
{
    (VOID)tls_session;
    (VOID)key_set;
    h_finish_note("keys");
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_finished(NX_SECURE_TLS_SESSION *tls_session,
                                  NX_PACKET *send_packet)
{
    (VOID)tls_session;
    (VOID)send_packet;
    h_finish_note("finished");
    return NX_SUCCESS;
}

UINT _nx_secure_tls_send_handshake_record(NX_SECURE_TLS_SESSION *tls_session,
                                          NX_PACKET *send_packet,
                                          UCHAR handshake_type,
                                          ULONG wait_option)
{
    (VOID)tls_session;
    (VOID)send_packet;
    (VOID)handshake_type;
    (VOID)wait_option;
    return NX_SUCCESS;
}

/* --------------------------------------------------------- the ClientHello */

/*
 * What the vendored _nx_secure_tls_send_clienthello() leaves behind, in the
 * shape __wrap__nx_secure_tls_send_clienthello() walks: version, random, a
 * zero session ID length, the ciphersuite list, the compression list, then an
 * extensions block that is exactly the rest of the message.
 *
 * Hand-built rather than taken from nx_secure, because the wrap's contract is
 * with RFC 5246's wire format and not with the vendored function.  A splice
 * that only works against one version of nx_secure's output is the bug this
 * catches.
 */
/*
 * Sized so the whole thing is the length a real one from this library is,
 * about 135 bytes plus the host name.  That is not decoration.  The ceiling
 * test below turns on a message long enough that a 256-byte ticket pushes it
 * past 500, and a toy ClientHello makes that test pass while it asserts
 * nothing.
 */
#define H_CH_EXT_BODY   180     /* one extension: 4 header + 176 payload */
#define H_CH_LENGTH     (2 + 32 + 1 + 2 + 4 + 1 + 1 + 2 + H_CH_EXT_BODY)

/*
 * A ClientHello so long that even the empty four-byte extension will not fit
 * under the 500-byte handshake cache.  495 + 4 header is 499; one extension
 * more is 503.
 */
#define H_CH_LONG_EXT   450
#define H_CH_LONG_LENGTH (2 + 32 + 1 + 2 + 4 + 1 + 1 + 2 + H_CH_LONG_EXT)

/*
 * h_ch_ext_total makes the vendored function claim an extensions block that is
 * not the rest of the message, which is the "layout I do not recognise" case.
 * h_ch_long selects the long message above.
 */
static UWORD h_ch_ext_total = H_CH_EXT_BODY;
static int   h_ch_long;

UINT __real__nx_secure_tls_send_clienthello(NX_SECURE_TLS_SESSION *tls_session,
                                            NX_PACKET *send_packet);

UINT __real__nx_secure_tls_send_clienthello(NX_SECURE_TLS_SESSION *tls_session,
                                            NX_PACKET *send_packet)
{
    UCHAR *p    = send_packet->nx_packet_append_ptr;
    ULONG  ext  = h_ch_long ? H_CH_LONG_EXT : H_CH_EXT_BODY;
    ULONG  full = h_ch_long ? H_CH_LONG_LENGTH : H_CH_LENGTH;
    UWORD  told = h_ch_long ? (UWORD)H_CH_LONG_EXT : h_ch_ext_total;
    ULONG  i;

    (VOID)tls_session;

    p[0] = 0x03; p[1] = 0x03;                   /* version               */
    for (i = 0; i < 32; i++)
        p[2 + i] = (UCHAR)(0xA0 + i);           /* random                */
    p[34] = 0;                                  /* session ID length     */
    p[35] = 0; p[36] = 4;                       /* ciphersuites, 4 bytes */
    p[37] = 0xC0; p[38] = 0x2F; p[39] = 0xC0; p[40] = 0x30;
    p[41] = 1; p[42] = 0;                       /* compression, null     */
    p[43] = (UCHAR)(told >> 8);                 /* extensions total      */
    p[44] = (UCHAR)told;
    p[45] = 0x00; p[46] = 0x00;                 /* server_name           */
    p[47] = (UCHAR)((ext - 4) >> 8);
    p[48] = (UCHAR)(ext - 4);
    for (i = 0; i < ext - 4; i++)
        p[49 + i] = (UCHAR)(0xD0 + (i & 0x0F));

    send_packet->nx_packet_append_ptr = p + full;
    send_packet->nx_packet_length    += full;

    return NX_SUCCESS;
}

/* ------------------------------------------------------------- fixtures -- */

static const NX_CRYPTO_METHOD          h_prf;
static NX_SECURE_TLS_CIPHERSUITE_INFO  h_suite;

static int h_keys_generated;

static UINT h_generate_session_keys(const NX_SECURE_TLS_CIPHERSUITE_INFO *ciphersuite,
                                    USHORT protocol_version,
                                    const NX_CRYPTO_METHOD *session_prf_method,
                                    NX_SECURE_TLS_KEY_MATERIAL *tls_key_material,
                                    UCHAR *master_sec, VOID *prf_metadata,
                                    ULONG prf_metadata_size)
{
    (VOID)ciphersuite;
    (VOID)protocol_version;
    (VOID)session_prf_method;
    (VOID)tls_key_material;
    (VOID)master_sec;
    (VOID)prf_metadata;
    (VOID)prf_metadata_size;

    h_keys_generated++;

    return NX_SUCCESS;
}

/* A master secret with no zero bytes in it, so a partly-copied one is not
   mistaken for a whole one. */
static UBYTE h_master[TLS_MASTER_SECRET_SIZE];
static UBYTE h_sid[TLS_RESUME_SID_MAX];
/* Sized for the largest take: the ceiling test copies TLS_RESUME_TICKET_MAX
   from it.  Most tests use the first 192 bytes. */
static UBYTE h_ticket[TLS_RESUME_TICKET_MAX];

static void fixtures_init(void)
{
    ULONG i;

    for (i = 0; i < TLS_MASTER_SECRET_SIZE; i++)
        h_master[i] = (UBYTE)(0x11 + i);
    for (i = 0; i < TLS_RESUME_SID_MAX; i++)
        h_sid[i] = (UBYTE)(0x40 + i);
    for (i = 0; i < sizeof(h_ticket); i++)
        h_ticket[i] = (UBYTE)(0x80 + (i & 0x3F));

    h_suite.nx_secure_tls_ciphersuite = 0xC02F;
    h_suite.nx_secure_tls_prf         = &h_prf;
}

/*
 * A connection as tls_conn.c would have left it: resumption on, persisting to
 * the given file, verified against a store with the given fingerprint.
 */
static TLSStore h_store;

static void conn_init(const char *host, UWORD port, const char *path,
                      ULONG fingerprint)
{
    memset(&h_conn, 0, sizeof(h_conn));

    h_conn.tc_Base   = &h_base;
    h_conn.tc_Flags  = TLSF_VERIFY;
    h_conn.tc_Port   = port;
    h_conn.tc_Ticket = h_ticket_store;

    h_conn.tc_ResumeFlags   = TLSR_ENABLED | TLSR_PERSIST;
    h_conn.tc_ExpiryChecked = TRUE;
    h_conn.tc_RemoteCount   = 2;
    h_conn.tc_CipherSuite   = 0;
    h_conn.tc_Protocol      = 0x0303;

    h_store.ts_Fingerprint = fingerprint;
    h_conn.tc_Store        = (fingerprint != 0) ? &h_store : NULL;

    strncpy((char *)h_conn.tc_HostName, host, sizeof(h_conn.tc_HostName) - 1);
    h_conn.tc_HostNameLength = (USHORT)strlen(host);

    tls_strncpy(h_conn.tc_SessionPath, path, sizeof(h_conn.tc_SessionPath));

    /* The session as a completed TLS 1.2 handshake leaves it. */
    h_conn.tc_Session.nx_secure_tls_session_ciphersuite = &h_suite;
    h_conn.tc_Session.nx_secure_generate_session_keys   = h_generate_session_keys;
    h_conn.tc_Session.nx_secure_tls_extended_master_secret = NX_TRUE;
#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    h_conn.tc_Session.nx_secure_tls_1_3 = NX_FALSE;
#endif

    memcpy(h_conn.tc_Session.nx_secure_tls_key_material.nx_secure_tls_master_secret,
           h_master, TLS_MASTER_SECRET_SIZE);
    h_conn.tc_Session.nx_secure_tls_session_id_length = TLS_RESUME_SID_MAX;
    memcpy(h_conn.tc_Session.nx_secure_tls_session_id, h_sid,
           TLS_RESUME_SID_MAX);

    h_real_handshake_calls = 0;
    h_hashed_count         = 0;
    h_finish_log[0]        = '\0';
}

/* Everything the library base forgets when it is expunged. */
static void base_reset(void)
{
    if (h_base.tb_Sessions != NULL)
    {
        FreeVec(h_base.tb_Sessions);
        h_base.tb_Sessions = NULL;
    }

    memset(&h_base, 0, sizeof(h_base));
}

/* A NewSessionTicket the server issued, as tls_conn.c would have seen it. */
static void conn_take_ticket(UWORD length, ULONG lifetime)
{
    memcpy(h_conn.tc_Ticket, h_ticket, length);
    h_conn.tc_TicketLength   = length;
    h_conn.tc_TicketLifetime = lifetime;
    h_conn.tc_ResumeFlags   |= TLSR_TICKET_NEW;
}

static ULONG file_size(const char *path)
{
    FILE *fh = fopen(path, "rb");
    long  n;

    if (fh == NULL)
        return 0;

    fseek(fh, 0, SEEK_END);
    n = ftell(fh);
    fclose(fh);

    return (ULONG)n;
}

/* ================================================================ 'ATS3' == */

/*
 * A whole session out to disk and back.  Every field, because the file is
 * read by a different build of this same library and a field that survives
 * the round trip in memory but not on disk is a resumption that silently
 * stops happening.
 */
static void test_mirror_round_trip(void)
{
    TLSResumeEntry *e;

    printf("tls_resume: a session survives the disk mirror\n");

    unlink(h_path_a);
    base_reset();

    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    h_conn.tc_CipherSuite = 0xC02F;     /* as tls_conn.c leaves it */
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    CHECK(tls_resume_count(&h_base) == 1);
    CHECK(file_size(h_path_a) == 16 + 424);

    /* A reboot: the cache is gone, the file is not. */
    base_reset();
    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);

    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
    CHECK(h_conn.tc_TicketLength == 192);
    CHECK(memcmp(h_conn.tc_Ticket, h_ticket, 192) == 0);
    CHECK(memcmp(h_conn.tc_Master, h_master, TLS_MASTER_SECRET_SIZE) == 0);
    CHECK(h_conn.tc_OfferSidLength == TLS_RESUME_SID_MAX);
    CHECK(memcmp(h_conn.tc_OfferSid, h_sid, TLS_RESUME_SID_MAX) == 0);

    e = &h_base.tb_Sessions[0];
    CHECK(e->re_Port == 443);
    CHECK(e->re_CipherSuite == 0xC02F);
    CHECK(e->re_Protocol == 0x0303);
    CHECK(e->re_Lifetime == 3600);
    CHECK(e->re_Flags == (TLSRE_VERIFIED | TLSRE_DATED));
    CHECK(e->re_MaxChain == 2);
    CHECK(strcmp(e->re_Host, "example.com") == 0);

    CHECK(h_sem_nest == 0);
}

/*
 * The RFC 7627 magic bump.  An 'ATS2' file is byte-for-byte an 'ATS3' one, so
 * nothing but the magic tells them apart, and everything in it is a session
 * negotiated before the extended master secret was required.  It must be
 * ignored rather than offered.  An offer produces a ServerHello mismatch on
 * every connection to that host, which costs a round trip and a full
 * handshake, forever, and looks like a difficult server.
 */
static void test_ats2_file_is_ignored(void)
{
    FILE *fh;
    UBYTE magic[4];

    printf("tls_resume: an 'ATS2' file is ignored, not offered\n");

    unlink(h_path_a);
    base_reset();

    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    /* Downgrade the magic in place and change nothing else. */
    fh = fopen(h_path_a, "r+b");
    CHECK(fh != NULL);
    if (fh != NULL)
    {
        magic[0] = 'A'; magic[1] = 'T'; magic[2] = 'S'; magic[3] = '2';
        fwrite(magic, 1, 4, fh);
        fclose(fh);
    }

    base_reset();
    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);

    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
    CHECK(tls_resume_count(&h_base) == 0);

    /* And it goes quiet for exactly one connection: the next session written
       replaces the file with an 'ATS3' one. */
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    base_reset();
    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
}

/*
 * A file that is not a whole number of records.  tls_resume_load() reads
 * fixed strides precisely so a truncated file cannot be mis-parsed, and the
 * records before the cut are still sessions.
 */
static void test_truncated_file(void)
{
    FILE *fh;
    long  whole;
    char *bytes;
    ULONG n;

    printf("tls_resume: a truncated file keeps the records before the cut\n");

    unlink(h_path_a);
    base_reset();

    conn_init("one.example", 443, h_path_a, 0xCAFEBABEUL);
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("two.example", 443, h_path_a, 0xCAFEBABEUL);
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    CHECK(file_size(h_path_a) == 16 + 2 * 424);

    /* Cut the second record in half; the header still claims two. */
    whole = 16 + 424 + 200;
    bytes = malloc((size_t)whole);
    CHECK(bytes != NULL);
    if (bytes == NULL)
        return;

    fh = fopen(h_path_a, "rb");
    n  = (ULONG)fread(bytes, 1, (size_t)whole, fh);
    fclose(fh);
    CHECK(n == (ULONG)whole);

    fh = fopen(h_path_a, "wb");
    fwrite(bytes, 1, (size_t)whole, fh);
    fclose(fh);
    free(bytes);

    base_reset();
    conn_init("one.example", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);

    CHECK(tls_resume_count(&h_base) == 1);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);

    base_reset();
    conn_init("two.example", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
}

/*
 * The three shapes tls_resume_decode() refuses.  All three are what a
 * zero-filled or half-written file looks like, and none of them must come back
 * as a session.  A record with no trust key cannot be matched against anything
 * the live code computes, and one with neither a session ID nor a ticket has
 * nothing to offer a server.
 */
static void test_decode_rejects(void)
{
    static const struct {
        const char *what;
        ULONG       offset;     /* into the record */
        UBYTE       value;
    } poke[] = {
        { "a record with no host name",     0,   0x00 },
        { "a session ID longer than 32",    70,  33   },
        { "a record with no trust key",     112, 0x00 },
    };
    ULONG i;

    printf("tls_resume: a corrupt record is refused, not clamped\n");

    for (i = 0; i < sizeof(poke) / sizeof(poke[0]); i++)
    {
        FILE *fh;
        long  pos;

        unlink(h_path_a);
        base_reset();

        conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
        conn_take_ticket(192, 3600);
        tls_resume_record(&h_conn);

        fh  = fopen(h_path_a, "r+b");
        pos = 16 + (long)poke[i].offset;
        fseek(fh, pos, SEEK_SET);
        fputc(poke[i].value, fh);

        /* The trust key is four bytes; one zeroed byte is not enough. */
        if (poke[i].offset == 112)
        {
            fputc(0, fh);
            fputc(0, fh);
            fputc(0, fh);
        }
        fclose(fh);

        base_reset();
        conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
        tls_resume_prepare(&h_conn);

        checks++;
        if (tls_resume_count(&h_base) != 0 ||
            (h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0)
        {
            failures++;
            printf("  FAIL %s came back as a session\n", poke[i].what);
        }
    }
}

/*
 * TLSA_SessionFile naming a different file is a different cache, not an
 * addition to this one.  A mistake here means the tag is honoured on the
 * write and ignored on the read, and a caller that asked for its own session
 * file gets somebody else's master secrets.
 */
static void test_a_different_file_is_a_different_cache(void)
{
    printf("tls_resume: a different session file is a different cache\n");

    unlink(h_path_a);
    unlink(h_path_b);
    base_reset();

    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);
    CHECK(tls_resume_count(&h_base) == 1);

    /* Same library base, same host, a different file: nothing to offer. */
    conn_init("example.com", 443, h_path_b, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
    CHECK(tls_resume_count(&h_base) == 0);

    /* And back again. */
    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
}

/* An empty TLSA_SessionFile is RAM only: a cache, but never a file. */
static void test_empty_path_writes_nothing(void)
{
    printf("tls_resume: an empty session file keeps the cache in RAM\n");

    unlink(h_path_a);
    base_reset();

    conn_init("example.com", 443, "", 0xCAFEBABEUL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    CHECK(tls_resume_count(&h_base) == 1);
    CHECK(file_size(h_path_a) == 0);
}

/* ========================================================= RFC 7627 5.4 == */

/*
 * The caching half.  A session negotiated without the extended master secret
 * is not written down at all: not to the resident cache, not to the file.
 * The negative is the whole point, so the positive is asserted beside it from
 * the same fixture.  Otherwise a tls_resume_record() that stopped working
 * entirely passes this.
 */
static void test_no_ems_is_not_cached(void)
{
    printf("tls_resume: RFC 7627, a session without EMS is not cached\n");

    unlink(h_path_a);
    base_reset();

    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    conn_take_ticket(192, 3600);
    h_conn.tc_Session.nx_secure_tls_extended_master_secret = NX_FALSE;
    tls_resume_record(&h_conn);

    CHECK(tls_resume_count(&h_base) == 0);
    CHECK(file_size(h_path_a) == 0);

    /* The same fixture with the extension: cached, and on disk. */
    conn_init("example.com", 443, h_path_a, 0xCAFEBABEUL);
    h_conn.tc_CipherSuite = 0xC02F;     /* as tls_conn.c leaves it */
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    CHECK(tls_resume_count(&h_base) == 1);
    CHECK(file_size(h_path_a) == 16 + 424);
}

/*
 * A session ID and no ticket is still a session, and still subject to the
 * rule: an intranet server with no ticket support resumes by session ID, and
 * that path must refuse EMS-less sessions too.
 */
static void test_no_ems_session_id_only(void)
{
    printf("tls_resume: the rule holds for a session-ID-only session\n");

    base_reset();

    conn_init("intranet.local", 443, "", 0xCAFEBABEUL);
    h_conn.tc_Session.nx_secure_tls_extended_master_secret = NX_FALSE;
    tls_resume_record(&h_conn);
    CHECK(tls_resume_count(&h_base) == 0);

    conn_init("intranet.local", 443, "", 0xCAFEBABEUL);
    tls_resume_record(&h_conn);
    CHECK(tls_resume_count(&h_base) == 1);
}

/* ========================================================= RFC 7627 5.3 == */

/*
 * The ServerHello half, through the real __wrap__nx_secure_tls_client_handshake().
 *
 * A ServerHello that echoes the offered session ID is the acceptance signal,
 * and it is the only one a TLS 1.2 client gets.  If it arrives without the
 * extended master secret, the cached session cannot be the one resumed, or
 * the server is downgrading.  Either way the handshake must stop.  Otherwise
 * the record keys are derived from a secret bound to nothing.
 */
static UCHAR h_serverhello[8];

static void serverhello_init(void)
{
    h_serverhello[0] = NX_SECURE_TLS_SERVER_HELLO;
    h_serverhello[1] = 0;
    h_serverhello[2] = 0;
    h_serverhello[3] = 4;
    h_serverhello[4] = 0x03;
    h_serverhello[5] = 0x03;
    h_serverhello[6] = 0x00;
    h_serverhello[7] = 0x00;
}

/* A connection that has offered the cached session and is about to be told
   whether the server took it. */
static void offer_init(void)
{
    base_reset();

    conn_init("example.com", 443, "", 0xCAFEBABEUL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xCAFEBABEUL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    tls_resume_prepare(&h_conn);

    /* The server echoes what was offered: acceptance. */
    memcpy(h_echo_sid, h_conn.tc_OfferSid, TLS_RESUME_SID_MAX);
    h_echo_sid_length = h_conn.tc_OfferSidLength;

    /* The cached master secret must be visibly different from whatever the
       session already held, or "restored" cannot be told from "unchanged". */
    memset(h_conn.tc_Session.nx_secure_tls_key_material.nx_secure_tls_master_secret,
           0, TLS_MASTER_SECRET_SIZE);

    h_keys_generated = 0;
}

static void test_serverhello_resumes(void)
{
    UINT status;

    printf("tls_resume: an echoed session ID restores the cached secret\n");

    serverhello_init();
    offer_init();

    status = __wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session,
                                                    h_serverhello,
                                                    sizeof(h_serverhello), 0);

    CHECK(status == NX_SUCCESS);
    CHECK((h_conn.tc_ResumeFlags & TLSR_RESUMED) != 0);
    CHECK(memcmp(h_conn.tc_Session.nx_secure_tls_key_material.nx_secure_tls_master_secret,
                 h_master, TLS_MASTER_SECRET_SIZE) == 0);
    CHECK(h_keys_generated == 1);
    CHECK(h_conn.tc_Session.nx_secure_tls_received_remote_credentials == NX_TRUE);
    CHECK(h_conn.tc_Session.nx_secure_tls_client_state ==
          NX_SECURE_TLS_CLIENT_STATE_SERVERHELLO_DONE);
    CHECK(h_conn.tc_CipherSuite == 0xC02F);
}

static void test_serverhello_without_ems_is_refused(void)
{
    UINT status;

    printf("tls_resume: RFC 7627, a resumption without EMS is refused\n");

    serverhello_init();
    offer_init();

    h_conn.tc_Session.nx_secure_tls_extended_master_secret = NX_FALSE;

    status = __wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session,
                                                    h_serverhello,
                                                    sizeof(h_serverhello), 0);

    CHECK(status == NX_SECURE_TLS_DOWNGRADE_DETECTED);
    CHECK((h_conn.tc_ResumeFlags & TLSR_RESUMED) == 0);

    /* Nothing derived, and the session's own master secret left alone.  A
       failure that half-applied the cached secret is worse than one that did
       nothing. */
    CHECK(h_keys_generated == 0);
}

/*
 * A server that resumes a session and then changes the ciphersuite is broken
 * in a way this code cannot recover from mid-handshake.  It must fail rather
 * than derive keys with the wrong PRF.
 */
static void test_serverhello_changing_ciphersuite(void)
{
    NX_SECURE_TLS_CIPHERSUITE_INFO other = { 0 };
    UINT                           status;

    printf("tls_resume: a resumption that changes ciphersuite fails\n");

    serverhello_init();
    offer_init();

    other.nx_secure_tls_ciphersuite = 0x009C;
    other.nx_secure_tls_prf         = &h_prf;
    h_conn.tc_Session.nx_secure_tls_session_ciphersuite = &other;
    h_conn.tc_CipherSuite = 0xC02F;

    status = __wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session,
                                                    h_serverhello,
                                                    sizeof(h_serverhello), 0);

    CHECK(status == NX_SECURE_TLS_UNKNOWN_CIPHERSUITE);
    CHECK((h_conn.tc_ResumeFlags & TLSR_RESUMED) == 0);
    CHECK(h_keys_generated == 0);
}

/*
 * The ordinary decline: a server that ignores the offer echoes something
 * else, and the handshake carries on as a full one.  This must not be an
 * error, and nothing must be restored.
 */
static void test_serverhello_declines(void)
{
    UINT status;

    printf("tls_resume: a server that declines gets a full handshake\n");

    serverhello_init();
    offer_init();

    memset(h_echo_sid, 0x5A, sizeof(h_echo_sid));
    h_echo_sid_length = TLS_RESUME_SID_MAX;

    status = __wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session,
                                                    h_serverhello,
                                                    sizeof(h_serverhello), 0);

    CHECK(status == NX_SUCCESS);
    CHECK((h_conn.tc_ResumeFlags & TLSR_RESUMED) == 0);
    CHECK(h_keys_generated == 0);
}

/* An empty echo is the other way a server declines, and a zero-length
   comparison must not read as a match. */
static void test_serverhello_empty_echo(void)
{
    UINT status;

    printf("tls_resume: an empty echoed session ID is not a match\n");

    serverhello_init();
    offer_init();

    h_echo_sid_length = 0;
    memset(h_echo_sid, 0, sizeof(h_echo_sid));

    status = __wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session,
                                                    h_serverhello,
                                                    sizeof(h_serverhello), 0);

    CHECK(status == NX_SUCCESS);
    CHECK((h_conn.tc_ResumeFlags & TLSR_RESUMED) == 0);
}

/* ====================================================== the trust key ==== */

/*
 * A resumed handshake checks nothing: no certificate, no signature, no host
 * name.  So the cache key must name the trust decision completely.  Otherwise
 * the library resumes across a boundary a full handshake never crosses, and
 * reports a check it did not perform.
 *
 * Each row changes exactly one thing about the caller and expects no offer.
 */
static void test_trust_key_discriminates(void)
{
    printf("tls_resume: a session is not offered under different trust\n");

    /* Store it once, verified, dated, two certificates deep, fingerprint A. */
    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);
    CHECK(tls_resume_count(&h_base) == 1);

    /* A different trust store. */
    conn_init("example.com", 443, "", 0xBBBB0002UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    /* TLSA_NoVerify. */
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_Flags &= ~(ULONG)TLSF_VERIFY;
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    /* A machine whose clock was not set, so the dates were not checked. */
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ExpiryChecked = FALSE;
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    /* A caller willing to accept a shallower chain. */
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_RemoteCount = 1;
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    /* A different port on the same host. */
    conn_init("example.com", 8443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    /* A different host. */
    conn_init("example.net", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    /* Everything the same: the control, and the reason the rows above mean
       anything.  Six negatives with a broken tls_resume_find() would all
       pass. */
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);

    /* And DNS names are case-insensitive, so this is the same session. */
    conn_init("Example.COM", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
}

/* No host name, no resumption: the cache is keyed by the name the caller
   asked for, and there is no name here to key on. */
static void test_no_host_name(void)
{
    printf("tls_resume: a connection with no host name offers nothing\n");

    base_reset();

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_HostName[0]    = '\0';
    h_conn.tc_HostNameLength = 0;
    tls_resume_prepare(&h_conn);

    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
    CHECK((h_conn.tc_ResumeFlags & TLSR_ENABLED) == 0);
}

/* ======================================================= expiry and LRU == */

/*
 * An aged-out entry is wiped by tls_resume_prepare(), not merely skipped.  A
 * cached master secret is a key on disk, and one left sitting in a slot it
 * still occupies is both a key that outlives its own rule and a slot the next
 * session cannot have.
 */
static void test_expired_entry_is_wiped(void)
{
    printf("tls_resume: an aged-out entry is wiped, not skipped\n");

    base_reset();

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 300);         /* the server said five minutes */
    tls_resume_record(&h_conn);
    CHECK(tls_resume_count(&h_base) == 1);

    h_now += 301;

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);

    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
    CHECK(tls_resume_count(&h_base) == 0);

    h_now -= 301;
}

/*
 * Nine hosts into eight slots.  The victim is the least recently used, not
 * the least recently stored, so a session that is still offered keeps its slot
 * even though it was the first one written.
 */
static void test_lru_eviction(void)
{
    char  host[32];
    ULONG i;

    printf("tls_resume: the ninth session evicts the least recently used\n");

    base_reset();

    for (i = 0; i < TLS_RESUME_SLOTS; i++)
    {
        snprintf(host, sizeof(host), "h%lu.example", (unsigned long)i);
        conn_init(host, 443, "", 0xAAAA0001UL);
        h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
        conn_take_ticket(192, 3600);
        tls_resume_record(&h_conn);
    }

    CHECK(tls_resume_count(&h_base) == TLS_RESUME_SLOTS);

    /* Touch the oldest, so it is no longer the least recently used. */
    conn_init("h0.example", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);

    /* One more host than there are slots. */
    conn_init("h8.example", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);
    CHECK(tls_resume_count(&h_base) == TLS_RESUME_SLOTS);

    /* h1 was the least recently used and is gone.  h0 was touched and
       stayed. */
    conn_init("h1.example", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);

    conn_init("h0.example", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);

    conn_init("h8.example", 443, "", 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
}

/* A second resumption of the same host reuses its slot rather than takes
   another.  Otherwise eight hosts exhaust the cache in eight connections. */
static void test_restore_reuses_the_slot(void)
{
    printf("tls_resume: a second session for a host reuses its slot\n");

    base_reset();

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    CHECK(tls_resume_count(&h_base) == 1);
}

/* tls_resume_evict() is what TLSOpenA calls when a handshake failed, so the
   retry is a clean full handshake rather than the same failure forever. */
static void test_evict(void)
{
    printf("tls_resume: an evicted session is gone from RAM and disk\n");

    unlink(h_path_a);
    base_reset();

    conn_init("example.com", 443, h_path_a, 0xAAAA0001UL);
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);
    CHECK(file_size(h_path_a) == 16 + 424);

    conn_init("example.com", 443, h_path_a, 0xAAAA0001UL);
    tls_resume_evict(&h_conn);

    CHECK(tls_resume_count(&h_base) == 0);
    CHECK(file_size(h_path_a) == 16);

    base_reset();
    conn_init("example.com", 443, h_path_a, 0xAAAA0001UL);
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
}

/* ================================================= the ClientHello splice = */

static UCHAR    h_packet[1024];
static NX_PACKET h_ch_packet;

static void ch_packet_init(ULONG capacity)
{
    memset(h_packet, 0, sizeof(h_packet));
    memset(&h_ch_packet, 0, sizeof(h_ch_packet));

    h_ch_packet.nx_packet_prepend_ptr = h_packet;
    h_ch_packet.nx_packet_append_ptr  = h_packet;
    h_ch_packet.nx_packet_data_start  = h_packet;
    h_ch_packet.nx_packet_data_end    = h_packet + capacity;
    h_ch_packet.nx_packet_length      = 0;
}

static UWORD be16(const UCHAR *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

/*
 * A first connection, nothing cached.  The empty session_ticket extension is
 * how a client asks to be issued one.  Without it the first handshake never
 * produces anything to resume from, and resumption never starts working at
 * all.
 */
static void test_clienthello_asks_for_a_ticket(void)
{
    UWORD ext_total;

    printf("tls_resume: a first ClientHello asks for a ticket\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;

    ch_packet_init(sizeof(h_packet));
    CHECK(__wrap__nx_secure_tls_send_clienthello(&h_conn.tc_Session,
                                                 &h_ch_packet) == NX_SUCCESS);

    CHECK(h_packet[34] == 0);                       /* no session ID */
    CHECK(h_ch_packet.nx_packet_length == H_CH_LENGTH + 4);

    ext_total = be16(&h_packet[43]);
    CHECK(ext_total == H_CH_EXT_BODY + 4);

    /* The extension is last, after everything the vendored code wrote. */
    CHECK(be16(&h_packet[45 + H_CH_EXT_BODY]) == 0x0023);
    CHECK(be16(&h_packet[45 + H_CH_EXT_BODY + 2]) == 0);

    /* The vendored code's own extension is where it was. */
    CHECK(be16(&h_packet[45]) == 0x0000);
    CHECK(h_packet[49] == 0xD0 && h_packet[45 + H_CH_EXT_BODY - 1] == 0xDF);
}

/*
 * A cached session goes out as both: the session ID spliced into the middle
 * of the message and the ticket in the extension at the end.  The splice
 * shifts everything after byte 35, so what is checked is that the bytes that
 * moved are the bytes that were there.
 */
static void test_clienthello_offers_the_session(void)
{
    ULONG ext_offset = 43 + TLS_RESUME_SID_MAX;
    ULONG ext_start  = 45 + TLS_RESUME_SID_MAX;

    printf("tls_resume: a cached session goes out as ID and ticket\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);

    ch_packet_init(sizeof(h_packet));
    CHECK(__wrap__nx_secure_tls_send_clienthello(&h_conn.tc_Session,
                                                 &h_ch_packet) == NX_SUCCESS);

    /* The session ID, in the slot the vendored code wrote as zero. */
    CHECK(h_packet[34] == TLS_RESUME_SID_MAX);
    CHECK(memcmp(&h_packet[35], h_sid, TLS_RESUME_SID_MAX) == 0);

    /* Everything after it moved up by exactly that much and is intact. */
    CHECK(be16(&h_packet[35 + TLS_RESUME_SID_MAX]) == 4);       /* suites   */
    CHECK(h_packet[41 + TLS_RESUME_SID_MAX] == 1);              /* compress */
    CHECK(be16(&h_packet[ext_start]) == 0x0000);                /* server_name */
    CHECK(h_packet[ext_start + 4] == 0xD0);
    CHECK(h_packet[ext_start + H_CH_EXT_BODY - 1] == 0xDF);

    /* The extensions block grew by the session_ticket extension. */
    CHECK(be16(&h_packet[ext_offset]) == H_CH_EXT_BODY + 4 + 192);
    CHECK(be16(&h_packet[ext_start + H_CH_EXT_BODY]) == 0x0023);
    CHECK(be16(&h_packet[ext_start + H_CH_EXT_BODY + 2]) == 192);
    CHECK(memcmp(&h_packet[ext_start + H_CH_EXT_BODY + 4], h_ticket, 192) == 0);

    CHECK(h_ch_packet.nx_packet_length ==
          H_CH_LENGTH + TLS_RESUME_SID_MAX + 4 + 192);
}

/*
 * The 500-byte ceiling.  _nx_secure_tls_send_handshake_record() memcpy()s the
 * whole ClientHello into NX_SECURE_TLS_SESSION's own 500-byte array with no
 * bounds check, so an overrun writes through the middle of the session
 * struct on a machine with no memory protection.  The ticket is dropped and
 * the handshake falls back to a full one, which is slow and correct.
 *
 * The cached session here has a ticket and no session ID of its own, which is
 * what a ticket-issuing server leaves behind (RFC 5077 3.4, and measured on
 * nginx).  tls_resume_prepare() generates a session ID for it, and when the
 * ticket then does not fit, the generated ID is not an acceptance signal for
 * anything, so TLSR_OFFERED must come off with it.
 */
static void test_clienthello_ticket_over_the_ceiling(void)
{
    printf("tls_resume: a ticket that would overrun the cache is dropped\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;

    /* A session with a ticket and no ID: what nginx leaves behind. */
    h_conn.tc_Session.nx_secure_tls_session_id_length = 0;
    conn_take_ticket(TLS_RESUME_TICKET_MAX, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    tls_resume_prepare(&h_conn);

    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
    CHECK(h_conn.tc_TicketLength == TLS_RESUME_TICKET_MAX);
    CHECK(h_conn.tc_OfferSidLength == TLS_RESUME_SID_MAX);  /* generated */

    /* H_CH_LENGTH + 32 + 4 + 256 + 4 is over 500. */
    CHECK((ULONG)(H_CH_LENGTH + TLS_RESUME_SID_MAX + 4 +
                  TLS_RESUME_TICKET_MAX + 4) > TLS_CLIENTHELLO_CACHE_MAX);

    ch_packet_init(sizeof(h_packet));
    CHECK(__wrap__nx_secure_tls_send_clienthello(&h_conn.tc_Session,
                                                 &h_ch_packet) == NX_SUCCESS);

    /* Under the ceiling, and the ticket is not on the wire. */
    CHECK(h_ch_packet.nx_packet_length + 4 <= TLS_CLIENTHELLO_CACHE_MAX);
    CHECK(be16(&h_packet[45 + TLS_RESUME_SID_MAX + H_CH_EXT_BODY + 2]) == 0);

    /*
     * The session ID stays on the wire and TLSR_OFFERED stays set.
     *
     * That is what the code does, and it is deliberate for the other shape of
     * cached session, the one whose session ID came from a server rather than
     * from ami_random_rand().  A dropped ticket still leaves a legitimate
     * RFC 5246 session-ID resumption to attempt.
     *
     * For the shape here, a generated handle with nothing behind it, the
     * offer is meaningless.  It costs 32 bytes, and a server cannot echo an ID
     * it never issued.  If one does, the restored master secret does not match
     * and the handshake dies at the server's Finished, which is a failure and
     * not a false trust claim.  tls_resume.c:1134 describes a guard for
     * exactly this and spells it `sid_length == 0`, which cannot be true here
     * because tls_resume_prepare() generates the ID precisely when a ticket is
     * present -- so the guard never fires for the case it names.
     */
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);
    CHECK(h_packet[34] == TLS_RESUME_SID_MAX);
}

/*
 * The other end of the same ceiling: a ClientHello where not even the empty
 * four-byte extension fits under 500.  Here TLSR_OFFERED does come off, and it
 * must, because the message goes out with no session ID and no ticket, and
 * anything the server echoes is its own.
 */
static void test_clienthello_cache_ceiling_leaves_nothing(void)
{
    printf("tls_resume: a ClientHello at the cache ceiling offers nothing\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    tls_resume_prepare(&h_conn);
    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) != 0);

    /* A vendored ClientHello long enough that 4 more bytes overrun the
       500-byte handshake cache: 492 body + 4 header + 4 extension. */
    h_ch_ext_total = 0;
    h_ch_long      = 1;

    ch_packet_init(sizeof(h_packet));
    CHECK(__wrap__nx_secure_tls_send_clienthello(&h_conn.tc_Session,
                                                 &h_ch_packet) == NX_SUCCESS);

    h_ch_long      = 0;
    h_ch_ext_total = H_CH_EXT_BODY;

    CHECK((h_conn.tc_ResumeFlags & TLSR_OFFERED) == 0);
    CHECK(h_packet[34] == 0);
    CHECK(h_ch_packet.nx_packet_length == H_CH_LONG_LENGTH);
}

/*
 * A packet with no room even for an empty extension.  The message must be
 * left exactly as the vendored code wrote it.  A partial splice is a corrupt
 * ClientHello, which is worse than no resumption.
 */
static void test_clienthello_no_room_at_all(void)
{
    UCHAR before[H_CH_LENGTH];

    printf("tls_resume: no room means the message is left alone\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;

    ch_packet_init(H_CH_LENGTH + 2);
    CHECK(__wrap__nx_secure_tls_send_clienthello(&h_conn.tc_Session,
                                                 &h_ch_packet) == NX_SUCCESS);

    memcpy(before, h_packet, sizeof(before));

    CHECK(h_ch_packet.nx_packet_length == H_CH_LENGTH);
    CHECK(h_packet[34] == 0);
    CHECK(be16(&h_packet[43]) == H_CH_EXT_BODY);
    CHECK(h_ch_packet.nx_packet_append_ptr == h_packet + H_CH_LENGTH);
    CHECK(memcmp(before, h_packet, sizeof(before)) == 0);
}

/*
 * A message whose extensions block is not exactly the rest of it is not the
 * shape the splice assumes.  The walk happens before anything is written for
 * exactly this case, so the message comes out untouched rather than
 * half-spliced.
 */
static void test_clienthello_unfamiliar_layout(void)
{
    printf("tls_resume: an unfamiliar layout is not spliced\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    conn_take_ticket(192, 3600);
    tls_resume_record(&h_conn);

    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;
    tls_resume_prepare(&h_conn);

    /* One byte fewer of extensions than are actually there. */
    h_ch_ext_total = (UWORD)(H_CH_EXT_BODY - 1);

    ch_packet_init(sizeof(h_packet));
    CHECK(__wrap__nx_secure_tls_send_clienthello(&h_conn.tc_Session,
                                                 &h_ch_packet) == NX_SUCCESS);

    h_ch_ext_total = H_CH_EXT_BODY;

    CHECK(h_packet[34] == 0);                       /* no session ID spliced */
    CHECK(be16(&h_packet[43]) == H_CH_EXT_BODY - 1);
    CHECK(h_ch_packet.nx_packet_length == H_CH_LENGTH);
    CHECK(h_ch_packet.nx_packet_append_ptr == h_packet + H_CH_LENGTH);
}

/* ============================================ the NewSessionTicket parse == */

/*
 * RFC 5077 3.3: uint32 lifetime_hint, then opaque ticket<0..2^16-1>.  The
 * message arrives from a server before anything has authenticated it, so
 * every length in it is somebody else's number.
 */
static UCHAR h_nst[8 + TLS_RESUME_TICKET_MAX + 64];

static UINT nst_build(ULONG lifetime, ULONG declared, ULONG actual)
{
    ULONG body = 6 + actual;
    ULONG i;

    h_nst[0] = NX_SECURE_TLS_NEW_SESSION_TICKET;
    h_nst[1] = (UCHAR)(body >> 16);
    h_nst[2] = (UCHAR)(body >> 8);
    h_nst[3] = (UCHAR)body;

    h_nst[4] = (UCHAR)(lifetime >> 24);
    h_nst[5] = (UCHAR)(lifetime >> 16);
    h_nst[6] = (UCHAR)(lifetime >> 8);
    h_nst[7] = (UCHAR)lifetime;

    h_nst[8] = (UCHAR)(declared >> 8);
    h_nst[9] = (UCHAR)declared;

    for (i = 0; i < actual; i++)
        h_nst[10 + i] = (UCHAR)(0x80 + (i & 0x3F));

    return (UINT)(4 + body);
}

static void test_new_session_ticket(void)
{
    UINT length;

    printf("tls_resume: a NewSessionTicket is kept and hashed\n");

    base_reset();
    conn_init("example.com", 443, "", 0xAAAA0001UL);
    h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;

    h_hashed_count = 0;
    length = nst_build(7200, 192, 192);

    CHECK(__wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session, h_nst,
                                                 length, 0) == NX_SUCCESS);

    CHECK((h_conn.tc_ResumeFlags & TLSR_TICKET_NEW) != 0);
    CHECK(h_conn.tc_TicketLength == 192);
    CHECK(h_conn.tc_TicketLifetime == 7200);
    CHECK(memcmp(h_conn.tc_Ticket, &h_nst[10], 192) == 0);

    /* RFC 5077 3.3: it counts toward the transcript.  A Finished computed
       without it fails, which is a handshake that dies at the last message
       with nothing to point at. */
    CHECK(h_hashed_count == 1);
    CHECK(h_hashed_types[0] == NX_SECURE_TLS_NEW_SESSION_TICKET);

    /* The vendored state machine has no case for this message and its
       default leaves NX_SECURE_TLS_HANDSHAKE_FAILURE, so it must not see it. */
    CHECK(h_real_handshake_calls == 0);
}

static void test_new_session_ticket_bounds(void)
{
    static const struct {
        const char *what;
        ULONG       declared;
        ULONG       actual;
    } row[] = {
        { "a ticket longer than the cap",   TLS_RESUME_TICKET_MAX + 1,
                                            TLS_RESUME_TICKET_MAX + 1 },
        { "a length past the message",      64, 8  },
        { "an empty ticket",                0,  0  },
    };
    ULONG i;

    printf("tls_resume: a NewSessionTicket that does not fit is refused\n");

    for (i = 0; i < sizeof(row) / sizeof(row[0]); i++)
    {
        UINT length;

        base_reset();
        conn_init("example.com", 443, "", 0xAAAA0001UL);
        h_conn.tc_ResumeFlags &= ~TLSR_PERSIST;

        length = nst_build(7200, row[i].declared, row[i].actual);

        CHECK(__wrap__nx_secure_tls_client_handshake(&h_conn.tc_Session, h_nst,
                                                     length, 0) == NX_SUCCESS);

        checks++;
        if ((h_conn.tc_ResumeFlags & TLSR_TICKET_NEW) != 0 ||
            h_conn.tc_TicketLength != 0)
        {
            failures++;
            printf("  FAIL %s was kept\n", row[i].what);
        }

        /* And nothing is cached from it, because there is nothing to cache. */
        tls_resume_record(&h_conn);
        CHECK(tls_resume_count(&h_base) == 1);  /* the session ID, still */
    }
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    /* A real directory, because the disk mirror is what several of these
       tests are about.  TMPDIR first: a build in a sandbox can lack /tmp,
       and a test that cannot write is a test that passes. */
    const char *tmp = getenv("TMPDIR");
    char        dir_template[sizeof(h_dir)];
    int         want;

    /* 64 bytes used to truncate the XXXXXX off under a macOS TMPDIR
       (/var/folders/... is 49 characters), and mkdtemp then fails on the
       template rather than the filesystem. */
    want = snprintf(dir_template, sizeof(dir_template),
                    "%s/anx-tls-resume-XXXXXX",
                    (tmp != NULL && tmp[0] != '\0') ? tmp : "/tmp");
    if (want < 0 || (size_t)want >= sizeof(dir_template))
    {
        printf("TMPDIR is too long for the template\n");
        return 1;
    }

    if (mkdtemp(dir_template) == NULL)
    {
        printf("cannot make a temporary directory\n");
        return 1;
    }
    snprintf(h_dir, sizeof(h_dir), "%s", dir_template);
    snprintf(h_path_a, sizeof(h_path_a), "%s/a.sessions", h_dir);
    snprintf(h_path_b, sizeof(h_path_b), "%s/b.sessions", h_dir);

    fixtures_init();
    CHECK(tls_runtime_open() == TRUE);

    test_mirror_round_trip();
    test_ats2_file_is_ignored();
    test_truncated_file();
    test_decode_rejects();
    test_a_different_file_is_a_different_cache();
    test_empty_path_writes_nothing();

    test_no_ems_is_not_cached();
    test_no_ems_session_id_only();

    test_serverhello_resumes();
    test_serverhello_without_ems_is_refused();
    test_serverhello_changing_ciphersuite();
    test_serverhello_declines();
    test_serverhello_empty_echo();

    test_trust_key_discriminates();
    test_no_host_name();

    test_expired_entry_is_wiped();
    test_lru_eviction();
    test_restore_reuses_the_slot();
    test_evict();

    test_clienthello_asks_for_a_ticket();
    test_clienthello_offers_the_session();
    test_clienthello_ticket_over_the_ceiling();
    test_clienthello_cache_ceiling_leaves_nothing();
    test_clienthello_no_room_at_all();
    test_clienthello_unfamiliar_layout();

    test_new_session_ticket();
    test_new_session_ticket_bounds();

    /* Nothing must be left holding tb_Lock, and nothing must be left
       allocated.  The base is expunged on a machine that will not notice. */
    CHECK(h_sem_nest == 0);
    CHECK(h_sem_max > 0);
    base_reset();
    CHECK(h_allocs == 0);
    CHECK(h_opens_failed > 0);      /* the first load, before any save */

    unlink(h_path_a);
    unlink(h_path_b);
    rmdir(h_dir);

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
