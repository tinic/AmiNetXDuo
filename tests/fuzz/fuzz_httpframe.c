/*
 * AmiNetXDuo, host fuzz driver for httpd's request framing: the
 * Content-Length parser, the Transfer-Encoding list, and the chunked decoder.
 *
 * Usage:
 *   fuzz_httpframe -s             the seed cases, including the backlog's
 *   fuzz_httpframe -r SEED COUNT  built-in random generator, no corpus needed
 */

#include "httpframe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- rng */

static unsigned long fz_state;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(fz_state >> 33);
}

static unsigned fz_below(unsigned n)
{
    return (n == 0) ? 0 : (fz_rand() % n);
}

/* ------------------------------------------------------------------ sink */

#define FZ_SUNK_MAX     8192

static unsigned char fz_sunk[FZ_SUNK_MAX];
static unsigned long fz_sunk_n;
static int           fz_sink_bad;

static void fz_collect(void *ctx, const unsigned char *data, long len)
{
    long i;

    (void)ctx;

    /* A sink handed a non-positive length, or a null pointer with a length,
       is a decoder that has lost track of its own chunk. */
    if (len <= 0 || data == NULL)
    {
        fz_sink_bad = 1;
        return;
    }

    for (i = 0; i < len; i++)
    {
        if (fz_sunk_n < FZ_SUNK_MAX)
            fz_sunk[fz_sunk_n] = data[i];
        fz_sunk_n++;
    }
}

/* ---------------------------------------------------------- the reference */

/*
 * A second chunked decoder, 64-bit and stateless, whose only job is to
 * disagree.  It must match httpframe.c on CR handling: CR is dropped anywhere
 * in a size line, and any run of them is allowed before the LF after data.
 */
enum { FZ_REF_BAD = 0, FZ_REF_DONE, FZ_REF_MORE };

#define FZ_REF_LINE     40          /* HTTP_CHUNK_LINE                      */

static unsigned char fz_ref_body[FZ_SUNK_MAX];
static unsigned long fz_ref_body_n;
static unsigned long fz_ref_used;

static int fz_reference(const unsigned char *buf, unsigned long len)
{
    unsigned long p = 0;

    fz_ref_body_n = 0;
    fz_ref_used   = 0;

    for (;;)
    {
        char               line[FZ_REF_LINE + 2];
        unsigned long      n = 0;
        unsigned long long size = 0;
        int                digits = 0;
        int                significant = 0;
        unsigned long      k;

        /* ---- the size line ---- */

        for (;;)
        {
            if (p >= len)
                return FZ_REF_MORE;

            if (buf[p] == '\n')
            {
                p++;
                break;
            }

            if (buf[p] != '\r')
            {
                /* Longer than the buffer holds is refused, and refused as
                   soon as it is too long, there may be no LF coming. */
                if (n >= FZ_REF_LINE - 1UL)
                    return FZ_REF_BAD;

                line[n++] = (char)buf[p];
            }

            p++;
        }

        line[n] = '\0';

        for (k = 0; k < n; k++)
        {
            int d = (unsigned char)line[k];

            if      (d >= '0' && d <= '9') d -= '0';
            else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
            else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
            else                           break;

            if (significant > 0 || d != 0)
                significant++;

            size = (size << 4) | (unsigned long long)d;
            digits++;
        }

        if (digits == 0 || significant > 8)
            return FZ_REF_BAD;

        if (line[digits] != '\0' && line[digits] != ';')
            return FZ_REF_BAD;

        if (size == 0ULL)
            break;                          /* on to the trailers          */

        /* ---- the data ---- */

        /* Half a chunk is still half a chunk's worth of body: the decoder
           hands those bytes on as they arrive rather than holding them, so
           the reference counts them too. */
        if ((unsigned long long)(len - p) < size)
        {
            for (k = p; k < len; k++)
            {
                if (fz_ref_body_n < FZ_SUNK_MAX)
                    fz_ref_body[fz_ref_body_n] = buf[k];
                fz_ref_body_n++;
            }

            return FZ_REF_MORE;
        }

        for (k = 0; k < (unsigned long)size; k++)
        {
            if (fz_ref_body_n < FZ_SUNK_MAX)
                fz_ref_body[fz_ref_body_n] = buf[p + k];
            fz_ref_body_n++;
        }

        p += (unsigned long)size;

        /* ---- the line ending after it ---- */

        while (p < len && buf[p] == '\r')
            p++;

        if (p >= len)
            return FZ_REF_MORE;

        if (buf[p] != '\n')
            return FZ_REF_BAD;

        p++;
    }

    /* ---- the trailers, up to a blank line ---- */

    for (;;)
    {
        unsigned long n = 0;

        for (;;)
        {
            if (p >= len)
                return FZ_REF_MORE;

            if (buf[p] == '\n')
            {
                p++;
                break;
            }

            if (buf[p] != '\r')
                n++;

            p++;
        }

        if (n == 0UL)
        {
            fz_ref_used = p;
            return FZ_REF_DONE;
        }
    }
}

/* ----------------------------------------------------------- the checking */

static unsigned long fz_case;

static int fz_fail(const char *what, const unsigned char *body,
                   unsigned long len)
{
    unsigned long i;

    printf("fuzz_httpframe: %s at case %lu\n", what, fz_case);
    printf("  body (%lu bytes):", len);

    for (i = 0; i < len && i < 96UL; i++)
    {
        unsigned c = body[i];

        if (c >= 0x20 && c < 0x7f && c != '\\')
            printf("%c", (char)c);
        else
            printf("\\x%02x", c);
    }

    printf("\n");

    return 1;
}

static int fz_body(const unsigned char *body, unsigned long len)
{
    HttpChunk     whole;
    HttpChunk     drib;
    unsigned char whole_out[FZ_SUNK_MAX];
    unsigned long whole_n;
    unsigned char whole_state;
    unsigned long whole_total;
    long          took;
    long          drib_took = 0;
    unsigned long i;

    /* ---- the whole buffer in one call ---- */

    fz_sunk_n   = 0;
    fz_sink_bad = 0;
    http_chunk_start(&whole);

    took = http_chunk_feed(&whole, body, (long)len, fz_collect, NULL);

    if (fz_sink_bad)
        return fz_fail("the sink was handed an empty run", body, len);

    /* The count is where the next request starts. */
    if (took < 0 || (unsigned long)took > len)
        return fz_fail("consumed a count outside the buffer", body, len);

    if (whole.total != fz_sunk_n)
        return fz_fail("total disagrees with what the sink counted",
                       body, len);

    if (fz_sunk_n > len)
        return fz_fail("handed out more bytes than the body held", body, len);

    if (whole.state != HTTP_CHUNK_DONE && whole.state != HTTP_CHUNK_ERROR &&
        (unsigned long)took != len)
        return fz_fail("stopped early without finishing or failing",
                       body, len);

    if (whole.state == HTTP_CHUNK_ERROR && whole.total > len)
        return fz_fail("a failed body still delivered bytes", body, len);

    whole_n     = (fz_sunk_n < FZ_SUNK_MAX) ? fz_sunk_n : FZ_SUNK_MAX;
    whole_state = whole.state;
    whole_total = whole.total;
    memcpy(whole_out, fz_sunk, whole_n);

    /* ---- and against the reference ---- */

    switch (fz_reference(body, len))
    {
        case FZ_REF_BAD:
            /* The one that matters: a decoder that says DONE on framing the
               reference calls malformed has told the server that whatever
               follows is the next request. */
            if (whole.state != HTTP_CHUNK_ERROR)
                return fz_fail("read a body the reference calls malformed",
                               body, len);
            break;

        case FZ_REF_DONE:
            if (whole.state != HTTP_CHUNK_DONE)
                return fz_fail("did not finish a body the reference did",
                               body, len);

            if ((unsigned long)took != fz_ref_used)
                return fz_fail("ended the body somewhere else than the "
                               "reference", body, len);

            if (whole.total != fz_ref_body_n)
                return fz_fail("a different number of body bytes to the "
                               "reference", body, len);

            if (whole_n < FZ_SUNK_MAX &&
                memcmp(whole_out, fz_ref_body, whole_n) != 0)
                return fz_fail("different body bytes to the reference",
                               body, len);
            break;

        default:                            /* FZ_REF_MORE                 */
            if (whole.state == HTTP_CHUNK_DONE)
                return fz_fail("finished a body the reference says is "
                               "incomplete", body, len);

            if (whole.state == HTTP_CHUNK_ERROR)
                return fz_fail("refused a body the reference says is "
                               "incomplete", body, len);

            if (whole.total != fz_ref_body_n)
                return fz_fail("a different number of body bytes to the "
                               "reference, incomplete", body, len);
            break;
    }

    /* Once it has stopped it stays stopped: a second call on a DONE or ERROR
       decoder must not take a byte of what follows. */
    if (whole.state == HTTP_CHUNK_DONE || whole.state == HTTP_CHUNK_ERROR)
    {
        static const unsigned char next[] = "GET / HTTP/1.1\r\n\r\n";

        if (http_chunk_feed(&whole, next, (long)sizeof(next) - 1,
                            fz_collect, NULL) != 0)
            return fz_fail("kept reading after it had stopped", body, len);
    }

    /* ---- the same bytes, one per call ---- */

    fz_sunk_n   = 0;
    fz_sink_bad = 0;
    http_chunk_start(&drib);

    for (i = 0; i < len; i++)
    {
        long n = http_chunk_feed(&drib, &body[i], 1, fz_collect, NULL);

        if (n < 0 || n > 1)
            return fz_fail("a one-byte call consumed something else",
                           body, len);

        if (n == 0)
            break;                      /* DONE or ERROR: it stopped here  */

        drib_took++;
    }

    if (fz_sink_bad)
        return fz_fail("the sink was handed an empty run, dribbled",
                       body, len);

    if (drib.state != whole_state)
        return fz_fail("a different state when dribbled", body, len);

    if (drib.total != whole_total)
        return fz_fail("a different total when dribbled", body, len);

    if (drib_took != took)
        return fz_fail("a different count when dribbled", body, len);

    if (fz_sunk_n != whole_n && whole_n < FZ_SUNK_MAX)
        return fz_fail("a different number of body bytes when dribbled",
                       body, len);

    if (whole_n < FZ_SUNK_MAX && memcmp(fz_sunk, whole_out, whole_n) != 0)
        return fz_fail("different body bytes when dribbled", body, len);

    return 0;
}

/* ---------------------------------------------------------- the generator */

#define FZ_BODY_MAX     512

/* A body that is mostly a chunked body: pure random bytes reach ERROR on the
   first size line and never see CHUNK_DATA or the trailer. */
static unsigned long fz_make(unsigned char *out)
{
    unsigned long n = 0;
    unsigned      chunks;
    unsigned      k;

    if (fz_below(8) == 0)
    {
        unsigned long len = fz_below(FZ_BODY_MAX);

        for (n = 0; n < len; n++)
            out[n] = (unsigned char)fz_rand();

        return n;
    }

    chunks = fz_below(4) + 1;

    for (k = 0; k < chunks && n + 64UL < FZ_BODY_MAX; k++)
    {
        unsigned long size = fz_below(24);
        char          line[48];
        unsigned long j;
        int           written;

        switch (fz_below(10))
        {
            case 0:     /* the nine-hex-digit wrap from the backlog        */
                written = snprintf(line, sizeof(line), "1%08lx",
                                   (unsigned long)fz_rand() & 0xffffffUL);
                break;
            case 1:     /* junk after the digits                           */
                written = snprintf(line, sizeof(line), "%lxZ", size);
                break;
            case 2:     /* a chunk extension                               */
                written = snprintf(line, sizeof(line), "%lx;a=b", size);
                break;
            case 3:     /* leading zeros                                   */
                written = snprintf(line, sizeof(line), "%016lx", size);
                break;
            case 4:     /* a size line with nothing on it                  */
                written = snprintf(line, sizeof(line), "%s", "");
                break;
            case 5:     /* the largest a ULONG holds, and one past it      */
                written = snprintf(line, sizeof(line), "%s",
                                   (fz_below(2) != 0) ? "ffffffff"
                                                      : "100000000");
                break;
            case 6:     /* a size that disagrees with the data that comes  */
                written = snprintf(line, sizeof(line), "%lx", size + 8UL);
                break;
            default:
                written = snprintf(line, sizeof(line), "%lx", size);
                break;
        }

        if (written < 0)
            written = 0;

        for (j = 0; j < (unsigned long)written && n + 1UL < FZ_BODY_MAX; j++)
            out[n++] = (unsigned char)line[j];

        if (n + 2UL < FZ_BODY_MAX)
        {
            out[n++] = '\r';
            out[n++] = '\n';
        }

        for (j = 0; j < size && n + 1UL < FZ_BODY_MAX; j++)
            out[n++] = (unsigned char)('a' + (fz_rand() % 26));

        if (n + 2UL < FZ_BODY_MAX)
        {
            if (fz_below(8) == 0)
            {
                out[n++] = (unsigned char)fz_rand();
                out[n++] = (unsigned char)fz_rand();
            }
            else
            {
                out[n++] = '\r';
                out[n++] = '\n';
            }
        }
    }

    if (fz_below(8) != 0 && n + 8UL < FZ_BODY_MAX)
    {
        out[n++] = '0';
        out[n++] = '\r';
        out[n++] = '\n';

        if (fz_below(3) == 0)
        {
            static const char trailer[] = "X-A: 1\r\n";
            unsigned long     j;

            for (j = 0; j < sizeof(trailer) - 1UL && n + 1UL < FZ_BODY_MAX;
                 j++)
                out[n++] = (unsigned char)trailer[j];
        }

        if (n + 2UL < FZ_BODY_MAX)
        {
            out[n++] = '\r';
            out[n++] = '\n';
        }
    }

    {
        unsigned bites = fz_below(4);
        unsigned b;

        for (b = 0; b < bites && n > 0UL; b++)
            out[fz_below((unsigned)n)] = (unsigned char)fz_rand();
    }

    if (fz_below(4) == 0 && n > 0UL)
        n = fz_below((unsigned)n);

    return n;
}

/* --------------------------------------------------- the header parsers */

/* Neither has an invariant richer than terminating and returning one of its own
   values, but both walk a caller's string to its NUL. */
static int fz_headers(void)
{
    char          text[64];
    unsigned long len = fz_below(sizeof(text) - 1UL);
    unsigned long i;
    unsigned long n = 0;

    static const char alphabet[] = "0123456789abcdefxX ,;\t+-chunkedgzip";

    for (i = 0; i < len; i++)
        text[i] = alphabet[fz_rand() % (sizeof(alphabet) - 1UL)];
    text[len] = '\0';

    switch (http_frame_length(text, &n))
    {
        case HTTP_FRAME_OK:
            break;
        case HTTP_FRAME_EMPTY:
        case HTTP_FRAME_JUNK:
        case HTTP_FRAME_OVERFLOW:
            /* Nothing is written when it is refused. */
            if (n != 0UL)
            {
                printf("fuzz_httpframe: a refused length wrote a value at "
                       "case %lu\n", fz_case);
                return 1;
            }
            break;
        default:
            printf("fuzz_httpframe: an unknown length result at case %lu\n",
                   fz_case);
            return 1;
    }

    if (http_frame_error(HTTP_FRAME_JUNK) == NULL)
        return 1;

    switch (http_frame_coding(text))
    {
        case HTTP_TE_IDENTITY:
        case HTTP_TE_CHUNKED:
        case HTTP_TE_UNSUPPORTED:
            break;
        default:
            printf("fuzz_httpframe: an unknown coding result at case %lu\n",
                   fz_case);
            return 1;
    }

    return 0;
}

/* --------------------------------------------------------------- the seeds */

/* The named cases, so they run on every ctest.  Each was a way to end a body
   early and have the rest of it parsed as a request. */
static int fz_seeds(void)
{
    static const char *bodies[] = {
        /* Nine hex digits: 0x100000000 truncated to 0, which reads as the
           terminating chunk.  Everything after it was a pipelined request. */
        "100000000\r\nDELETE /x HTTP/1.1\r\n\r\n",
        /* A size line long enough to be truncated into one that parses. */
        "0000000000000000000000000005\r\nhello\r\n0\r\n\r\n",
        "5X\r\nhello\r\n0\r\n\r\n",
        "\r\n0\r\n\r\n",
        ";a=b\r\n",
        /* Data that does not end where its size said. */
        "5\r\nhelloXX\r\n0\r\n\r\n",
        "5\r\nhi\r\n0\r\n\r\n",
        /* The ordinary ones, so a change that breaks them is caught here. */
        "5\r\nhello\r\n0\r\n\r\n",
        "0\r\n\r\n",
        "1\r\nx\r\n0\r\nX-T: 1\r\n\r\n",
        "5;ext=1\r\nhello\r\n0\r\n\r\n",
        "ffffffff\r\n",
        "a\r\n0123456789\r\n0\r\n\r\n",
        /* Truncated at every interesting boundary. */
        "5\r\nhel",
        "5\r\n",
        "5",
        "",
        "0\r\n",
        NULL
    };

    static const struct { const char *v; HttpFrameResult why; } lengths[] = {
        { "4294967306", HTTP_FRAME_OVERFLOW },
        { "4294967296", HTTP_FRAME_OVERFLOW },
        { "4294967295", HTTP_FRAME_OK },
        { "5abc",       HTTP_FRAME_JUNK },
        { "",           HTTP_FRAME_EMPTY },
        { "0",          HTTP_FRAME_OK },
        { NULL,         HTTP_FRAME_OK }
    };

    static const struct { const char *v; HttpFrameCoding te; } codings[] = {
        { "chunked",       HTTP_TE_CHUNKED },
        { "chunkedX",      HTTP_TE_UNSUPPORTED },
        { "gzip, chunked", HTTP_TE_UNSUPPORTED },
        { "",              HTTP_TE_UNSUPPORTED },
        { NULL,            HTTP_TE_IDENTITY }
    };

    unsigned long i;
    unsigned long cases = 0;

    for (i = 0; bodies[i] != NULL; i++)
    {
        fz_case = i;

        if (fz_body((const unsigned char *)bodies[i],
                    (unsigned long)strlen(bodies[i])))
            return 1;
    }

    cases += i;

    for (i = 0; lengths[i].v != NULL; i++)
    {
        unsigned long n = 0;

        if (http_frame_length(lengths[i].v, &n) != lengths[i].why)
        {
            printf("fuzz_httpframe: \"%s\" is not the length it was\n",
                   lengths[i].v);
            return 1;
        }
    }

    cases += i;

    for (i = 0; codings[i].v != NULL; i++)
    {
        if (http_frame_coding(codings[i].v) != codings[i].te)
        {
            printf("fuzz_httpframe: \"%s\" is not the coding it was\n",
                   codings[i].v);
            return 1;
        }
    }

    cases += i;

    printf("fuzz_httpframe: %lu seed case(s), clean\n", cases);

    return 0;
}

int main(int argc, char **argv)
{
    unsigned char body[FZ_BODY_MAX];
    unsigned long seed  = 1;
    unsigned long count = 10000;
    unsigned long n;
    int           seeds = 0;
    int           i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            seed  = strtoul(argv[++i], NULL, 0);
            count = strtoul(argv[++i], NULL, 0);
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            seeds = 1;
        }
    }

    if (seeds)
        return fz_seeds();

    fz_state = seed;

    for (n = 0; n < count; n++)
    {
        unsigned long len = fz_make(body);

        fz_case = n;

        if (fz_body(body, len))
            return 1;

        if (fz_headers())
            return 1;
    }

    printf("fuzz_httpframe: %lu case(s) from seed %lu, clean\n", count, seed);

    return 0;
}
