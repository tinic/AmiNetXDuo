/*
 * AmiNetXDuo, dos.library file access for the configuration layer.
 *
 * This is the only file in src/config that talks to AmigaDOS. Everything else
 * works on memory buffers, which is what lets the host test drive the same
 * parsers. No newlib stdio anywhere: this code lives in a shared library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

/* -------------------------------------------------------------- file read */

APTR ami_cfg_read_file(const char *path, ULONG *size_out)
{
    BPTR  file;
    LONG  size;
    LONG  got;
    char *buf;

    if (size_out != NULL)
        *size_out = 0;

    if (path == NULL)
        return NULL;

    file = Open((STRPTR)path, MODE_OLDFILE);
    if (file == 0)
    {
        /* A missing configuration file is normal, not an error. */
        AMI_DEBUG("config: %s not present (%ld)", path, (long)IoErr());
        return NULL;
    }

    /* Seek() returns the *previous* position, so this yields the file size. */
    if (Seek(file, 0, OFFSET_END) < 0)
    {
        Close(file);
        return NULL;
    }
    size = Seek(file, 0, OFFSET_BEGINNING);

    if (size < 0)
    {
        AMI_WARN("config: %s: cannot determine size", path);
        Close(file);
        return NULL;
    }

    if ((ULONG)size > AMI_CFG_FILE_MAX)
    {
        AMI_WARN("config: %s: %ld bytes is too large, ignoring", path, (long)size);
        Close(file);
        return NULL;
    }

    buf = (char *)ami_alloc((ULONG)size + 1);
    if (buf == NULL)
    {
        AMI_ERROR("config: %s: out of memory (%ld bytes)", path, (long)size);
        Close(file);
        return NULL;
    }

    got = (size > 0) ? Read(file, buf, size) : 0;
    Close(file);

    if (got < 0)
    {
        AMI_WARN("config: %s: read failed (%ld)", path, (long)IoErr());
        ami_free(buf);
        return NULL;
    }

    buf[got] = '\0';

    if (size_out != NULL)
        *size_out = (ULONG)got;

    return buf;
}

/* -------------------------------------------------------------- utilities */

static VOID join_path(char *dst, ULONG dstlen, const char *dir, const char *name)
{
    ULONG pos;

    ami_cfg_copy_string(dst, dstlen, dir);
    pos = ami_cfg_strlen(dst);

    if (pos > 0 && dst[pos - 1] != '/' && dst[pos - 1] != ':' && pos + 1 < dstlen)
    {
        dst[pos++] = '/';
        dst[pos]   = '\0';
    }

    ami_cfg_copy_string(dst + pos, dstlen - pos, name);
}

/* Workbench icons sit next to the configuration files, and are not one. */
static BOOL is_icon(const char *name)
{
    ULONG len = ami_cfg_strlen(name);

    if (len < 5)
        return FALSE;

    return (BOOL)(ami_cfg_stricmp(name + len - 5, ".info") == 0);
}

/* ------------------------------------------------------------- interfaces */

LONG ami_config_load_interface(const char *name, AmiIfConfig *out)
{
    char  path[AMI_CFG_PATH_LEN + AMI_CFG_NAME_LEN + 8];
    char *buf;
    LONG  result;

    if (name == NULL || out == NULL)
        return AMI_CFG_ERR_SYNTAX;

    join_path(path, sizeof(path), AMI_CFG_DIR_NETINTERFACES, name);

    buf = (char *)ami_cfg_read_file(path, NULL);
    if (buf == NULL)
    {
        ami_cfg_zero(out, sizeof(*out));
        return AMI_CFG_ERR_IO;
    }

    /*
     * `path` is on this stack frame and the reporter is handed it by pointer,
     * so the name is cleared before this function returns rather than left for
     * the next caller to overwrite.
     */
    ami_cfg_problem_file(path);
    result = ami_cfg_parse_interface(name, buf, out);
    ami_cfg_problem_file(NULL);

    ami_free(buf);

    return result;
}

/*
 * Roadshow processes the interface files in alphabetical order (a PRI tooltype
 * can override that, and icons are not read here). A sorted array makes the
 * first interface deterministic, and its gateway becomes the default route
 * when no routes file exists.
 */
static VOID insert_interface(AmiConfig *cfg, const AmiIfConfig *iface)
{
    UWORD pos;
    UWORD i;

    if (cfg->interface_count >= AMI_CFG_MAX_INTERFACES)
    {
        AMI_WARN("config: more than %ld interfaces, ignoring '%s'",
                 (long)AMI_CFG_MAX_INTERFACES, iface->name);
        return;
    }

    for (pos = 0; pos < cfg->interface_count; pos++)
    {
        if (ami_cfg_stricmp(iface->name, cfg->interfaces[pos].name) < 0)
            break;
    }

    for (i = cfg->interface_count; i > pos; i--)
        cfg->interfaces[i] = cfg->interfaces[i - 1];

    cfg->interfaces[pos] = *iface;
    cfg->interface_count++;
}

static VOID load_interfaces(AmiConfig *cfg)
{
    struct FileInfoBlock *fib;
    BPTR                  lock;

    lock = Lock((STRPTR)AMI_CFG_DIR_NETINTERFACES, ACCESS_READ);
    if (lock == 0)
    {
        AMI_WARN("config: no " AMI_CFG_DIR_NETINTERFACES " drawer");

        ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR,
                        "there is no DEVS:NetInterfaces drawer, so nothing "
                        "describes a network card",
                        "Run NetSetup: it asks which card this machine has, "
                        "then writes the drawer and the file.");
        ami_cfg_problem_file(NULL);
        return;
    }

    fib = (struct FileInfoBlock *)ami_alloc(sizeof(struct FileInfoBlock));
    if (fib == NULL)
    {
        UnLock(lock);
        return;
    }

    if (Examine(lock, fib))
    {
        while (ExNext(lock, fib))
        {
            AmiIfConfig iface;
            /* fib_FileName is TEXT[] (unsigned char), the strings here are char. */
            const char *entry_name = (const char *)fib->fib_FileName;

            if (fib->fib_DirEntryType > 0)      /* a drawer */
                continue;
            if (is_icon(entry_name))
                continue;

            if (ami_config_load_interface(entry_name, &iface) != AMI_CFG_OK)
            {
                char text[AMI_CFG_NAME_LEN + 64];

                ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
                ami_cfg_join3(text, sizeof(text), "the file '", entry_name,
                              "' could not be used, so that interface does "
                              "not exist");
                ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR, text,
                                "The problems listed above it say why.  "
                                "NetSetup can rewrite the file from scratch.");
                ami_cfg_problem_file(NULL);
                continue;
            }

            AMI_INFO("config: interface %s: %s unit %lu",
                     iface.name, iface.device, (unsigned long)iface.unit);

            insert_interface(cfg, &iface);
        }
    }

    ami_free(fib);
    UnLock(lock);

    if (cfg->interface_count == 0)
    {
        ami_cfg_problem_file(AMI_CFG_DIR_NETINTERFACES);
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR,
                        "the DEVS:NetInterfaces drawer holds no usable "
                        "interface file",
                        "One file per network card goes in there.  The name "
                        "of the file is the name of the card, and eth0 is "
                        "the usual choice.  NetSetup writes one.");
        ami_cfg_problem_file(NULL);
    }
}

/* ----------------------------------------------------------------- pieces */

static VOID load_resolver(AmiConfig *cfg)
{
    char *buf;

    buf = (char *)ami_cfg_read_file(AMI_CFG_FILE_NAMERES, NULL);
    if (buf != NULL)
    {
        ami_cfg_parse_resolver(buf, &cfg->resolver,
                               cfg->hostname, sizeof(cfg->hostname));
        ami_free(buf);
    }

    /*
     * AmiTCP installations keep NAMESERVER/DOMAIN/HOST lines in the netdb
     * file itself. They are read too, but only to fill gaps: a real
     * name_resolution file always wins.
     */
    if (cfg->resolver.nameserver_count == 0 || cfg->hostname[0] == '\0')
    {
        buf = (char *)ami_cfg_read_file(AMI_CFG_FILE_HOSTS, NULL);
        if (buf != NULL)
        {
            AmiResolverConfig extra;

            ami_cfg_zero(&extra, sizeof(extra));
            ami_cfg_parse_resolver(buf, &extra,
                                   cfg->hostname, sizeof(cfg->hostname));

            if (cfg->resolver.nameserver_count == 0)
            {
                cfg->resolver.nameserver_count = extra.nameserver_count;
                {
                    UWORD i;
                    for (i = 0; i < extra.nameserver_count; i++)
                    {
                        cfg->resolver.nameserver[i]     = extra.nameserver[i];
                        cfg->resolver.nameserver_use[i] = extra.nameserver_use[i];
                    }
                }
            }
            if (cfg->resolver.domain[0] == '\0')
                ami_cfg_copy_string(cfg->resolver.domain,
                                    sizeof(cfg->resolver.domain), extra.domain);

            ami_free(buf);
        }
    }

    /*
     * A HOSTNAME or HOST line went straight into cfg->hostname, so the rank is
     * recorded here rather than through ami_config_hostname_offer(). This is
     * the strongest source. Nothing later can displace it.
     */
    if (cfg->hostname[0] != '\0')
        cfg->hostname_source = (UWORD)AMI_HOSTNAME_NAMERES;
}

static VOID load_gateway(AmiConfig *cfg)
{
    static const char *const files[] =
    {
        AMI_CFG_FILE_GATEWAY,   /* docs/RESEARCH.md §6.6 and the config.h contract */
        AMI_CFG_FILE_ROUTES,    /* where Roadshow 1.15 really keeps it */
        NULL
    };
    int i;

    for (i = 0; files[i] != NULL && cfg->default_gateway == 0; i++)
    {
        char *buf = (char *)ami_cfg_read_file(files[i], NULL);

        if (buf == NULL)
            continue;

        ami_cfg_parse_gateway(buf, &cfg->default_gateway);
        ami_free(buf);
    }

    /* Fall back to a GATEWAY= given in an interface file. */
    if (cfg->default_gateway == 0)
    {
        UWORD i2;

        for (i2 = 0; i2 < cfg->interface_count; i2++)
        {
            if (cfg->interfaces[i2].gateway != 0)
            {
                cfg->default_gateway = cfg->interfaces[i2].gateway;
                break;
            }
        }
    }
}

static VOID load_tcp_handler(AmiConfig *cfg)
{
    char *buf = (char *)ami_cfg_read_file(AMI_CFG_FILE_TCPHANDLER, NULL);

    if (buf == NULL)
        return;                     /* no file: the device stays published */

    ami_cfg_problem_file(AMI_CFG_FILE_TCPHANDLER);
    ami_cfg_parse_tcp_handler(buf, &cfg->tcp_handler);
    ami_cfg_problem_file(NULL);

    ami_free(buf);

    if (!cfg->tcp_handler)
        AMI_INFO("config: TCP: switched off in " AMI_CFG_FILE_TCPHANDLER);
}

#ifdef AMINETXDUO_MDNS
/*
 * Only in an mDNS build. The AmiSdService fields exist in both builds, so
 * nothing else changes. A build with no responder must not open a file it can
 * do nothing with.
 */
static VOID load_dnssd(AmiConfig *cfg)
{
    char *buf = (char *)ami_cfg_read_file(AMI_CFG_FILE_DNSSD, NULL);

    if (buf == NULL)
        return;

    ami_cfg_problem_file(AMI_CFG_FILE_DNSSD);
    ami_cfg_parse_dnssd(buf, cfg->sd_services, AMI_CFG_MAX_SD_SERVICES,
                        &cfg->sd_service_count);
    ami_cfg_problem_file(NULL);

    /* Every field was copied into cfg, so the text goes back now. */
    ami_free(buf);

    AMI_INFO("config: %lu service(s) to advertise",
             (unsigned long)cfg->sd_service_count);
}
#endif

/*
 * The name an administrator gave this machine, or nothing.
 *
 * load_resolver() has already taken any HOSTNAME or HOST line from
 * DEVS:Internet/name_resolution, which outranks both sources here. The rest of
 * the chain, offered weakest first, is an interface file's ID= and then
 * ENV:HOSTNAME. AmiHostnameSource in aminetxduo/config.h gives the order. DHCP
 * option 12 outranks both and arrives later, once there is a lease.
 *
 * Nothing is invented after that. gethostname() derives a name from the
 * interface address instead (bsdsocket.doc NOTES). DHCP option 12 and the mDNS
 * host label need a network label whatever happens, and carry their own
 * default.
 */
static VOID load_hostname(AmiConfig *cfg)
{
    char *buf = (char *)ami_cfg_read_file("ENV:HOSTNAME", NULL);

    ami_cfg_hostname_from_files(cfg, buf);

    if (buf != NULL)
        ami_free(buf);
}

/* -------------------------------------------------------------------- API */

/*
 * Fills cfg from DEVS: and allocates nothing the caller has to give back.
 *
 * This function does not call ami_netdb_load(). That load is 12,616 bytes of
 * hosts/networks/protocols/services tables, held in file-scope statics in
 * netdb.c and never reclaimed by AmigaOS when the process exits. Nothing
 * parsed here reads the netdb, and a lookup loads it on its own through
 * netdb_table().
 *
 * A caller that needs the tables asks for them. bsd_lib_open() does, on a
 * Process, because the netstack's own threads then look up names from a
 * context that cannot open a file. Four commands do. The rest allocate nothing
 * here. tools/check-netdb-free.sh checks that anything reaching
 * ami_netdb_load() also links ami_netdb_free().
 */
LONG ami_config_load(AmiConfig *cfg)
{
    if (cfg == NULL)
        return AMI_CFG_ERR_SYNTAX;

    ami_cfg_zero(cfg, sizeof(*cfg));
    cfg->tcp_handler = TRUE;

    load_interfaces(cfg);
    load_resolver(cfg);
    load_gateway(cfg);
    load_tcp_handler(cfg);
#ifdef AMINETXDUO_MDNS
    load_dnssd(cfg);
#endif

    load_hostname(cfg);

    {
        const char *source = ami_config_hostname_source_text(cfg->hostname_source);

        AMI_INFO("config: %lu interface(s), %lu name server(s), host '%s' (%s)",
                 (unsigned long)cfg->interface_count,
                 (unsigned long)cfg->resolver.nameserver_count,
                 (cfg->hostname[0] != '\0') ? cfg->hostname : "(unnamed)",
                 (source != NULL) ? source : "nothing named it");
    }

    return AMI_CFG_OK;
}
