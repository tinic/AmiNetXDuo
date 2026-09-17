/*
 * AmiNetXDuo, dos.library file access for the configuration layer.
 *
 * The ONLY file in src/config that talks to AmigaDOS, through exactly two
 * seams: ami_cfg_read_file() and ami_cfg_scan_interfaces().  No newlib stdio:
 * this code lives in a shared library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"

#include "aminetxduo/config_advice.h"
#include "aminetxduo/pool.h"
#include "aminetxduo/compat.h"

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* ------------------------------------------------------------ where DEVS: is */

/*
 * A self-contained installation keeps its configuration beside its library,
 * under AmiNetXDuo:Devs, and its Devs is the LAST member of the DEVS:
 * multi-assign, behind the system's.  So on a machine that also has Roadshow,
 * DEVS:NetInterfaces is Roadshow's drawer and DEVS:Internet/routes is
 * Roadshow's file, and the files this installer wrote would never be read.
 * Every DEVS: path this layer opens is therefore redirected into that drawer
 * when it exists; the strings stay "DEVS:..." everywhere else because that is
 * the documented name and the right one on a system install, where
 * AmiNetXDuo: is either the documentation drawer, which has no Devs, or not
 * assigned at all.
 *
 * A path the caller gave with DEVS: in it is redirected too, deliberately:
 * the installer's own S:Network-Startup line names DEVS:NetInterfaces/eth0,
 * and on the Roadshow machine above that name resolves to Roadshow's eth0.
 *
 * Probed on every call, not cached: the library outlives any assign, and a
 * Lock on an assigned directory costs nothing worth remembering.
 */
#define AMI_CFG_OWN_DEVS        "AmiNetXDuo:Devs"
#define AMI_CFG_DEVS_PREFIX     "DEVS:"

static BOOL own_devs_exists(VOID)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            saved;
    BPTR            lock;

    /* Lock() from a task is not allowed, and on a system install the name
       may not be assigned: no "Please insert volume AmiNetXDuo". */
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return FALSE;

    saved = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1L;
    lock = Lock((STRPTR)AMI_CFG_OWN_DEVS, ACCESS_READ);
    me->pr_WindowPtr = saved;

    if (lock == 0)
        return FALSE;

    UnLock(lock);
    return TRUE;
}

const char *ami_cfg_resolve(const char *path, char *buf, ULONG buflen)
{
    static const char prefix[] = AMI_CFG_DEVS_PREFIX;
    ULONG i;

    if (path == NULL || buf == NULL || buflen == 0)
        return path;

    for (i = 0; prefix[i] != '\0'; i++)
    {
        char c = path[i];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != prefix[i])
            return path;
    }

    if (!own_devs_exists())
        return path;

    ami_cfg_join3(buf, buflen, AMI_CFG_OWN_DEVS "/", path + i, NULL);
    return buf;
}

/*
 * The resolved name, in a buffer from the pool rather than the stack: this
 * runs under bsd_lib_open(), whose depth is what every Shell command's 4 KB
 * has to leave room for (tools/check-stack-frames.sh), and 128 bytes there
 * is 128 bytes off every command.  NULL means the pool is empty, and the
 * caller uses the name as given.
 */
static char *resolved_path(const char *path)
{
    char *where = (char *)ami_alloc((ULONG)AMI_CFG_PATH_LEN);

    if (where == NULL)
        return NULL;

    if (ami_cfg_resolve(path, where, (ULONG)AMI_CFG_PATH_LEN) == path)
    {
        ami_free(where);
        return NULL;
    }

    return where;
}

/* -------------------------------------------------------------- file read */

static APTR read_file_at(const char *path, ULONG *size_out);

APTR ami_cfg_read_file(const char *path, ULONG *size_out)
{
    char *where;
    APTR  result;

    if (size_out != NULL)
        *size_out = 0;

    if (path == NULL)
        return NULL;

    where  = resolved_path(path);
    result = read_file_at((where != NULL) ? where : path, size_out);

    if (where != NULL)
        ami_free(where);

    return result;
}

static APTR read_file_at(const char *path, ULONG *size_out)
{
    BPTR  file;
    LONG  size;
    LONG  got;
    char *buf;

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

/* Workbench icons sit next to the configuration files, and are not one. */
static BOOL is_icon(const char *name)
{
    ULONG len = ami_cfg_strlen(name);

    if (len < 5)
        return FALSE;

    return (BOOL)(ami_cfg_stricmp(name + len - 5, ".info") == 0);
}

/* ------------------------------------------------------------- interfaces */

/*
 * Every interface file in DEVS:NetInterfaces, handed to `sink` one name at a
 * time.  FALSE only when the drawer itself is missing.  A sink and not a list:
 * the file count is not known before the scan and has no ceiling.
 */
BOOL ami_cfg_scan_interfaces(AmiConfig *cfg, AmiCfgIfaceSink sink)
{
    struct FileInfoBlock *fib;
    BPTR                  lock;
    char                 *where;
    const char           *dir;

    if (cfg == NULL || sink == NULL)
        return FALSE;

    where = resolved_path(AMI_CFG_DIR_NETINTERFACES);
    dir   = (where != NULL) ? where : AMI_CFG_DIR_NETINTERFACES;
    lock  = Lock((STRPTR)dir, ACCESS_READ);
    if (lock == 0)
    {
        AMI_WARN("config: no %s drawer", dir);

        ami_cfg_problem_file(dir);
        ami_cfg_problem_code(0, AMI_CFG_PROBLEM_ERROR, AMI_CFG_SAYS_THERE_IS_NO_DEVS, AMI_CFG_ADVICE_RUN_NETSETUP_IT_ASKS);
        ami_cfg_problem_file(NULL);
        if (where != NULL)
            ami_free(where);
        return FALSE;
    }

    if (where != NULL)
        ami_free(where);

    fib = (struct FileInfoBlock *)ami_alloc(sizeof(struct FileInfoBlock));
    if (fib == NULL)
    {
        UnLock(lock);
        return FALSE;
    }

    if (Examine(lock, fib))
    {
        while (ExNext(lock, fib))
        {
            /* fib_FileName is TEXT[] (unsigned char), the strings here are char. */
            const char *entry_name = (const char *)fib->fib_FileName;

            if (fib->fib_DirEntryType > 0)      /* a drawer */
                continue;
            if (is_icon(entry_name))
                continue;

            sink(cfg, entry_name);
        }
    }

    ami_free(fib);
    UnLock(lock);

    return TRUE;
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

    /* AmiTCP keeps NAMESERVER/DOMAIN/HOST in the netdb file; read only to
       fill gaps -- a real name_resolution file always wins. */
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

    /* And last, the interface files: AmiTCP_NG's installer writes NAMESERVER
       and DOMAIN there, and nothing here read them. */
    ami_config_resolver_from_interfaces(cfg);

    /* The strongest host-name source; nothing later can displace it. */
    if (cfg->hostname[0] != '\0')
        cfg->hostname_source = (UWORD)AMI_HOSTNAME_NAMERES;
}

static VOID load_gateway(AmiConfig *cfg)
{
    char *buf;

    /* The compatibility file wins for the default only.  The Roadshow file
       is still always read below, because it can carry specific routes even
       when this older file already supplied DEFAULT. */
    buf = (char *)ami_cfg_read_file(AMI_CFG_FILE_GATEWAY, NULL);
    if (buf != NULL)
    {
        ami_cfg_problem_file(AMI_CFG_FILE_GATEWAY);
        ami_cfg_parse_gateway(buf, &cfg->default_gateway);
        ami_cfg_problem_file(NULL);
        ami_free(buf);
    }

    buf = (char *)ami_cfg_read_file(AMI_CFG_FILE_ROUTES, NULL);
    if (buf != NULL)
    {
        ami_cfg_problem_file(AMI_CFG_FILE_ROUTES);
        ami_cfg_parse_routes(buf, cfg);
        ami_cfg_problem_file(NULL);
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

#ifdef AMINETXDUO_TCPDEVICE
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
#endif /* AMINETXDUO_TCPDEVICE */

#ifdef AMINETXDUO_MDNS
/* mDNS builds only: a build with no responder must not open this file. */
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
 * The name an administrator gave this machine, or nothing.  Offered weakest
 * first: an interface file's ID=, then ENV:HOSTNAME.  AmiHostnameSource in
 * aminetxduo/config.h gives the full order.  Nothing is invented here.
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
 * Deliberately does NOT call ami_netdb_load(); a caller that needs the tables
 * asks for them, and must then also link ami_netdb_free().
 */
/*
 * The pool-share dial: a bare number in ENV:ANXDPOOLDIV is the divisor over
 * free memory that sizes the packet pool.  Anything else is the fallback,
 * silently.
 */
/* One number in an ENV: file, inside [lo, hi], else `fallback`.  The text
   rule is ami_cfg_env_number()'s in config_text.c, where the host test
   drives it; this is only the file. */
static ULONG cfg_env_number(const char *path, ULONG lo, ULONG hi,
                            ULONG fallback)
{
    char  *buf = (char *)ami_cfg_read_file(path, NULL);
    ULONG  value;

    if (buf == NULL)
        return fallback;

    value = ami_cfg_env_number(buf, lo, hi, fallback);
    ami_free(buf);

    return value;
}

ULONG ami_config_pool_divisor(ULONG fallback)
{
    return cfg_env_number("ENV:ANXDPOOLDIV", 4UL, 64UL, fallback);
}

ULONG ami_config_pool_packets(VOID)
{
    return cfg_env_number("ENV:ANXDPOOLPACKETS", (ULONG)AMI_POOL_MIN_PACKETS,
                          (ULONG)AMI_POOL_MAX_PACKETS, 0UL);
}

/*
 * The diagnostic dial: a single digit 0..4 in ENV:ANXDLOGLEVEL.  Anything else
 * is the fallback, silently -- a mistyped variable must not be the reason a
 * machine says nothing.
 */
LONG ami_config_log_level(LONG fallback)
{
    char *buf = (char *)ami_cfg_read_file("ENV:ANXDLOGLEVEL", NULL);
    LONG  value = fallback;
    char *p = buf;

    if (buf == NULL)
        return fallback;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p >= '0' && *p <= '4')
    {
        LONG digit = (LONG)(*p++ - '0');

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (*p == '\0')
            value = digit;
    }

    ami_free(buf);

    return value;
}

static LONG begin_config_load(AmiConfig *cfg)
{
    if (cfg == NULL)
        return AMI_CFG_ERR_SYNTAX;

    ami_cfg_zero(cfg, sizeof(*cfg));
    cfg->tcp_handler = TRUE;

    /*
     * The invariant the rest of the tree leans on: a loaded configuration
     * always has capacity for EVERY slot NetX Duo can attach, so no later
     * reserve can move the list under an attached interface.  Fatal if unmet.
     */
    if (!ami_config_reserve(cfg, (UWORD)AMI_CFG_IFACE_FLOOR))
        return AMI_CFG_ERR_NOMEM;

    return AMI_CFG_OK;
}

static VOID finish_config_load(AmiConfig *cfg)
{
    const char *source;

    load_resolver(cfg);
    load_gateway(cfg);
#ifdef AMINETXDUO_TCPDEVICE
    load_tcp_handler(cfg);
#endif
#ifdef AMINETXDUO_MDNS
    load_dnssd(cfg);
#endif

    load_hostname(cfg);

    source = ami_config_hostname_source_text(cfg->hostname_source);

    AMI_INFO("config: %lu interface(s), %lu name server(s), host '%s' (%s)",
             (unsigned long)cfg->interface_count,
             (unsigned long)cfg->resolver.nameserver_count,
             (cfg->hostname[0] != '\0') ? cfg->hostname : "(unnamed)",
             (source != NULL) ? source : "nothing named it");
}

LONG ami_config_load(AmiConfig *cfg)
{
    LONG rc = begin_config_load(cfg);

    if (rc != AMI_CFG_OK)
        return rc;

    ami_config_load_interfaces(cfg);
    finish_config_load(cfg);

    return AMI_CFG_OK;
}

LONG ami_config_load_base(AmiConfig *cfg)
{
    LONG rc = begin_config_load(cfg);

    if (rc != AMI_CFG_OK)
        return rc;
    finish_config_load(cfg);

    return AMI_CFG_OK;
}
