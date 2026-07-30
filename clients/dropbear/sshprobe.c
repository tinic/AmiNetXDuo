/*
 * SSHProbe -- the two things an SSH session has to be measured with from
 * inside the guest.
 *
 *   SSHProbe mem        one line of AvailMem: total, largest, chip, fast
 *   SSHProbe bulk <kb>  write <kb> KB of printable bytes to Output()
 *
 * Both exist to be run BY THE SERVER, over the channel.  `mem` run through a
 * session reports the machine with the server, the session and the client all
 * live, which is the figure the 4 MB floor has to be compared against and the
 * one no host-side tool can take.  `bulk` is the payload: two sizes in one run
 * subtract the handshake out of a throughput figure, since everything except
 * the bytes is the same in both.
 *
 * dos.library and exec only, so it costs a Shell's 4 KB stack and adds nothing
 * to the measurement it is part of.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: SSHProbe 1.0 (30.7.2026)";

#define CHUNK 4096

static UBYTE buf[CHUNK];

static int probe_mem(void)
{
    Printf((CONST_STRPTR)"AvailMem total %ld largest %ld chip %ld fast %ld\n",
           (LONG)AvailMem(MEMF_ANY),
           (LONG)AvailMem(MEMF_ANY | MEMF_LARGEST),
           (LONG)AvailMem(MEMF_CHIP),
           (LONG)AvailMem(MEMF_FAST));
    return RETURN_OK;
}

/* A repeating 64-column line.  Printable and newline-terminated so the bytes
   survive whatever a channel and a T: file do to them, and so a truncated
   transfer is visible in the report rather than silent. */
static void fill(void)
{
    LONG i;

    for (i = 0; i < CHUNK; i++)
        buf[i] = ((i % 64) == 63) ? '\n' : (UBYTE)('0' + (i % 10));
}

static int probe_bulk(LONG kb)
{
    BPTR out = Output();
    LONG left = kb * 1024L;

    fill();
    while (left > 0)
    {
        LONG n = (left < CHUNK) ? left : CHUNK;

        if (Write(out, buf, n) != n)
        {
            PutStr((CONST_STRPTR)"SSHProbe: short write\n");
            return RETURN_FAIL;
        }
        left -= n;
    }
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && argv[1][0] == 'm')
        return probe_mem();

    if (argc >= 3 && argv[1][0] == 'b')
    {
        LONG kb = 0;

        (void)StrToLong((STRPTR)argv[2], &kb);
        if (kb > 0)
            return probe_bulk(kb);
    }

    PutStr((CONST_STRPTR)"usage: SSHProbe mem | SSHProbe bulk <kb>\n");
    return RETURN_ERROR;
}
