/*
 * AmiNetXDuo, host fuzz driver for the DEVS:Internet parsers.
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *const fz_files[] =
{
    AMI_CFG_FILE_HOSTS,
    AMI_CFG_FILE_NETWORKS,
    AMI_CFG_FILE_PROTOCOLS,
    AMI_CFG_FILE_SERVICES
};

#define FZ_NFILES (int)(sizeof(fz_files) / sizeof(fz_files[0]))

static void fz_run_dnssd(const char *data, size_t len)
{
    UWORD         max   = AMI_CFG_MAX_SD_SERVICES - 1;
    UWORD         count = 1;
    AmiSdService *svc   = (AmiSdService *)calloc(max, sizeof(*svc));
    char         *scratch = (char *)malloc(len + 1);

    if (svc != NULL && scratch != NULL)
    {
        memcpy(scratch, data, len);
        scratch[len] = '\0';
        ami_cfg_parse_dnssd(scratch, svc, max, &count);
    }

    free(svc);
    free(scratch);
}

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

    fz_run_dnssd(data, len);
}

static unsigned long fz_state;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(fz_state >> 33);
}

static const char *const fz_atoms[] =
{
    " ", "\t", "\n", "\r", "\r\n", "#", ";", "\"", "\"\"", "\"\"\"\"",
    "\\", "/", "=", ".", ":", "-", "a", "Z", "0", "9", "127.0.0.1",
    "255.255.255.255", "HOST", "NAMESERVER", "DOMAIN", "SEARCH", "domain",
    "21/tcp", "0x", "4294967296", "-1", "eth0", "localhost", "\"a b\"",
    "\"unterminated", "%s", "\x7f", "\x80", "\xff",
    /* service_discovery: the type grammar, the txt= field and its separator. */
    "_ftp._tcp", "_http._tcp", "_x._udp", "._tcp", "_", "txt=", "TXT=",
    "txt", "path=/", "Amiga web server", "65536", "80"
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
    int         dnssd = 0;
    int         i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
            which = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            dnssd = 1;
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

                if (dnssd)
                {
                    fz_run_dnssd(buf, len);
                    continue;
                }

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

        if (dnssd)
            fz_run_dnssd(buf, len);
        else
            fz_run_once(buf, len, which);
    }

    printf("fuzz_config: one case, clean\n");

    return 0;
}
