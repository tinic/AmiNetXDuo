/*
 * The tests for src/tools/httpws.c, RFC 6455 on the wire.
 *
 * Everything here fails silently.  An accept computed over the key without the
 * GUID is 28 characters of base64 and every browser refuses the connection
 * with no diagnostic the server can see.  A mask applied from the wrong offset
 * when a frame is split across two reads produces different bytes rather than
 * an error, so a shell behind it runs a command nobody typed.  A control frame
 * accepted while fragmented, or a 64-bit length whose top word was not
 * checked, is a length the decoder and the client disagree about, and after
 * that the two are reading different frames on the same socket for ever.
 *
 * So the vectors are written down.  The handshake pair is RFC 6455 1.3's own,
 * digit for digit, and the frames are 5.7's.  The rest are the cases the
 * specification says a server must fail, each fed both whole and one byte at a
 * time, because a state machine that is right on a whole buffer and wrong
 * across a split is the failure this decoder exists to avoid.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tools -DNX_CRYPTO_STANDALONE_ENABLE \
 *      -Ithird_party/netxduo/crypto_libraries/inc \
 *      src/tools/test/test_httpws.c src/tools/httpws.c src/tools/httpsha1.c \
 *      third_party/netxduo/crypto_libraries/src/nx_crypto_sha1.c -o test_httpws
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpws.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------ the sink --- */

/*
 * What the decoder said, flattened.  A message's pieces are concatenated and
 * the count of calls is kept, because one message in three pieces and three
 * messages are the difference a fragmentation test is about.
 */
typedef struct
{
    int             calls;
    int             finals;
    HttpWsEvent     last_ev;
    /*
     * Two buffers, for a reason.  RFC 6455 5.4 lets a control frame arrive
     * between the fragments of a data message, so a reader with one
     * accumulator concatenates a ping's payload onto the half-finished message
     * and delivers both as one.  A single buffer here reported "Helhi" and
     * "lo" for a Hello split around a ping, which is what a terminal with one
     * buffer would type into the shell.
     */
    unsigned long   len;
    unsigned char   buf[4096];
    unsigned long   ctl_len;
    unsigned char   ctl[256];
    /* Every completed message, in order, as "<ev>:<text>". */
    char            log[1024];
    unsigned long   log_n;
} Heard;

static int ev_is_control(HttpWsEvent ev)
{
    return (ev == HTTP_WS_EV_CLOSE || ev == HTTP_WS_EV_PING ||
            ev == HTTP_WS_EV_PONG) ? 1 : 0;
}

static Heard heard;

static void log_put(const char *s)
{
    while (*s != '\0' && heard.log_n + 1UL < sizeof(heard.log))
        heard.log[heard.log_n++] = *s++;
    heard.log[heard.log_n] = '\0';
}

static void sink(void *ctx, HttpWsEvent ev, const unsigned char *data,
                 long len, int final)
{
    Heard          *h    = (Heard *)ctx;
    int             ctl  = ev_is_control(ev);
    unsigned char  *buf  = ctl ? h->ctl : h->buf;
    unsigned long   cap  = ctl ? sizeof(h->ctl) : sizeof(h->buf);
    unsigned long  *used = ctl ? &h->ctl_len : &h->len;
    long            i;

    h->calls++;
    h->last_ev = ev;

    for (i = 0; i < len; i++)
    {
        if (*used + 1UL < cap)
            buf[(*used)++] = data[i];
    }

    if (final)
    {
        char          one[2];
        unsigned long j;

        h->finals++;

        one[0] = (char)('0' + (int)ev);
        one[1] = '\0';
        log_put(one);
        log_put(":");

        for (j = 0; j < *used && j < 64UL; j++)
        {
            char c[2];

            c[0] = (char)((buf[j] >= 0x20 && buf[j] < 0x7f) ? buf[j] : '.');
            c[1] = '\0';
            log_put(c);
        }

        log_put(" ");
        *used = 0;
    }
}

static void heard_clear(void)
{
    memset(&heard, 0, sizeof(heard));
}

/*
 * Feed the same bytes two ways, in one call and one byte at a time.  The log
 * both produce has to be identical, and the failure code too.  A decoder is
 * only correct if the split does not matter, and every real split is the
 * network's choice.
 */
static void feed_both(const unsigned char *frame, long len,
                      char *log_whole, unsigned short *fail_whole,
                      char *log_split, unsigned short *fail_split)
{
    HttpWsIn in;
    long     i;

    heard_clear();
    http_ws_reset(&in);
    (void)http_ws_feed(&in, frame, len, sink, &heard);
    strcpy(log_whole, heard.log);
    *fail_whole = in.failed;

    heard_clear();
    http_ws_reset(&in);
    for (i = 0; i < len; i++)
        (void)http_ws_feed(&in, &frame[i], 1, sink, &heard);
    strcpy(log_split, heard.log);
    *fail_split = in.failed;
}

/* One call for both shapes, so no test can check only one of them. */
static void expect(const char *what, const unsigned char *frame, long len,
                   const char *want_log, unsigned short want_fail)
{
    char           lw[1024];
    char           ls[1024];
    unsigned short fw;
    unsigned short fs;

    feed_both(frame, len, lw, &fw, ls, &fs);

    checks++;
    if (strcmp(lw, want_log) != 0 || fw != want_fail)
    {
        failures++;
        printf("  FAIL %s: whole buffer gave \"%s\" fail %u, wanted \"%s\" "
               "fail %u\n", what, lw, (unsigned)fw, want_log,
               (unsigned)want_fail);
    }

    checks++;
    if (strcmp(ls, want_log) != 0 || fs != want_fail)
    {
        failures++;
        printf("  FAIL %s: byte at a time gave \"%s\" fail %u, wanted \"%s\" "
               "fail %u\n", what, ls, (unsigned)fs, want_log,
               (unsigned)want_fail);
    }
}

/* ---------------------------------------------------------- the base64 --- */

static void test_base64(void)
{
    char          text[64];
    unsigned char raw[32];

    printf("base64\n");

    /* RFC 4648 10, so the table and the padding are checked against
       something other than this file's own encoder. */
    CHECK(http_ws_b64_encode((const unsigned char *)"", 0, text,
                             sizeof(text)) == 0 && strcmp(text, "") == 0);
    CHECK(http_ws_b64_encode((const unsigned char *)"f", 1, text,
                             sizeof(text)) == 4 && strcmp(text, "Zg==") == 0);
    CHECK(http_ws_b64_encode((const unsigned char *)"fo", 2, text,
                             sizeof(text)) == 4 && strcmp(text, "Zm8=") == 0);
    CHECK(http_ws_b64_encode((const unsigned char *)"foo", 3, text,
                             sizeof(text)) == 4 && strcmp(text, "Zm9v") == 0);
    CHECK(http_ws_b64_encode((const unsigned char *)"foobar", 6, text,
                             sizeof(text)) == 8 &&
          strcmp(text, "Zm9vYmFy") == 0);

    CHECK(http_ws_b64_decode("Zm9vYmFy", raw, sizeof(raw)) == 6 &&
          memcmp(raw, "foobar", 6) == 0);
    CHECK(http_ws_b64_decode("Zg==", raw, sizeof(raw)) == 1 && raw[0] == 'f');

    /* Not base64, and each for its own reason. */
    CHECK(http_ws_b64_decode("Zm9v*mFy", raw, sizeof(raw)) == -1);
    CHECK(http_ws_b64_decode("Zm9vYmF", raw, sizeof(raw)) == -1);
    CHECK(http_ws_b64_decode("Zg==Zg==", raw, sizeof(raw)) == -1);
    /* Padding bits that are not zero, which is two texts for one value. */
    CHECK(http_ws_b64_decode("Zh==", raw, sizeof(raw)) == -1);
    /* Longer than the caller's buffer is refused, not truncated. */
    CHECK(http_ws_b64_decode("Zm9vYmFy", raw, 3) == -1);

    /* The encoder refuses rather than writing a short answer. */
    CHECK(http_ws_b64_encode((const unsigned char *)"foobar", 6, text, 8) == 0);
}

/* -------------------------------------------------------- the handshake --- */

static void test_handshake(void)
{
    char accept[32];

    printf("the upgrade handshake\n");

    /*
     * RFC 6455 1.3, verbatim.  This pair is why the GUID is in the source.
     * The key alone hashes to something else entirely, and a server that got
     * it wrong would look identical from the outside up to the moment every
     * browser refused it.
     */
    CHECK(http_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", accept,
                         sizeof(accept)) == 1);
    CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    /* RFC 6455 4.2.2 spells the header value with the surrounding space that a
       header parser can leave on. */
    CHECK(http_ws_accept("  dGhlIHNhbXBsZSBub25jZQ==  ", accept,
                         sizeof(accept)) == 1 &&
          strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    /* A key is 16 bytes of base64 and nothing else is one. */
    CHECK(http_ws_accept("", accept, sizeof(accept)) == 0);
    CHECK(http_ws_accept("dGhlIHNhbXBsZSBub25j", accept, sizeof(accept)) == 0);
    CHECK(http_ws_accept("dGhlIHNhbXBsZSBub25jZQ==extra", accept,
                         sizeof(accept)) == 0);
    CHECK(http_ws_accept("dGhlIHNhbXBsZSBub25j!!==", accept,
                         sizeof(accept)) == 0);
    /* 24 unpadded characters decode to eighteen bytes, not sixteen, so a
       length check on the text alone would let this through and a nonce of the
       wrong size would be hashed.  The padded form of the same length is the
       one that is a key. */
    CHECK(http_ws_accept("AAAAAAAAAAAAAAAAAAAAAAAA", accept,
                         sizeof(accept)) == 0);
    CHECK(http_ws_accept("AAAAAAAAAAAAAAAAAAAAAA==", accept,
                         sizeof(accept)) == 1);

    /* A buffer that cannot hold the answer gets no answer. */
    CHECK(http_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", accept, 20) == 0);
}

/* ------------------------------------------------------------ the frames --- */

static void test_frames(void)
{
    printf("frames the client sends\n");

    /* RFC 6455 5.7's masked "Hello", the specification's own bytes. */
    {
        static const unsigned char hello[] = {
            0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
            0x7f, 0x9f, 0x4d, 0x51, 0x58
        };

        expect("a masked text Hello", hello, (long)sizeof(hello),
               "1:Hello ", 0);
    }

    /* And 5.7's fragmented "Hel" + "lo", which is the same message in two
       frames and must read as one. */
    {
        static const unsigned char frag[] = {
            0x01, 0x83, 0x00, 0x00, 0x00, 0x00, 'H', 'e', 'l',
            0x80, 0x82, 0x00, 0x00, 0x00, 0x00, 'l', 'o'
        };

        expect("a fragmented message is one message", frag, (long)sizeof(frag),
               "1:Hello ", 0);
    }

    /* A control frame between the fragments, which RFC 6455 5.4 allows and
       which is the interleaving a keep-alive ping produces. */
    {
        static const unsigned char mixed[] = {
            0x01, 0x83, 0x00, 0x00, 0x00, 0x00, 'H', 'e', 'l',
            0x89, 0x82, 0x00, 0x00, 0x00, 0x00, 'h', 'i',
            0x80, 0x82, 0x00, 0x00, 0x00, 0x00, 'l', 'o'
        };

        expect("a ping between fragments interrupts nothing", mixed,
               (long)sizeof(mixed), "4:hi 1:Hello ", 0);
    }

    /* Text is UTF-8 over the whole message, not independently per frame.  A
       Euro sign split after its lead byte is valid and exercises both the
       fragmented-message state and the one-byte-at-a-time feed. */
    {
        static const unsigned char utf8_split[] = {
            0x01, 0x81, 0, 0, 0, 0, 0xe2,
            0x80, 0x82, 0, 0, 0, 0, 0x82, 0xac
        };

        expect("UTF-8 may cross a fragmented-frame boundary", utf8_split,
               (long)sizeof(utf8_split), "1:... ", 0);
    }

    /* An empty ping, whose whole frame is its header.  Nothing follows it, so
       a decoder that only delivered from the payload loop would hold it for
       ever. */
    {
        static const unsigned char ping[] = {
            0x89, 0x80, 0x01, 0x02, 0x03, 0x04
        };

        expect("an empty ping is delivered on its header alone", ping,
               (long)sizeof(ping), "4: ", 0);
    }

    /* A close with a code and a reason, and an empty one. */
    {
        static const unsigned char bye[] = {
            0x88, 0x87, 0x00, 0x00, 0x00, 0x00,
            0x03, 0xe8, 'd', 'o', 'n', 'e', '!'
        };

        expect("a close carries its code", bye, (long)sizeof(bye),
               "3:..done! ", 0);
    }
    {
        static const unsigned char bye0[] = { 0x88, 0x80, 9, 9, 9, 9 };

        expect("an empty close is a close", bye0, (long)sizeof(bye0),
               "3: ", 0);
    }

    /* RFC 6455 5.5.1: receiving Close ends input.  A client can concatenate
       another frame in the same TCP segment, but none of that later frame may
       be delivered.  For the terminal, the binary payload would otherwise be
       a command run after the client had closed the session. */
    {
        static const unsigned char after_close[] = {
            0x88, 0x80, 0, 0, 0, 0,
            0x82, 0x85, 0, 0, 0, 0, 'E', 'x', 'i', 't', '\n'
        };

        expect("data after a close is discarded", after_close,
               (long)sizeof(after_close), "3: ", 0);
    }

    /* 126 bytes, so the two-byte extended length is exercised.  The payload
       is 'x' repeated, masked with a mask that is not zero, which is what
       catches an offset that resets at a buffer boundary. */
    {
        static const unsigned char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
        unsigned char frame[8 + 126];
        char          want[80];
        long          i;

        frame[0] = 0x82;
        frame[1] = (unsigned char)(0x80 | 126);
        frame[2] = 0x00;
        frame[3] = 126;
        for (i = 0; i < 4; i++)
            frame[4 + i] = mask[i];
        for (i = 0; i < 126; i++)
            frame[8 + i] = (unsigned char)('x' ^ mask[i & 3]);

        /* The log truncates a message at 64 characters. */
        want[0] = '2';
        want[1] = ':';
        for (i = 0; i < 64; i++)
            want[2 + i] = 'x';
        want[66] = ' ';
        want[67] = '\0';

        expect("a 126-byte payload and its extended length", frame,
               8 + 126, want, 0);
    }

    printf("frames the client may not send\n");

    /* RFC 6455 5.1: unmasked, from a client, is 1002 and the end of the
       connection. */
    {
        static const unsigned char bare[] = {
            0x81, 0x05, 'H', 'e', 'l', 'l', 'o'
        };

        expect("an unmasked frame is refused", bare, (long)sizeof(bare), "",
               HTTP_WS_CLOSE_PROTOCOL);
    }

    /* RFC 6455 5.2: a reserved bit describes a transformation nothing here
       negotiated. */
    {
        static const unsigned char rsv[] = {
            0xc1, 0x80, 0, 0, 0, 0
        };

        expect("a reserved bit is refused", rsv, (long)sizeof(rsv), "",
               HTTP_WS_CLOSE_PROTOCOL);
    }

    /* An opcode that is not one of the six. */
    {
        static const unsigned char op[] = { 0x83, 0x80, 0, 0, 0, 0 };

        expect("an unknown opcode is refused", op, (long)sizeof(op), "",
               HTTP_WS_CLOSE_PROTOCOL);
    }

    /* RFC 6455 5.5: a control frame is never fragmented... */
    {
        static const unsigned char split_ping[] = { 0x09, 0x80, 0, 0, 0, 0 };

        expect("a fragmented control frame is refused", split_ping,
               (long)sizeof(split_ping), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* ...and never longer than 125 bytes. */
    {
        static const unsigned char fat_ping[] = {
            0x89, (unsigned char)(0x80 | 126), 0x00, 0x7e, 0, 0, 0, 0
        };

        expect("an over-long control frame is refused", fat_ping,
               (long)sizeof(fat_ping), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* RFC 6455 5.5.1: one byte is not a close code. */
    {
        static const unsigned char stub[] = {
            0x88, 0x81, 0, 0, 0, 0, 0x03
        };

        expect("a one-byte close payload is refused", stub,
               (long)sizeof(stub), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* RFC 6455 7.4: these values cannot appear on the wire.  In particular,
       echoing 1005 back would make this endpoint send an invalid frame too. */
    {
        static const unsigned char low[] = {
            0x88, 0x82, 0, 0, 0, 0, 0x03, 0xe7
        };
        static const unsigned char pseudo[] = {
            0x88, 0x82, 0, 0, 0, 0, 0x03, 0xed
        };
        static const unsigned char high[] = {
            0x88, 0x82, 0, 0, 0, 0, 0x13, 0x88
        };

        expect("a close code below 1000 is refused", low,
               (long)sizeof(low), "", HTTP_WS_CLOSE_PROTOCOL);
        expect("a pseudo close code is refused", pseudo,
               (long)sizeof(pseudo), "", HTTP_WS_CLOSE_PROTOCOL);
        expect("a close code at 5000 is refused", high,
               (long)sizeof(high), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* RFC 6455 5.5.1: the bytes after the status are UTF-8.  Both are forms a
       byte-copying decoder accepts: an overlong slash and a truncated Euro
       sign. */
    {
        static const unsigned char overlong[] = {
            0x88, 0x84, 0, 0, 0, 0, 0x03, 0xe8, 0xc0, 0xaf
        };
        static const unsigned char truncated[] = {
            0x88, 0x84, 0, 0, 0, 0, 0x03, 0xe8, 0xe2, 0x82
        };

        expect("an overlong close reason is refused", overlong,
               (long)sizeof(overlong), "", HTTP_WS_CLOSE_DATA);
        expect("a truncated close reason is refused", truncated,
               (long)sizeof(truncated), "", HTTP_WS_CLOSE_DATA);
    }

    /* RFC 6455 8.1 applies the same rule to ordinary text messages.  The
       incomplete case ends in an empty final continuation so the decoder has
       to check message completion even when that frame has no payload. */
    {
        static const unsigned char overlong_text[] = {
            0x81, 0x82, 0, 0, 0, 0, 0xc0, 0xaf
        };
        static const unsigned char incomplete_text[] = {
            0x01, 0x81, 0, 0, 0, 0, 0xe2,
            0x80, 0x80, 0, 0, 0, 0
        };

        expect("overlong UTF-8 in text is refused", overlong_text,
               (long)sizeof(overlong_text), "", HTTP_WS_CLOSE_DATA);
        expect("unfinished UTF-8 at the message end is refused",
               incomplete_text, (long)sizeof(incomplete_text), "",
               HTTP_WS_CLOSE_DATA);
    }

    /* A continuation with nothing to continue. */
    {
        static const unsigned char orphan[] = {
            0x80, 0x81, 0, 0, 0, 0, 'x'
        };

        expect("a continuation of nothing is refused", orphan,
               (long)sizeof(orphan), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* A second data frame while the first message is still open. */
    {
        static const unsigned char both[] = {
            0x01, 0x81, 0, 0, 0, 0, 'a',
            0x81, 0x81, 0, 0, 0, 0, 'b'
        };

        expect("a data frame inside a fragmented message is refused", both,
               (long)sizeof(both), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* A 64-bit length whose top word is set is not a length this machine can
       hold, and the answer is 1009 rather than 1002, because the frame is well
       formed. */
    {
        static const unsigned char huge[] = {
            0x82, (unsigned char)(0x80 | 127),
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
            0, 0, 0, 0
        };

        expect("a length past 32 bits is too big, not malformed", huge,
               (long)sizeof(huge), "", HTTP_WS_CLOSE_TOOBIG);
    }

    /* RFC 6455 5.2: the shortest possible length encoding is mandatory.  No
       payload is needed here because both failures are known from the header. */
    {
        static const unsigned char short16[] = {
            0x82, (unsigned char)(0x80 | 126), 0x00, 0x7d, 0, 0, 0, 0
        };
        static const unsigned char short64[] = {
            0x82, (unsigned char)(0x80 | 127),
            0, 0, 0, 0, 0, 0, 0xff, 0xff,
            0, 0, 0, 0
        };

        expect("a non-minimal 16-bit length is refused", short16,
               (long)sizeof(short16), "", HTTP_WS_CLOSE_PROTOCOL);
        expect("a non-minimal 64-bit length is refused", short64,
               (long)sizeof(short64), "", HTTP_WS_CLOSE_PROTOCOL);
    }

    /* And one inside 32 bits but past what this server will assemble. */
    {
        static const unsigned char big[] = {
            0x82, (unsigned char)(0x80 | 127),
            0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01,
            0, 0, 0, 0
        };

        expect("a message past the ceiling is 1009", big, (long)sizeof(big),
               "", HTTP_WS_CLOSE_TOOBIG);
    }

    /* The same ceiling spans every fragment.  Each frame here is only 32769
       bytes, but the second header takes their message two bytes over 64 KiB;
       rejection is possible before any payload for that continuation arrives. */
    {
        static unsigned char fragmented_big[8 + 32769 + 8];
        long i;
        long at = 0;

        fragmented_big[at++] = 0x02; /* non-final binary */
        fragmented_big[at++] = (unsigned char)(0x80 | 126);
        fragmented_big[at++] = 0x80;
        fragmented_big[at++] = 0x01;
        fragmented_big[at++] = 0;
        fragmented_big[at++] = 0;
        fragmented_big[at++] = 0;
        fragmented_big[at++] = 0;
        for (i = 0; i < 32769; i++)
            fragmented_big[at++] = 0;

        fragmented_big[at++] = 0x80; /* final continuation */
        fragmented_big[at++] = (unsigned char)(0x80 | 126);
        fragmented_big[at++] = 0x80;
        fragmented_big[at++] = 0x01;
        fragmented_big[at++] = 0;
        fragmented_big[at++] = 0;
        fragmented_big[at++] = 0;
        fragmented_big[at++] = 0;

        expect("fragmentation cannot reset the message ceiling",
               fragmented_big, at, "", HTTP_WS_CLOSE_TOOBIG);
    }
}

/* ---------------------------------------------------------- the encoder --- */

static void test_encoder(void)
{
    unsigned char out[64];
    unsigned long n;

    printf("frames this server sends\n");

    /* A server frame is never masked, so the mask bit is clear and the header
       is two bytes for anything under 126. */
    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_BINARY, 5, 1);
    CHECK(n == 2 && out[0] == 0x82 && out[1] == 0x05);

    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_TEXT, 0, 1);
    CHECK(n == 2 && out[0] == 0x81 && out[1] == 0x00);

    /* Not final, which is the same opcode without the FIN bit. */
    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_BINARY, 5, 0);
    CHECK(n == 2 && out[0] == 0x02);

    /* 125 is the last two-byte header, and 126 the first four-byte one. */
    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_BINARY, 125, 1);
    CHECK(n == 2 && out[1] == 125);

    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_BINARY, 126, 1);
    CHECK(n == 4 && out[1] == 126 && out[2] == 0x00 && out[3] == 126);

    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_BINARY, 65535, 1);
    CHECK(n == 4 && out[1] == 126 && out[2] == 0xff && out[3] == 0xff);

    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_BINARY, 65536, 1);
    CHECK(n == 10 && out[1] == 127 && out[2] == 0 && out[6] == 0x00 &&
          out[7] == 0x01 && out[8] == 0x00 && out[9] == 0x00);

    n = http_ws_head(out, sizeof(out), HTTP_WS_EV_PONG, 3, 1);
    CHECK(n == 2 && out[0] == 0x8a);

    /* A buffer too small gets nothing, not a truncated header. */
    CHECK(http_ws_head(out, 1, HTTP_WS_EV_BINARY, 5, 1) == 0);
    CHECK(http_ws_head(out, 3, HTTP_WS_EV_BINARY, 200, 1) == 0);

    /* A close frame carries its code big-endian, ahead of the reason. */
    n = http_ws_close_frame(out, sizeof(out), HTTP_WS_CLOSE_PROTOCOL, "no");
    CHECK(n == 2 + 4 && out[0] == 0x88 && out[1] == 4 &&
          out[2] == 0x03 && out[3] == 0xea && out[4] == 'n' && out[5] == 'o');

    n = http_ws_close_frame(out, sizeof(out), HTTP_WS_CLOSE_NORMAL, 0);
    CHECK(n == 4 && out[1] == 2 && out[2] == 0x03 && out[3] == 0xe8);

    /*
     * What this server sends must be what it can read back, minus the mask
     * that only a client applies.  An encoder and a decoder that agree with
     * each other and not with the specification is the failure a single-sided
     * test cannot see, so every case above is the RFC's bytes and this one is
     * the round trip.
     */
    {
        HttpWsIn      in;
        unsigned char frame[16];
        unsigned long head;
        long          i;

        head = http_ws_head(frame, sizeof(frame), HTTP_WS_EV_BINARY, 5, 1);
        for (i = 0; i < 5; i++)
            frame[head + (unsigned long)i] = (unsigned char)("Hello"[i]);

        /* The decoder is a server's, so it refuses what it just wrote.  A
           server frame is unmasked and only a client's can be read. */
        heard_clear();
        http_ws_reset(&in);
        (void)http_ws_feed(&in, frame, (long)(head + 5), sink, &heard);
        CHECK(in.failed == HTTP_WS_CLOSE_PROTOCOL);
        CHECK(heard.finals == 0);
    }
}

/* ---------------------------------------------------------------- close --- */

static void test_reasons(void)
{
    printf("what a close code is called\n");

    CHECK(http_ws_close_reason(HTTP_WS_CLOSE_PROTOCOL) != 0);
    CHECK(http_ws_close_reason(HTTP_WS_CLOSE_DATA) != 0);
    CHECK(http_ws_close_reason(HTTP_WS_CLOSE_TOOBIG) != 0);
    CHECK(http_ws_close_reason(4999) != 0);
}

int main(void)
{
    test_base64();
    test_handshake();
    test_frames();
    test_encoder();
    test_reasons();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
