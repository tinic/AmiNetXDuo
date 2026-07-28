/*
 * AmiNetXDuo -- host fuzz driver for the DEVS:Internet parsers.
 *
 * Same shim arrangement as src/config/test/test_config.c: the AmigaOS calls
 * the parsers need are answered here, and ami_cfg_read_file() comes from an
 * in-memory fixture rather than from disk.  The difference is that the
 * fixture is whatever bytes arrive on stdin (or in argv[1]), so the whole of
 * netdb.c, config_text.c and config_parse.c can be driven with malformed
 * input under -fsanitize=address,undefined.
 *
 * Build (see fuzz.sh):
 *   cc -fsanitize=address,undefined -g -std=c99 -Iinclude \
 *      -Isrc/config/test/shim fuzz_config.c \
 *      src/config/config_text.c src/config/config_parse.c src/config/netdb.c
 *
 * Usage:
 *   fuzz_config < case            one case, every file fed the same bytes
 *   fuzz_config -f N < case       feed only file N (0..3 netdb, 4..6 config)
 *   fuzz_config -r SEED COUNT     built-in random generator, no corpus needed
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    p = calloc(1, size);
    if (p != NULL)
        stub_outstanding++;

    return p;
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID)
{
    return stub_outstanding;
}

VOID ami_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

/* The bytes under test, and which file they are presented as. */
static char       *fz_data;
static size_t      fz_len;
static const char *fz_only;      /* NULL: every file gets the same bytes */

APTR ami_cfg_read_file(const char *path, ULONG *size_out)
{
    char *buf;

    if (size_out != NULL)
        *size_out = 0;

    if (fz_only != NULL && strcmp(fz_only, path) != 0)
        return NULL;

    buf = (char *)ami_alloc((ULONG)fz_len + 1);
    if (buf == NULL)
        return NULL;

    memcpy(buf, fz_data, fz_len);
    buf[fz_len] = '\0';

    if (size_out != NULL)
        *size_out = (ULONG)fz_len;

    return buf;
}

/* ------------------------------------------------------------- the drives */

static const char *const fz_files[] =
{
    AMI_CFG_FILE_HOSTS,
    AMI_CFG_FILE_NETWORKS,
    AMI_CFG_FILE_PROTOCOLS,
    AMI_CFG_FILE_SERVICES
};

#define FZ_NFILES (int)(sizeof(fz_files) / sizeof(fz_files[0]))

/*
 * One pass over everything a file's bytes can reach.  Each parser is driven
 * on its own so a crash names the parser rather than "the config".
 */
static void fz_run_once(char *data, size_t len, int which)
{
    AmiConfig  cfg;
    ULONG      value = 0;
    char      *scratch;

    fz_data = data;
    fz_len  = len;

    /* netdb: hosts / networks / protocols / services. */
    fz_only = (which >= 0 && which < FZ_NFILES) ? fz_files[which] : NULL;
    ami_netdb_load();
    (void)ami_netdb_host_by_name("localhost");
    (void)ami_netdb_host_by_addr(0x7F000001UL);
    (void)ami_netdb_net_by_name("lan");
    (void)ami_netdb_proto_by_name("tcp");
    (void)ami_netdb_serv_by_name("domain", "udp");
    (void)ami_netdb_serv_by_port(53, "udp");
    ami_netdb_free();

    /* The name-resolution and interface parsers work on a mutable copy. */
    scratch = (char *)malloc(len + 1);
    if (scratch == NULL)
        return;

    memcpy(scratch, data, len);
    scratch[len] = '\0';
    memset(&cfg, 0, sizeof(cfg));
    {
        char hn[64];

        hn[0] = '\0';
        ami_cfg_parse_resolver(scratch, &cfg.resolver, hn, sizeof(hn));
    }
    free(scratch);

    scratch = (char *)malloc(len + 1);
    if (scratch == NULL)
        return;
    memcpy(scratch, data, len);
    scratch[len] = '\0';
    {
        AmiIfConfig ifc;

        memset(&ifc, 0, sizeof(ifc));
        (void)ami_cfg_parse_interface("fuzz", scratch, &ifc);
    }
    free(scratch);

    scratch = (char *)malloc(len + 1);
    if (scratch == NULL)
        return;
    memcpy(scratch, data, len);
    scratch[len] = '\0';
    ami_cfg_parse_gateway(scratch, &value);
    free(scratch);

    /* The scalar converters, on a NUL-terminated copy. */
    scratch = (char *)malloc(len + 1);
    if (scratch == NULL)
        return;
    memcpy(scratch, data, len);
    scratch[len] = '\0';
    (void)ami_config_parse_ip(scratch, &value);
    (void)ami_cfg_parse_ulong(scratch, &value);
    (void)ami_cfg_parse_net_number(scratch, &value);
    free(scratch);
}

/* ------------------------------------------------------- random generator */

static unsigned long fz_state;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(fz_state >> 33);
}

/*
 * Bytes shaped like a netdb file rather than uniform noise: the interesting
 * region of this input space is quoting, whitespace runs and line breaks, and
 * uniform random bytes reach almost none of it.
 */
static const char *const fz_atoms[] =
{
    " ", "\t", "\n", "\r", "\r\n", "#", ";", "\"", "\"\"", "\"\"\"\"",
    "\\", "/", "=", ".", ":", "-", "a", "Z", "0", "9", "127.0.0.1",
    "255.255.255.255", "HOST", "NAMESERVER", "DOMAIN", "SEARCH", "domain",
    "21/tcp", "0x", "4294967296", "-1", "eth0", "localhost", "\"a b\"",
    "\"unterminated", "%s", "\x7f", "\x80", "\xff"
};

static size_t fz_generate(char *out, size_t cap)
{
    size_t len = 0;
    unsigned atoms = fz_rand() % 200u + 1u;

    while (atoms-- > 0)
    {
        const char *a = fz_atoms[fz_rand() % (sizeof(fz_atoms) /
                                              sizeof(fz_atoms[0]))];
        size_t      n = strlen(a);

        if (len + n >= cap)
            break;

        memcpy(out + len, a, n);
        len += n;
    }

    return len;
}

int main(int argc, char **argv)
{
    static char buf[65536];
    int         which = -1;
    int         i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
            which = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;

            fz_state = seed;

            for (n = 0; n < count; n++)
            {
                size_t len = fz_generate(buf, sizeof(buf));
                int    f;

                for (f = 0; f < FZ_NFILES; f++)
                    fz_run_once(buf, len, f);
            }

            printf("fuzz_config: %lu case(s) from seed %lu, clean\n",
                   count, seed);
            return 0;
        }
    }

    {
        size_t len = fread(buf, 1, sizeof(buf), stdin);

        fz_run_once(buf, len, which);
    }

    printf("fuzz_config: one case, clean\n");

    return 0;
}
