/*
 * curl's vtls backend over AmiNetXDuo's tls.library.
 *
 * WHERE THIS FILE LIVES AND WHY
 *
 *   It is written here, in clients/curl/, and copied into
 *   third_party/curl/lib/vtls/amitls.c by clients/curl/build.sh -t.  The
 *   submodule stays pinned and unmodified in git; what makes curl aware of
 *   this backend is clients/curl/curl-amitls.patch, six hunks that add a
 *   CURLSSLBACKEND_* value, a CMake switch and three table entries.  Keeping
 *   the 600-odd lines of actual backend OUT of that patch is the whole point:
 *   a patch is something to rebase on the next pinned tag, and a rebase should
 *   be reading six hunks, not a source file.
 *
 * WHAT IT TALKS TO
 *
 *   tls.library's eight published vectors (include/aminetxduo/tlslib.h) and
 *   nothing else of ours.  It is an ordinary consumer of that library, exactly
 *   as src/tools/fetch.c is, and it does not link the stack.  The socket it
 *   hands to TLSOpen() is the one curl's cf-socket filter already connected;
 *   TLSClose() gives it back and curl closes it as it always did.
 *
 * THE THREE THINGS THAT ARE NOT LIKE OTHER BACKENDS
 *
 *   1. THE HANDSHAKE BLOCKS.  curl's `do_connect` is meant to be called
 *      repeatedly until it says *done; TLSOpen() is one call that returns
 *      when the handshake is over.  On a 14 MHz 68020 that is 7 seconds for a
 *      two-certificate RSA chain and 23 for an ECDSA one, and for all of it
 *      curl's event loop is stopped.  Nothing else in the process is doing
 *      anything -- the command line tool drives one transfer -- so what is
 *      actually lost is the progress meter, --max-time and Ctrl-C during the
 *      handshake.  TLSA_Timeout is set from curl's own remaining budget so a
 *      dead peer is still bounded.  Making it non-blocking is a state-machine
 *      handshake inside tls.library, which is a bigger piece of work than this
 *      whole file; see docs/RESEARCH.md 11.8.
 *
 *      Note what this does NOT need: curl has set the socket non-blocking with
 *      IoctlSocket(FIONBIO) by the time we are called, and it does not matter.
 *      tls.library does not read the descriptor through bsdsocket.library's
 *      recv() at all -- it borrows the NX_TCP_SOCKET behind it and blocks in
 *      NetX Duo with its own wait_option.  FIONBIO is a flag in bsdsocket's
 *      per-socket state (src/bsdsocket/options.c) that nothing on this path
 *      reads.  So there is no flipping back and forth to do, and no window in
 *      which the descriptor is in the wrong mode.
 *
 *   2. RECEIVING NEEDS THREE QUESTIONS ANSWERED, NOT ONE.  The obvious design
 *      -- poll the socket with a zero timeout, return CURLE_AGAIN when it is
 *      not readable -- DEADLOCKS, and it is worth writing down why so nobody
 *      re-derives it.  nx_secure keeps undecrypted bytes of its own when a TCP
 *      segment carried more than one TLS record, which is the ordinary case
 *      for a server that writes headers and body separately.  In that state
 *      the socket is NOT readable, TLSPending() is 0 because no plaintext has
 *      been produced yet, and a whole record is nevertheless sitting there
 *      ready to decrypt.  A backend that answered CURLE_AGAIN would wait on a
 *      descriptor that will never become readable again.
 *
 *      TLSBuffered() is the vector that answers that half, and amitls_recv()
 *      asks all three: plaintext ready (TLSPending), ciphertext held
 *      (TLSBuffered), bytes on the socket (Curl_socket_check with a zero
 *      timeout).  Only when all three say no does it answer CURLE_AGAIN, and
 *      then curl waits on the descriptor, which is now genuinely the only
 *      place data can come from.  So `--max-time` fires during a transfer, the
 *      progress meter moves and Ctrl-C is read; the handshake is the one place
 *      that still stops the world.
 *
 *      What is NOT removed is a bounded block inside TLSRead() when what is
 *      buffered turns out to be half a record -- the rest is already on its
 *      way and TLSA_Timeout is the ceiling.  tls.library's own documentation
 *      says the same thing about TLSWaitSelect().
 *
 *   3. NO ALPN, SO HTTP/1.1.  tls.library sends no ALPN extension, so
 *      Curl_alpn_set_negotiated() is called with nothing and curl falls back
 *      to HTTP/1.1.  nghttp2 is not built for m68k either, so nothing is lost
 *      today.
 *
 * SESSION RESUMPTION
 *
 *   Nothing here does it and nothing here has to: tls.library resumes by
 *   itself, keyed on TLSA_HostName, with the cache in the library and mirrored
 *   to DEVS:Internet/tlssessions.  There is no tag to pass and no blob to
 *   carry, so curl's own lib/vtls/vtls_scache.c stays unused -- which is the
 *   right answer for a machine where a second program benefits from the first
 *   one's handshake and a per-process cache would not.  All this file does is
 *   report which kind of handshake it got, from TLSInfo()'s ti_Resumed.
 *
 *   TLSA_NoResume exists and is deliberately NOT wired to a curl option.
 *   curl has no switch that means it -- --no-sessionid turns off CURL'S cache,
 *   not the library's -- and inventing one would be a curl patch for something
 *   nobody asked for.  A program that wants every connection to have forward
 *   secrecy should not be using this build.
 *
 * SPDX-License-Identifier: MIT
 */

#include "curl_setup.h"

#ifdef USE_AMITLS

#include <proto/exec.h>
#include <utility/tagitem.h>

#include "aminetxduo/tlslib.h"

#include "urldata.h"
#include "cfilters.h"
#include "connect.h"
#include "select.h"
#include "curl_trc.h"
#include "vtls/vtls.h"
#include "vtls/vtls_int.h"
#include "vtls/amitls.h"


/*
 * bsdsocket.library, opened by curl itself in lib/amigaos.c's OS3 branch
 * before any transfer starts.  Declared by <proto/bsdsocket.h>, which
 * curl_setup.h has already included for this platform.  Borrowing it rather
 * than opening a second one matters: bsdsocket.library is what starts and
 * stops the stack, and a second opener would keep it up past curl's exit.
 */

/* A handshake that is not otherwise bounded.  Two minutes is tls.library's own
   default and is far past the point where a public server has given up. */
#define AMITLS_DEFAULT_TIMEOUT_MS   120000L

/*
 * 16 KB, against tls.library's 10 KB default.  This is the buffer a whole
 * certificate flight has to fit in, and a four-deep chain from a CDN does not
 * always fit in ten.  16384 is the largest a TLS record may be, so nothing
 * larger can help and nothing smaller is safe.  Memory is not the constraint
 * it is for a resident library here: this is a command that exits.
 */
#define AMITLS_RECORD_BUFFER        16384L

/* tls.library's own ceiling (TLS_MAX_CHAIN).  Asking for the maximum costs
   nothing until a server actually sends that many certificates, and a refusal
   for "your chain is longer than I was told to expect" is the least useful
   failure available. */
#define AMITLS_MAX_CHAIN            8L

struct amitls_ssl_backend_data {
  struct TLSConnection *conn;
  curl_socket_t sock;
};

/*
 * One library base for the process, opened on the first handshake rather than
 * at curl_global_init().  A curl that only ever fetches http:// must not need
 * LIBS:tls.library to be present, and must not start the TLS machinery -- the
 * first TLSOpen() is what builds the private P-256 curve that routes through
 * src/crypto68k, and it is not free.
 */
static struct Library *amitls_base;

/*
 * TLSRandom() and TLSBuffered() are the last two entries in tls.library's
 * vector table and they arrived after version 1.0, which was NOT bumped for
 * them. So OpenLibrary("tls.library", 1) happily returns a library that does
 * not have them, and calling one would jump past the (APTR)-1 terminator that
 * MakeLibrary() stopped at -- on a machine with no memory protection, into
 * whatever is there.
 *
 * lib_NegSize is how long the jump table actually is, and comparing it against
 * the LVO is the whole check. src/tlslib/tls_netx.c does exactly this against
 * bsdsocket.library's private vector, for exactly this reason.
 */
static bool amitls_has_v2_vectors;

static struct Library *amitls_open_library(struct Curl_easy *data)
{
  if(!amitls_base) {
    amitls_base = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME,
                              (unsigned long)TLS_LIB_VERSION);
    if(!amitls_base) {
      failf(data, "https:// needs LIBS:tls.library version %d, and there is "
            "none. It ships with the bsdsocket.library this curl talks to; "
            "the two are a pair.", TLS_LIB_VERSION);
      return NULL;
    }

    amitls_has_v2_vectors =
      (amitls_base->lib_NegSize >= (unsigned short)(-TLS_LVO_TLSBuffered));

    if(!amitls_has_v2_vectors)
      infof(data, "this tls.library predates TLSRandom()/TLSBuffered(); "
            "reads will block and Curl_rand() falls back to a weak source");
  }
  return amitls_base;
}

/* TLS_ERR_* is a user-facing vocabulary and CURLcode is curl's. The mapping is
   a table because it is a mapping, and because the compiler can then tell us
   when tls.library grows a code nobody translated. */
static CURLcode amitls_map_error(LONG why)
{
  static const struct {
    LONG    why;
    CURLcode result;
  } map[] = {
    { TLS_ERR_NOMEM,      CURLE_OUT_OF_MEMORY },
    { TLS_ERR_BADSOCKET,  CURLE_SSL_CONNECT_ERROR },
    { TLS_ERR_NOSTACK,    CURLE_SSL_CONNECT_ERROR },
    { TLS_ERR_TRUSTSTORE, CURLE_SSL_CACERT_BADFILE },
    { TLS_ERR_HANDSHAKE,  CURLE_SSL_CONNECT_ERROR },
    { TLS_ERR_UNTRUSTED,  CURLE_PEER_FAILED_VERIFICATION },
    { TLS_ERR_HOSTNAME,   CURLE_PEER_FAILED_VERIFICATION },
    { TLS_ERR_EXPIRED,    CURLE_PEER_FAILED_VERIFICATION },
    { TLS_ERR_TIMEOUT,    CURLE_OPERATION_TIMEDOUT },
    { TLS_ERR_CLOSED,     CURLE_SSL_CONNECT_ERROR },
    { TLS_ERR_IO,         CURLE_SSL_CONNECT_ERROR },
    { TLS_ERR_NOHOSTNAME, CURLE_PEER_FAILED_VERIFICATION },
    { TLS_ERR_INTERNAL,   CURLE_SSL_CONNECT_ERROR }
  };
  size_t i;

  for(i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if(map[i].why == why)
      return map[i].result;

  return CURLE_SSL_CONNECT_ERROR;
}

/* What is left of curl's own budget, in milliseconds, as a TLSA_Timeout.
   Zero from Curl_timeleft_ms() means "no limit was asked for", which is not
   the same as "wait forever" on a machine where a stalled handshake is
   otherwise unkillable. */
static unsigned long amitls_timeout_ms(struct Curl_easy *data)
{
  timediff_t left = Curl_timeleft_ms(data);

  if(left <= 0)
    return (unsigned long)AMITLS_DEFAULT_TIMEOUT_MS;
  if(left > AMITLS_DEFAULT_TIMEOUT_MS)
    return (unsigned long)AMITLS_DEFAULT_TIMEOUT_MS;
  return (unsigned long)left;
}

/*
 * The IANA suite number, spelled the way a person reading -v would want it.
 *
 * This is exactly the set in src/tls/ami_tls_crypto.c's ciphersuite table and
 * deliberately no more: naming a suite tls.library cannot negotiate would be
 * inventing an answer, and anything unlisted prints as its number. If that
 * table grows an entry, this one is where it shows up.
 */
static const char *amitls_suite_name(unsigned long suite)
{
  switch(suite) {
  case 0x1301UL: return "TLS_AES_128_GCM_SHA256";
  case 0x1304UL: return "TLS_AES_128_CCM_SHA256";
  case 0x1305UL: return "TLS_AES_128_CCM_8_SHA256";
  case 0xC02BUL: return "ECDHE-ECDSA-AES128-GCM-SHA256";
  case 0xC02FUL: return "ECDHE-RSA-AES128-GCM-SHA256";
  case 0xC023UL: return "ECDHE-ECDSA-AES128-SHA256";
  case 0xC027UL: return "ECDHE-RSA-AES128-SHA256";
  case 0x009CUL: return "AES128-GCM-SHA256";
  case 0x003DUL: return "AES256-SHA256";
  case 0x003CUL: return "AES128-SHA256";
  default:       return NULL;
  }
}

static const char *amitls_version_name(unsigned long version)
{
  switch(version) {
  case 0x0301UL: return "TLSv1.0";
  case 0x0302UL: return "TLSv1.1";
  case 0x0303UL: return "TLSv1.2";
  case 0x0304UL: return "TLSv1.3";
  default:       return NULL;
  }
}

static void amitls_report(struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;
  struct TLSInfo info;
  const char *suite;
  const char *version;

  memset(&info, 0, sizeof(info));
  info.ti_Size = (unsigned long)sizeof(info);

  if(TLSInfo(amitls_base, backend->conn, &info) != 0)
    return;

  suite = amitls_suite_name(info.ti_CipherSuite);
  version = amitls_version_name(info.ti_Version);

  if(suite && version)
    infof(data, "SSL connection using %s / %s", version, suite);
  else
    infof(data, "SSL connection using TLS 0x%04lx / cipher suite 0x%04lx",
          info.ti_Version, info.ti_CipherSuite);

  if(info.ti_Resumed)
    infof(data, "Resumed a cached session: no certificate sent, no signature "
          "verified, handshake %lu.%02lu s",
          info.ti_HandshakeMillis / 1000UL,
          (info.ti_HandshakeMillis % 1000UL) / 10UL);
  else
    infof(data, "Server certificate chain: %lu certificate(s), %s, "
          "handshake %lu.%02lu s",
          info.ti_ChainDepth,
          info.ti_Verified ? "verified" : "NOT VERIFIED",
          info.ti_HandshakeMillis / 1000UL,
          (info.ti_HandshakeMillis % 1000UL) / 10UL);

  if(!info.ti_ExpiryChecked)
    infof(data, "WARNING: the clock is unset, so certificate validity dates "
          "were NOT checked. The chain signature and the host name were.");
}

/* ------------------------------------------------------------- connect --- */

static CURLcode amitls_connect(struct Curl_cfilter *cf,
                               struct Curl_easy *data,
                               bool *done)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;
  struct ssl_primary_config *conn_config = Curl_ssl_cf_get_primary_config(cf);
  struct TagItem tags[8];
  const char *hostname;
  curl_socket_t sockfd;
  LONG why = TLS_OK;
  int n = 0;

  DEBUGASSERT(backend);
  *done = FALSE;

  if(backend->conn) {
    /* Already shaken hands. Nothing here is re-entrant because TLSOpen() is
       not resumable; if we are called again it is because curl deferred the
       connection, and the answer is the same one. */
    connssl->state = ssl_connection_complete;
    *done = TRUE;
    return CURLE_OK;
  }

  if(!amitls_open_library(data))
    return CURLE_SSL_CONNECT_ERROR;

  if(!SocketBase) {
    failf(data, "bsdsocket.library is not open");
    return CURLE_SSL_CONNECT_ERROR;
  }

  sockfd = Curl_conn_cf_get_socket(cf->next, data);
  if(sockfd == CURL_SOCKET_BAD) {
    failf(data, "no socket to start TLS on");
    return CURLE_SSL_CONNECT_ERROR;
  }
  backend->sock = sockfd;

  /*
   * tls.library checks the chain and the host name together or not at all --
   * TLSA_NoVerify turns off both, in those words, because a verified chain
   * belonging to somebody else proves nothing. curl has two switches, so say
   * plainly what each combination gets rather than quietly doing something
   * else.
   */
  if(!conn_config->verifypeer && conn_config->verifyhost) {
    failf(data, "tls.library cannot verify the host name without verifying "
          "the chain it came from. Use -k/--insecure to turn off both, or "
          "leave both on.");
    return CURLE_SSL_CONNECT_ERROR;
  }
  if(conn_config->verifypeer && !conn_config->verifyhost)
    infof(data, "tls.library always checks the host name when it verifies a "
          "chain; --no-check-certificate-name has no effect here.");

  hostname = connssl->peer.sni ? connssl->peer.sni :
             (connssl->peer.peer ? connssl->peer.peer->hostname : NULL);

  if(conn_config->verifypeer && !hostname) {
    failf(data, "cannot verify a certificate against an IP address; "
          "connect by name, or use -k/--insecure");
    return CURLE_PEER_FAILED_VERIFICATION;
  }

  if(hostname) {
    tags[n].ti_Tag = TLSA_HostName;
    tags[n++].ti_Data = (unsigned long)hostname;
  }
  if(!conn_config->verifypeer) {
    tags[n].ti_Tag = TLSA_NoVerify;
    tags[n++].ti_Data = (unsigned long)TRUE;
  }
  if(conn_config->CAfile) {
    /* CURLOPT_CAINFO. The default is tls.library's own
       DEVS:Internet/certificates, which is where the machine keeps its trust
       store; --cacert points at another file in the same format. */
    tags[n].ti_Tag = TLSA_TrustStore;
    tags[n++].ti_Data = (unsigned long)conn_config->CAfile;
  }
  tags[n].ti_Tag = TLSA_Timeout;
  tags[n++].ti_Data = amitls_timeout_ms(data);
  tags[n].ti_Tag = TLSA_RecordBuffer;
  tags[n++].ti_Data = (unsigned long)AMITLS_RECORD_BUFFER;
  tags[n].ti_Tag = TLSA_MaxChain;
  tags[n++].ti_Data = (unsigned long)AMITLS_MAX_CHAIN;
  tags[n].ti_Tag = TLSA_Error;
  tags[n++].ti_Data = (unsigned long)&why;
  tags[n].ti_Tag = TAG_END;
  tags[n].ti_Data = 0;
  DEBUGASSERT((size_t)n < sizeof(tags) / sizeof(tags[0]));

  CURL_TRC_CF(data, cf, "TLSOpen(%s), blocking", hostname ? hostname : "-");

  backend->conn = TLSOpenA(amitls_base, (APTR)SocketBase, (LONG)sockfd, tags);

  if(!backend->conn) {
    const char *text = (const char *)TLSErrorString(amitls_base, why);

    failf(data, "%s: %s", hostname ? hostname : "TLS", text ? text : "failed");

    if(why == TLS_ERR_TRUSTSTORE)
      failf(data, "DEVS:Internet/certificates is the list of certificate "
            "authorities this machine trusts. Install it, or build one with "
            "tools/mkcertstore.py, or point --cacert at one.");

    return amitls_map_error(why);
  }

  amitls_report(cf, data);

  /* No ALPN extension is sent, so nothing was negotiated and curl uses
     HTTP/1.1. Saying so through the normal path keeps -v honest. */
  (void)Curl_alpn_set_negotiated(cf, data, connssl, NULL, 0);

  connssl->state = ssl_connection_complete;
  *done = TRUE;
  return CURLE_OK;
}

/* ------------------------------------------------------------ transfer --- */

/*
 * Can TLSRead() make progress without another byte arriving from the network?
 *
 * Three places data can be, and a reader that checks fewer than all three
 * either hangs or blocks when it did not have to:
 *
 *   plaintext already decrypted     TLSPending()
 *   ciphertext held by the library  TLSBuffered()
 *   bytes on the socket             the descriptor being readable
 *
 * The middle one is the one that is easy to miss and the one that hangs; see
 * the note at the top of this file.
 */
static bool amitls_can_read(struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;

  if(TLSPending(amitls_base, backend->conn) > 0)
    return TRUE;

  if(!amitls_has_v2_vectors)
    return TRUE;              /* no way to ask: read and let it block */

  if(TLSBuffered(amitls_base, backend->conn) > 0)
    return TRUE;

  if(backend->sock == CURL_SOCKET_BAD)
    return TRUE;

  (void)data;
  return SOCKET_READABLE(backend->sock, 0) > 0;
}

static CURLcode amitls_recv(struct Curl_cfilter *cf, struct Curl_easy *data,
                            char *buf, size_t len, size_t *pnread)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;
  LONG nread;

  DEBUGASSERT(backend);
  *pnread = 0;
  connssl->io_need = CURL_SSL_IO_NEED_NONE;

  if(!backend->conn)
    return CURLE_RECV_ERROR;

  if(len > 0x7FFFFFFFUL)
    len = 0x7FFFFFFFUL;

  if(!amitls_can_read(cf, data)) {
    connssl->io_need = CURL_SSL_IO_NEED_RECV;
    return CURLE_AGAIN;
  }

  /* May still block briefly if what is buffered is half a record. The rest is
     already on its way and TLSA_Timeout is the ceiling. */
  nread = TLSRead(amitls_base, backend->conn, (APTR)buf, (LONG)len);

  if(nread > 0) {
    *pnread = (size_t)nread;
    return CURLE_OK;
  }

  if(nread == 0) {
    /* close_notify, or the peer simply gone. Either is end of stream. */
    connssl->peer_closed = TRUE;
    return CURLE_OK;
  }

  failf(data, "TLS read failed");
  return CURLE_RECV_ERROR;
}

static CURLcode amitls_send(struct Curl_cfilter *cf, struct Curl_easy *data,
                            const void *mem, size_t len, size_t *pnwritten)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;
  LONG nwritten;

  DEBUGASSERT(backend);
  *pnwritten = 0;
  connssl->io_need = CURL_SSL_IO_NEED_NONE;

  if(!backend->conn)
    return CURLE_SEND_ERROR;

  if(len > 0x7FFFFFFFUL)
    len = 0x7FFFFFFFUL;

  /* TLSWrite() records the whole buffer or fails; a short return means it got
     part way and then could not, which curl handles as a partial write. */
  nwritten = TLSWrite(amitls_base, backend->conn, (CONST_APTR)mem, (LONG)len);

  if(nwritten > 0) {
    *pnwritten = (size_t)nwritten;
    return CURLE_OK;
  }

  failf(data, "TLS write failed");
  return CURLE_SEND_ERROR;
}

static bool amitls_data_pending(struct Curl_cfilter *cf,
                                const struct Curl_easy *data)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;

  (void)data;
  DEBUGASSERT(backend);

  if(!backend->conn || !amitls_base)
    return FALSE;

  /* Both kinds of "the answer is already in memory": plaintext the library has
     decrypted, and ciphertext it is holding. Either means curl should call
     recv again rather than wait on a descriptor that will stay quiet. */
  if(TLSPending(amitls_base, backend->conn) > 0)
    return TRUE;

  return amitls_has_v2_vectors &&
         TLSBuffered(amitls_base, backend->conn) > 0;
}

static void amitls_close(struct Curl_cfilter *cf, struct Curl_easy *data)
{
  struct ssl_connect_data *connssl = cf->ctx;
  struct amitls_ssl_backend_data *backend = connssl->backend;

  (void)data;
  DEBUGASSERT(backend);

  if(backend->conn) {
    /* Sends close_notify and gives the descriptor back. curl closes the
       descriptor itself, through cf-socket, exactly as for http://. */
    TLSClose(amitls_base, backend->conn);
    backend->conn = NULL;
  }
  backend->sock = CURL_SOCKET_BAD;
}

/* --------------------------------------------------------------- odds --- */

static int amitls_init(void)
{
  /* Deliberately does not open tls.library: see amitls_open_library(). */
  return 1;
}

static void amitls_cleanup(void)
{
  if(amitls_base) {
    CloseLibrary(amitls_base);
    amitls_base = NULL;
  }
}

static size_t amitls_version(char *buffer, size_t size)
{
  if(amitls_base)
    return (size_t)curl_msnprintf(buffer, size, "tls.library/%u.%u",
                                  amitls_base->lib_Version,
                                  amitls_base->lib_Revision);

  return (size_t)curl_msnprintf(buffer, size, "tls.library/%d.%d",
                                TLS_LIB_VERSION, TLS_LIB_REVISION);
}

/*
 * Curl_rand() lands here whenever libcurl is built with TLS. NOT the TLS
 * handshake's own randomness -- that never leaves the library -- but curl's:
 * multipart boundaries, a Digest cnonce, the salt on curl's session cache
 * keys, the jitter in the DNS cache.
 *
 * TLSRandom() is the machine's one entropy pool, the SHA-256 DRBG the session
 * keys come from, and it is what this asks. It answers -1 until a connection
 * has been opened in this program, because the pool lives in
 * bsdsocket.library and tls.library reaches it through the link TLSOpen()
 * makes -- and curl calls Curl_rand() well before its first https:// URL. So
 * there is a fallback and it is a time-seeded LCG.
 *
 * The LCG is not cryptographic and is not pretending to be. What keeps that
 * from mattering is that nothing which needs a secret comes from here: an
 * http://-only run never reaches TLS at all, and once anything has, the real
 * pool is answering. Opening tls.library merely to seed a boundary string
 * would make every http:// fetch depend on a library it does not use.
 */
static CURLcode amitls_random(struct Curl_easy *data,
                              unsigned char *entropy, size_t length)
{
  static unsigned long seed;
  static bool seeded;

  (void)data;

  if(amitls_base && amitls_has_v2_vectors) {
    LONG got = TLSRandom(amitls_base, (APTR)entropy, (LONG)length);

    if(got == (LONG)length)
      return CURLE_OK;
  }

  if(!seeded) {
    struct DateStamp ds;

    DateStamp(&ds);
    seed = (unsigned long)ds.ds_Days * 86400UL * 50UL +
           (unsigned long)ds.ds_Minute * 3000UL +
           (unsigned long)ds.ds_Tick;
    seed ^= (unsigned long)FindTask(NULL);
    seed ^= (unsigned long)&seed;
    seeded = TRUE;
  }

  while(length--) {
    seed = seed * 1103515245UL + 12345UL;
    *entropy++ = (unsigned char)((seed >> 16) & 0xFF);
  }

  return CURLE_OK;
}

static void *amitls_get_internals(struct ssl_connect_data *connssl,
                                  CURLINFO info)
{
  struct amitls_ssl_backend_data *backend = connssl->backend;

  (void)info;
  DEBUGASSERT(backend);
  return backend->conn;
}

const struct Curl_ssl Curl_ssl_amitls = {
  { CURLSSLBACKEND_AMITLS, "amitls" }, /* info */

  0,                                /* supports nothing optional */

  sizeof(struct amitls_ssl_backend_data),

  amitls_init,                      /* init */
  amitls_cleanup,                   /* cleanup */
  amitls_version,                   /* version */
  NULL,                             /* shutdown: TLSClose sends close_notify */
  amitls_data_pending,              /* data_pending */
  amitls_random,                    /* random */
  NULL,                             /* cert_status_request: no OCSP */
  amitls_connect,                   /* connect */
  Curl_ssl_adjust_pollset,          /* adjust_pollset */
  amitls_get_internals,             /* get_internals */
  amitls_close,                     /* close_one */
  NULL,                             /* close_all */
  NULL,                             /* set_engine */
  NULL,                             /* set_engine_default */
  NULL,                             /* engines_list */
  NULL,                             /* sha256sum: no --pinnedpubkey */
  amitls_recv,                      /* recv decrypted data */
  amitls_send,                      /* send data to encrypt */
  NULL,                             /* get_channel_binding */
};

#endif /* USE_AMITLS */
