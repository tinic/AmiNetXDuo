/*
 * AmiNetXDuo, host fuzz driver for the DHCP option parser.
 *
 * DHCP is the parser with the shortest path from a hostile LAN to this
 * machine: it runs on every boot, before anything is configured, against
 * whatever answers a broadcast. With no MMU a walk off the end of an option
 * is not a crashed process, it is a write into whatever Exec put next.
 *
 * What is under test is the vendored client's option walk, not our policy.
 * netstack.c only asks for options (nx_dhcp_user_option_request) and reads the
 * results; the bytes are parsed by addons/dhcp/nxd_dhcp_client.c. Its parser
 * is `static`, so the translation unit is #included here rather than linked,
 * the alternative is fuzzing an entry point the wire cannot reach, which
 * proves nothing.
 *
 * THE LENGTH CONTRACT MATTERS. _nx_dhcp_packet_process() refuses anything not
 * longer than NX_BOOTP_OFFSET_OPTIONS (236) before it calls the parser, and
 * the parser then reads the fixed BOOTP header without re-checking. Feeding it
 * less would report an over-read the wire cannot produce, so every case here
 * is at least NX_DHCP_MIN bytes. A fuzzer that manufactures its own false
 * positives gets ignored, which is worse than not having one.
 *
 * Usage, matching fuzz_dns:
 *   fuzz_dhcp -s                every seed case, named
 *   fuzz_dhcp -c NAME          one seed case by name
 *   fuzz_dhcp -r SEED COUNT    seeds plus mutations
 *   fuzz_dhcp < message        one message from stdin
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The DHCP client stashes back-pointers in ThreadX thread and timer extension
 * slots, and those macros cast the slot, a ULONG, 32-bit from the shim's
 * tx_port.h, to a pointer, which does not survive a 64-bit host. Defined
 * away rather than building this driver 32-bit only: nothing here starts a
 * thread or a timer, the parser is called directly, so the slots are never
 * read. (fuzz_mdns is 32-bit-only for a different reason, its cache really
 * does store pointers in ULONG slots at run time.)
 */
#define NX_THREAD_EXTENSION_PTR_GET(a, b, c)    { (a) = NX_NULL; }
#define NX_TIMER_EXTENSION_PTR_GET(a, b, c)     { (a) = NX_NULL; }

#include "nx_api.h"

/*
 * The parser is static; reach it the only way the wire's shape allows.
 *
 * The push/pop is around the vendored file alone, it has unused parameters
 * this build's -Werror would reject, and they are not ours to fix. Everything
 * below the pop is this driver, under the full set.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nxd_dhcp_client.c"
#pragma GCC diagnostic pop

#define FD_MAX      1600
#define FD_MIN      (NX_BOOTP_OFFSET_OPTIONS + 1)   /* what the caller grants */

typedef struct
{
    unsigned char b[FD_MAX];
    unsigned      len;
} FdBuf;

static void fd_reset(FdBuf *w)
{
    memset(w->b, 0, sizeof(w->b));
    w->len = 0;
}

static void fd_u8(FdBuf *w, unsigned v)
{
    if (w->len < FD_MAX)
        w->b[w->len++] = (unsigned char)v;
}

static void fd_u32(FdBuf *w, unsigned long v)
{
    fd_u8(w, (unsigned)(v >> 24));
    fd_u8(w, (unsigned)(v >> 16));
    fd_u8(w, (unsigned)(v >> 8));
    fd_u8(w, (unsigned)v);
}

static void fd_pad_to(FdBuf *w, unsigned off)
{
    while (w->len < off && w->len < FD_MAX)
        fd_u8(w, 0);
}

/* The fixed BOOTP header plus the magic cookie: every case starts here, so a
   seed differs from a real reply only in its options. */
static void fd_header(FdBuf *w, unsigned long yiaddr)
{
    fd_reset(w);
    fd_u8(w, 2);                        /* op: BOOTREPLY                    */
    fd_u8(w, 1);                        /* htype: ethernet                  */
    fd_u8(w, 6);                        /* hlen                             */
    fd_u8(w, 0);                        /* hops                             */
    fd_u32(w, 0x12345678UL);            /* xid                              */
    fd_pad_to(w, NX_BOOTP_OFFSET_YOUR_IP);
    fd_u32(w, yiaddr);
    fd_pad_to(w, NX_BOOTP_OFFSET_VENDOR);
    fd_u32(w, NX_BOOTP_MAGIC_COOKIE);
}

static void fd_opt(FdBuf *w, unsigned code, unsigned len, const void *data)
{
    unsigned i;

    fd_u8(w, code);
    fd_u8(w, len);
    for (i = 0; i < len; i++)
        fd_u8(w, ((const unsigned char *)data)[i]);
}

static void fd_end(FdBuf *w)
{
    fd_u8(w, NX_DHCP_OPTION_END);
    while (w->len < FD_MIN)
        fd_u8(w, NX_DHCP_OPTION_PAD);
}

/* ------------------------------------------------------------- the seeds --- */

static void fds_valid_offer(FdBuf *w)
{
    unsigned char mask[4] = { 255, 255, 255, 0 };
    unsigned char rtr[4]  = { 192, 168, 1, 1 };
    unsigned char dns[8]  = { 192, 168, 1, 1, 8, 8, 8, 8 };
    unsigned char lease[4] = { 0, 0, 14, 16 };
    unsigned char type    = 2;      /* OFFER */

    fd_header(w, 0xC0A80164UL);     /* 192.168.1.100 */
    fd_opt(w, NX_DHCP_OPTION_DHCP_TYPE, 1, &type);
    fd_opt(w, NX_DHCP_OPTION_SUBNET_MASK, 4, mask);
    fd_opt(w, NX_DHCP_OPTION_GATEWAYS, 4, rtr);
    fd_opt(w, NX_DHCP_OPTION_DNS_SVR, 8, dns);
    fd_opt(w, NX_DHCP_OPTION_DHCP_LEASE, 4, lease);
    fd_end(w);
}

/* An option whose length runs past the last byte of the message. The walk has
   to stop at the buffer, not at the length it was told. */
static void fds_len_past_end(FdBuf *w)
{
    fd_header(w, 0xC0A80164UL);
    while (w->len < FD_MIN - 4)
        fd_u8(w, NX_DHCP_OPTION_PAD);
    fd_u8(w, NX_DHCP_OPTION_DNS_SVR);
    fd_u8(w, 200);                  /* 200 bytes promised, ~2 present */
    fd_u8(w, 8);
    fd_u8(w, 8);
}

/* The longest an option can claim, at the very end. */
static void fds_len_255(FdBuf *w)
{
    fd_header(w, 0xC0A80164UL);
    while (w->len < FD_MIN - 2)
        fd_u8(w, NX_DHCP_OPTION_PAD);
    fd_u8(w, NX_DHCP_OPTION_DNS_SVR);
    fd_u8(w, 255);
}

/* No END marker at all: the walk must be bounded by length, not by finding
   255. */
static void fds_no_end(FdBuf *w)
{
    unsigned char mask[4] = { 255, 255, 255, 0 };

    fd_header(w, 0xC0A80164UL);
    while (w->len + 6 < FD_MIN + 64)
        fd_opt(w, NX_DHCP_OPTION_SUBNET_MASK, 4, mask);
}

/* Nothing but padding after the cookie. */
static void fds_all_pad(FdBuf *w)
{
    fd_header(w, 0xC0A80164UL);
    while (w->len < FD_MIN + 32)
        fd_u8(w, NX_DHCP_OPTION_PAD);
}

/* Exactly the shortest message the caller will pass on. */
static void fds_runt(FdBuf *w)
{
    fd_header(w, 0xC0A80164UL);
    while (w->len < FD_MIN)
        fd_u8(w, NX_DHCP_OPTION_PAD);
}

/* The cookie is wrong, so the options are not options. */
static void fds_bad_cookie(FdBuf *w)
{
    unsigned char mask[4] = { 255, 255, 255, 0 };

    fd_header(w, 0xC0A80164UL);
    w->b[NX_BOOTP_OFFSET_VENDOR] = 0xDE;
    w->b[NX_BOOTP_OFFSET_VENDOR + 1] = 0xAD;
    fd_opt(w, NX_DHCP_OPTION_SUBNET_MASK, 4, mask);
    fd_end(w);
}

/* Zero-length options, which are legal for PAD/END and not for the rest. */
static void fds_zero_len(FdBuf *w)
{
    fd_header(w, 0xC0A80164UL);
    fd_opt(w, NX_DHCP_OPTION_SUBNET_MASK, 0, NULL);
    fd_opt(w, NX_DHCP_OPTION_DNS_SVR, 0, NULL);
    fd_opt(w, NX_DHCP_OPTION_DHCP_LEASE, 0, NULL);
    fd_end(w);
}

/* The same option again and again: whatever stores it must not accumulate. */
static void fds_repeat(FdBuf *w)
{
    unsigned char dns[4] = { 8, 8, 8, 8 };
    unsigned      i;

    fd_header(w, 0xC0A80164UL);
    for (i = 0; i < 60; i++)
        fd_opt(w, NX_DHCP_OPTION_DNS_SVR, 4, dns);
    fd_end(w);
}

/* An address the parser is documented to reject, so the early return is
   covered too. */
static void fds_bad_yiaddr(FdBuf *w)
{
    fd_header(w, 0xF0000001UL);     /* class E */
    fd_end(w);
}

static void fds_zeros(FdBuf *w)
{
    fd_reset(w);
    while (w->len < FD_MIN)
        fd_u8(w, 0);
}

static void fds_ones(FdBuf *w)
{
    fd_reset(w);
    while (w->len < FD_MIN)
        fd_u8(w, 0xFF);
}

typedef void (*FdSeedFn)(FdBuf *);

typedef struct
{
    const char *name;
    FdSeedFn    build;
} FdSeed;

static const FdSeed fd_seeds[] =
{
    { "valid_offer",  fds_valid_offer  },
    { "len_past_end", fds_len_past_end },
    { "len_255",      fds_len_255      },
    { "no_end",       fds_no_end       },
    { "all_pad",      fds_all_pad      },
    { "runt",         fds_runt         },
    { "bad_cookie",   fds_bad_cookie   },
    { "zero_len",     fds_zero_len     },
    { "repeat",       fds_repeat       },
    { "bad_yiaddr",   fds_bad_yiaddr   },
    { "zeros",        fds_zeros        },
    { "ones",         fds_ones         }
};

#define FD_SEED_COUNT   (int)(sizeof(fd_seeds) / sizeof(fd_seeds[0]))

/* ------------------------------------------------------------- the driver -- */

/*
 * The parser writes into an NX_DHCP_INTERFACE_RECORD and reads a handful of
 * NX_DHCP fields. Both are zeroed for every run, so a case cannot pass because
 * the one before it left something behind.
 */
static void fd_run(const unsigned char *msg, unsigned len)
{
    static NX_DHCP                  dhcp;
    static NX_DHCP_INTERFACE_RECORD rec;
    static NX_IP                    ip;
    unsigned char                   copy[FD_MAX];

    if (len < FD_MIN)
        return;                     /* below the caller's contract */
    if (len > FD_MAX)
        len = FD_MAX;

    memset(&dhcp, 0, sizeof(dhcp));
    memset(&rec, 0, sizeof(rec));
    memset(&ip, 0, sizeof(ip));

    /*
     * The parse is not pure: on a good address it reaches back through
     * nx_dhcp_ip_ptr to read and clear the interface's current address and the
     * gateway. So the IP instance has to exist and one interface has to be
     * valid, or every run stops on a null dereference in the IP layer that the
     * real client could never reach. Zeroed and valid is what an interface
     * looks like before DHCP has given it anything, which is exactly the
     * state a reply arrives in.
     */
    ip.nx_ip_id = NX_IP_ID;
    ip.nx_ip_interface[0].nx_interface_valid = NX_TRUE;
    ip.nx_ip_interface[0].nx_interface_link_up = NX_TRUE;

    dhcp.nx_dhcp_ip_ptr        = &ip;
    rec.nx_dhcp_interface_index = 0;

    /* Copied so ASan sees the real end of the buffer: a parser that walks one
       byte past `len` must land in the redzone, not in the rest of a fixed
       array that happens to be there. */
    memcpy(copy, msg, len);

    (void)_nx_dhcp_extract_information(&dhcp, &rec, copy, len);
}

/*
 * Proof that the driver reaches the parser at all.
 *
 * A fuzzer that silently stopped short, a struct the parser rejects on its
 * first field, a length that never clears the contract, reports "clean"
 * for every input and is worse than not having one, because it reads as
 * coverage. So a known-good OFFER is parsed once at startup and the fields it
 * must have filled are checked. If this fails the sweep does not run.
 */
static void fd_selftest(void)
{
    static NX_DHCP                  dhcp;
    static NX_DHCP_INTERFACE_RECORD rec;
    static NX_IP                    ip;
    unsigned char                   copy[FD_MAX];
    FdBuf                           w;
    UINT                            status;

    fds_valid_offer(&w);

    memset(&dhcp, 0, sizeof(dhcp));
    memset(&rec, 0, sizeof(rec));
    memset(&ip, 0, sizeof(ip));
    ip.nx_ip_id = NX_IP_ID;
    ip.nx_ip_interface[0].nx_interface_valid   = NX_TRUE;
    ip.nx_ip_interface[0].nx_interface_link_up = NX_TRUE;
    dhcp.nx_dhcp_ip_ptr         = &ip;
    rec.nx_dhcp_interface_index = 0;

    memcpy(copy, w.b, w.len);
    status = _nx_dhcp_extract_information(&dhcp, &rec, copy, w.len);

    if (status != NX_SUCCESS)
    {
        printf("fuzz_dhcp: SELFTEST FAILED, a valid OFFER returned %u\n",
               (unsigned)status);
        exit(2);
    }

    if (rec.nx_dhcp_ip_address != 0xC0A80164UL)
    {
        printf("fuzz_dhcp: SELFTEST FAILED, yiaddr not stored (got %08lx)\n",
               (unsigned long)rec.nx_dhcp_ip_address);
        exit(2);
    }

    if (rec.nx_dhcp_network_mask != 0xFFFFFF00UL)
    {
        printf("fuzz_dhcp: SELFTEST FAILED, option 1 not walked (mask %08lx)\n",
               (unsigned long)rec.nx_dhcp_network_mask);
        exit(2);
    }

    /*
     * A renewal time (option 58) longer than the lease it belongs to must not
     * be stored. Both fields end up in timer ticks, and the guard used to
     * compare the option's seconds against the lease already converted, so it
     * admitted a T1 up to NX_IP_PERIODIC_RATE times the lease and scheduled
     * the renewal for after the lease had expired.
     *
     * 3600-second lease, 180000-second T1: fifty times too long, and exactly
     * the value that slips through a comparison against the lease in ticks.
     */
    {
        unsigned char type      = 2;
        unsigned char lease[4]  = { 0, 0, 0x0E, 0x10 };         /* 3600     */
        unsigned char renew[4]  = { 0, 0x02, 0xBF, 0x20 };      /* 180000   */

        fd_reset(&w);
        fd_header(&w, 0xC0A80164UL);
        fd_opt(&w, NX_DHCP_OPTION_DHCP_TYPE, 1, &type);
        fd_opt(&w, NX_DHCP_OPTION_DHCP_LEASE, 4, lease);
        fd_opt(&w, NX_DHCP_OPTION_RENEWAL, 4, renew);
        fd_end(&w);

        memset(&rec, 0, sizeof(rec));
        rec.nx_dhcp_interface_index = 0;
        memcpy(copy, w.b, w.len);
        (void)_nx_dhcp_extract_information(&dhcp, &rec, copy, w.len);

        if (rec.nx_dhcp_lease_time != 3600UL * (ULONG)NX_IP_PERIODIC_RATE)
        {
            printf("fuzz_dhcp: SELFTEST FAILED, option 51 not stored in "
                   "ticks (%lu)\n", (unsigned long)rec.nx_dhcp_lease_time);
            exit(2);
        }

        if (rec.nx_dhcp_renewal_time > rec.nx_dhcp_lease_time)
        {
            printf("fuzz_dhcp: SELFTEST FAILED, a renewal time of %lu ticks "
                   "was accepted against a lease of %lu\n",
                   (unsigned long)rec.nx_dhcp_renewal_time,
                   (unsigned long)rec.nx_dhcp_lease_time);
            exit(2);
        }
    }
}

static unsigned long fd_state = 1;

static unsigned fd_rand(void)
{
    fd_state = fd_state * 1103515245UL + 12345UL;
    return (unsigned)((fd_state >> 16) & 0x7FFFUL);
}

static unsigned fd_below(unsigned n)
{
    return (n == 0) ? 0 : (fd_rand() % n);
}

/* Aimed at the bytes that decide how far the walk goes, an option code or a
   length, not at the payload. A flipped address byte changes an answer; a
   flipped length byte changes where the next read lands. */
static void fd_mutate(FdBuf *w)
{
    unsigned rounds = fd_below(6) + 1u;

    while (rounds-- > 0)
    {
        unsigned at;

        if (w->len <= NX_BOOTP_OFFSET_OPTIONS)
            return;

        switch (fd_below(6))
        {
        case 0:     /* any byte in the options */
            at = NX_BOOTP_OFFSET_OPTIONS +
                 fd_below(w->len - NX_BOOTP_OFFSET_OPTIONS);
            w->b[at] = (unsigned char)fd_rand();
            break;
        case 1:     /* a length byte to its maximum */
            at = NX_BOOTP_OFFSET_OPTIONS +
                 fd_below(w->len - NX_BOOTP_OFFSET_OPTIONS);
            w->b[at] = 255;
            break;
        case 2:     /* a length byte to zero */
            at = NX_BOOTP_OFFSET_OPTIONS +
                 fd_below(w->len - NX_BOOTP_OFFSET_OPTIONS);
            w->b[at] = 0;
            break;
        case 3:     /* truncate, never below the contract */
            w->len = FD_MIN + fd_below(w->len - FD_MIN + 1);
            break;
        case 4:     /* break the cookie */
            w->b[NX_BOOTP_OFFSET_VENDOR + fd_below(4)] =
                (unsigned char)fd_rand();
            break;
        default:    /* an END where one was not */
            at = NX_BOOTP_OFFSET_OPTIONS +
                 fd_below(w->len - NX_BOOTP_OFFSET_OPTIONS);
            w->b[at] = NX_DHCP_OPTION_END;
            break;
        }
    }
}

static void fd_run_seed(int s)
{
    FdBuf w;

    fd_seeds[s].build(&w);
    fd_run(w.b, w.len);
}

int main(int argc, char **argv)
{
    int i;

    fd_selftest();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FD_SEED_COUNT; s++)
            {
                fd_run_seed(s);
                printf("  %-16s ok\n", fd_seeds[s].name);
            }

            printf("fuzz_dhcp: %d seed case(s), clean\n", FD_SEED_COUNT);
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FD_SEED_COUNT; s++)
            {
                if (strcmp(fd_seeds[s].name, want) == 0)
                {
                    fd_run_seed(s);
                    printf("fuzz_dhcp: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_dhcp: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            int           s;

            /* The seeds first, always: a sweep that never runs the known
               shapes is a sweep whose coverage nobody can state. */
            for (s = 0; s < FD_SEED_COUNT; s++)
                fd_run_seed(s);

            fd_state = seed ? seed : 1;

            for (n = 0; n < count; n++)
            {
                FdBuf w;

                fd_seeds[fd_below((unsigned)FD_SEED_COUNT)].build(&w);
                fd_mutate(&w);
                fd_run(w.b, w.len);
            }

            printf("fuzz_dhcp: %d seed(s) + %lu mutation(s) from %lu, clean\n",
                   FD_SEED_COUNT, count, seed);
            return 0;
        }
    }

    /* One message on stdin. */
    {
        unsigned char buf[FD_MAX];
        size_t        got = fread(buf, 1, sizeof(buf), stdin);

        fd_run(buf, (unsigned)got);
    }

    return 0;
}
