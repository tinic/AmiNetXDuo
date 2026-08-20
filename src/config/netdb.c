/*
 * AmiNetXDuo, the DEVS:Internet netdb store.
 *
 * Backs get{host,net,proto,serv}by*() in bsdsocket.library. The four files are
 * in the standard /etc layout: whitespace-separated columns, '#' starts a
 * comment that runs to the end of the line, aliases follow the canonical name.
 *
 *   hosts       <address>  <name> [alias...]
 *   networks    <name>     <net>  [alias...]
 *   protocols   <name>     <num>  [alias...]
 *   services    <name>     <port>/<proto> [alias...]
 *
 * AmiTCP's netdb / netdb-myhost put a HOST keyword in front of the address in
 * the hosts file. That is accepted too, and cannot collide: a standard hosts
 * line always starts with a dotted-quad address and "HOST" is not one. Where
 * the two formats disagree the standard /etc shape wins. AmiTCP's
 * NAMESERVER/DOMAIN lines in the same file are not netdb entries and are
 * skipped here. ami_config_load() feeds them to the resolver instead.
 *
 * Each file is read once into one buffer and tokenised in place, so an entry's
 * strings point straight into that buffer, three allocations per table, not
 * one per name.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

#define AMI_NETDB_MAX_TOKENS    34      /* name + value + 32 aliases */

typedef enum
{
    NETDB_HOSTS = 0,
    NETDB_NETWORKS,
    NETDB_PROTOCOLS,
    NETDB_SERVICES
} NetdbKind;

typedef struct NetdbTable
{
    AmiNetdbEntry *entries;
    ULONG          count;
    char          *buffer;      /* file text, tokenised in place */
    const char   **alias_pool;  /* every alias vector, back to back */
} NetdbTable;

static NetdbTable ami_netdb[4];
static BOOL       ami_netdb_loaded;

/*
 * Used when a file is missing entirely, so that "localhost" and
 * getprotobyname("tcp") still work on a half-installed system. Parsed by the
 * same code as the real files.
 */
static const char ami_netdb_builtin_hosts[] =
    "127.0.0.1 localhost loopback\n";

static const char ami_netdb_builtin_networks[] =
    "loopback 127\n";

static const char ami_netdb_builtin_protocols[] =
    "ip 0 IP\n"
    "icmp 1 ICMP\n"
    "igmp 2 IGMP\n"
    "tcp 6 TCP\n"
    "udp 17 UDP\n"
    "ipv6 41 IPV6\n"
    "icmpv6 58 ICMPV6\n";

static const char ami_netdb_builtin_services[] =
    "echo 7/tcp\n"      "echo 7/udp\n"
    "discard 9/tcp\n"   "discard 9/udp\n"
    "daytime 13/tcp\n"  "daytime 13/udp\n"
    "ftp-data 20/tcp\n" "ftp 21/tcp\n"
    "ssh 22/tcp\n"      "telnet 23/tcp\n"
    "smtp 25/tcp mail\n"
    "time 37/tcp timserver\n"    "time 37/udp timserver\n"
    "domain 53/tcp nameserver\n" "domain 53/udp nameserver\n"
    "bootps 67/udp\n"   "bootpc 68/udp\n"
    "tftp 69/udp\n"
    "finger 79/tcp\n"   "http 80/tcp www\n"
    "pop3 110/tcp\n"    "sunrpc 111/tcp\n"
    "nntp 119/tcp\n"    "ntp 123/udp\n"
    "imap 143/tcp\n"    "snmp 161/udp\n"
    "https 443/tcp\n"   "syslog 514/udp\n"
    "printer 515/tcp\n";

/* ------------------------------------------------------------------ sizing */

/*
 * Non-destructive first pass: an upper bound on the number of lines and of
 * whitespace-separated tokens. Overshooting costs a few hundred bytes and
 * saves parsing the file twice.
 *
 * It is only an estimate, so netdb_parse() below bounds every write against
 * what was allocated. This counts a run of non-space characters as one token
 * where ami_cfg_tokenize() does not. scan_item() in config_text.c returns an
 * empty, non-NULL token for every `""` pair, so the 64-character run
 * `""""..""` is one token here and thirty-two there. The line
 *
 *     1.2.3.4 host """"""""""""""""""""""""""""""""
 *
 * in DEVS:Internet/hosts wrote twenty-nine pointers past the end of
 * alias_pool. Repeating it scattered the file's whole size in stray pointers
 * over the heap, silently, on a machine with no MMU. Found by the host
 * sanitizer fuzz driver.
 */
static VOID netdb_measure(const char *buf, ULONG *lines, ULONG *tokens)
{
    BOOL in_token = FALSE;

    *lines  = 1;
    *tokens = 0;

    for (; *buf != '\0'; buf++)
    {
        if (*buf == '\n' || *buf == '\r')
        {
            (*lines)++;
            in_token = FALSE;
        }
        else if (*buf == ' ' || *buf == '\t')
        {
            in_token = FALSE;
        }
        else if (!in_token)
        {
            in_token = TRUE;
            (*tokens)++;
        }
    }
}

/* -------------------------------------------------------------- one table */

/*
 * Copy one entry's aliases into the pool and terminate the vector, or return
 * FALSE when the pool cannot hold them. Every alias write in this file goes
 * through here, so a wrong estimate from the sizing pass above can never reach
 * a pointer store.
 */
static BOOL netdb_aliases(NetdbTable *table, ULONG pool_size, ULONG *alias_pos,
                          AmiNetdbEntry *entry, char **row, ULONG count,
                          ULONG first_alias)
{
    ULONG have = (count > first_alias) ? (count - first_alias) : 0;
    ULONG i;

    /* The aliases plus the NULL that ends the vector. */
    if (have + 1 > pool_size - *alias_pos)
        return FALSE;

    for (i = 0; i < have; i++)
        table->alias_pool[*alias_pos + i] = row[first_alias + i];

    entry->aliases = &table->alias_pool[*alias_pos];
    *alias_pos += have;
    table->alias_pool[(*alias_pos)++] = NULL;

    return TRUE;
}

static BOOL netdb_parse(NetdbTable *table, NetdbKind kind, char *buf)
{
    ULONG  max_lines;
    ULONG  max_tokens;
    ULONG  pool_size;
    ULONG  alias_pos = 0;
    char  *cursor    = buf;
    char  *line;

    netdb_measure(buf, &max_lines, &max_tokens);

    pool_size = max_tokens + max_lines + 1;

    table->entries = (AmiNetdbEntry *)ami_alloc(max_lines * sizeof(AmiNetdbEntry));
    /* Every entry needs a NULL terminator, hence + max_lines. */
    table->alias_pool = (const char **)ami_alloc(pool_size * sizeof(char *));

    if (table->entries == NULL || table->alias_pool == NULL)
    {
        ami_free(table->entries);
        ami_free((APTR)table->alias_pool);
        table->entries    = NULL;
        table->alias_pool = NULL;
        return FALSE;
    }

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char          *tokens[AMI_NETDB_MAX_TOKENS];
        ULONG          count;
        ULONG          first_alias;
        AmiNetdbEntry *entry;

        /* '#' can start a comment anywhere on the line. ';' is not a comment
         * character in /etc-format files (it can appear in a name), so it is
         * left alone. */
        ami_cfg_strip_comment(line, "#");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        count = ami_cfg_tokenize(line, tokens, AMI_NETDB_MAX_TOKENS);
        if (count < 2)
            continue;

        /* Same rule as the pool: the estimate is checked, not trusted. */
        if (table->count >= max_lines)
            break;

        entry = &table->entries[table->count];
        ami_cfg_zero(entry, sizeof(*entry));

        if (kind == NETDB_HOSTS)
        {
            ULONG  addr;
            char **row = tokens;

            /* AmiTCP netdb-myhost: HOST <address> <name> [alias...]. */
            if (ami_cfg_stricmp(row[0], "host") == 0 && count >= 3 &&
                ami_config_parse_ip(row[1], &addr))
            {
                row++;
                count--;
            }
            else if (!ami_config_parse_ip(row[0], &addr))
            {
                /* NAMESERVER/DOMAIN/SEARCH lines land here, and are not netdb
                   data. */
                continue;
            }

            entry->name  = row[1];
            entry->value = addr;
            first_alias  = 2;

            if (!netdb_aliases(table, pool_size, &alias_pos, entry, row,
                               count, first_alias))
                break;

            table->count++;
            continue;
        }

        switch (kind)
        {
        case NETDB_NETWORKS:
            if (!ami_cfg_parse_net_number(tokens[1], &entry->value))
                continue;
            break;

        case NETDB_PROTOCOLS:
            /* The field is the 8-bit IPv4/IPv6 next-header number. Keeping
               larger values makes getprotobyname() return a protocol that no
               socket or packet can carry (and values above LONG_MAX emerge
               through struct protoent as negative numbers). */
            if (!ami_cfg_parse_ulong(tokens[1], &entry->value) ||
                entry->value > 255UL)
                continue;
            break;

        case NETDB_SERVICES:
        {
            char *slash = tokens[1];

            while (*slash != '\0' && *slash != '/')
                slash++;

            if (*slash == '/')
            {
                *slash      = '\0';
                entry->proto = slash + 1;
            }

            if (!ami_cfg_parse_ulong(tokens[1], &entry->value) ||
                entry->value > 65535UL)
                continue;
            break;
        }

        default:
            continue;
        }

        entry->name = tokens[0];
        first_alias = 2;

        if (!netdb_aliases(table, pool_size, &alias_pos, entry, tokens,
                           count, first_alias))
            break;

        table->count++;
    }

    table->buffer = buf;

    return TRUE;
}

static VOID netdb_load_one(NetdbTable *table, NetdbKind kind,
                           const char *path, const char *builtin)
{
    char  *buf;
    ULONG  size = 0;

    buf = (char *)ami_cfg_read_file(path, &size);

    if (buf == NULL || size == 0)
    {
        ULONG len = ami_cfg_strlen(builtin);

        ami_free(buf);
        AMI_DEBUG("netdb: %s missing, using built-in defaults", path);

        buf = (char *)ami_alloc(len + 1);
        if (buf == NULL)
            return;
        ami_cfg_copy_string(buf, len + 1, builtin);
    }

    if (!netdb_parse(table, kind, buf))
    {
        ami_free(buf);
        return;
    }

    AMI_DEBUG("netdb: %s: %lu entries", path, (unsigned long)table->count);
}

static VOID netdb_free_one(NetdbTable *table)
{
    ami_free(table->entries);
    ami_free((APTR)table->alias_pool);
    ami_free(table->buffer);
    ami_cfg_zero(table, sizeof(*table));
}

/* ------------------------------------------------------------------- API */

LONG ami_netdb_load(VOID)
{
    if (ami_netdb_loaded)
        return AMI_CFG_OK;

    ami_netdb_loaded = TRUE;

    netdb_load_one(&ami_netdb[NETDB_HOSTS],     NETDB_HOSTS,
                   AMI_CFG_FILE_HOSTS,     ami_netdb_builtin_hosts);
    netdb_load_one(&ami_netdb[NETDB_NETWORKS],  NETDB_NETWORKS,
                   AMI_CFG_FILE_NETWORKS,  ami_netdb_builtin_networks);
    netdb_load_one(&ami_netdb[NETDB_PROTOCOLS], NETDB_PROTOCOLS,
                   AMI_CFG_FILE_PROTOCOLS, ami_netdb_builtin_protocols);
    netdb_load_one(&ami_netdb[NETDB_SERVICES],  NETDB_SERVICES,
                   AMI_CFG_FILE_SERVICES,  ami_netdb_builtin_services);

    return AMI_CFG_OK;
}

VOID ami_netdb_free(VOID)
{
    int i;

    for (i = 0; i < 4; i++)
        netdb_free_one(&ami_netdb[i]);

    ami_netdb_loaded = FALSE;
}

/*
 * Lookups load on demand. bsdsocket.library must still call ami_netdb_load()
 * from its init path. The load is not re-entrant, and one load up front keeps
 * every later lookup read-only and lock-free.
 */
static const NetdbTable *netdb_table(NetdbKind kind)
{
    if (!ami_netdb_loaded)
        ami_netdb_load();

    return &ami_netdb[kind];
}

static BOOL netdb_name_matches(const AmiNetdbEntry *entry, const char *name)
{
    const char **alias;

    if (entry->name != NULL && ami_cfg_stricmp(entry->name, name) == 0)
        return TRUE;

    for (alias = entry->aliases; alias != NULL && *alias != NULL; alias++)
    {
        if (ami_cfg_stricmp(*alias, name) == 0)
            return TRUE;
    }

    return FALSE;
}

static const AmiNetdbEntry *netdb_by_name(NetdbKind kind, const char *name)
{
    const NetdbTable *table = netdb_table(kind);
    ULONG             i;

    if (name == NULL || *name == '\0')
        return NULL;

    for (i = 0; i < table->count; i++)
    {
        if (netdb_name_matches(&table->entries[i], name))
            return &table->entries[i];
    }

    return NULL;
}

static const AmiNetdbEntry *netdb_by_value(NetdbKind kind, ULONG value)
{
    const NetdbTable *table = netdb_table(kind);
    ULONG             i;

    for (i = 0; i < table->count; i++)
    {
        if (table->entries[i].value == value)
            return &table->entries[i];
    }

    return NULL;
}

static const AmiNetdbEntry *netdb_entry(NetdbKind kind, ULONG index)
{
    const NetdbTable *table = netdb_table(kind);

    if (index >= table->count)
        return NULL;

    return &table->entries[index];
}

const AmiNetdbEntry *ami_netdb_host_by_name(const char *name)
{
    return netdb_by_name(NETDB_HOSTS, name);
}

const AmiNetdbEntry *ami_netdb_host_by_addr(ULONG addr)
{
    return netdb_by_value(NETDB_HOSTS, addr);
}

const AmiNetdbEntry *ami_netdb_net_by_name(const char *name)
{
    return netdb_by_name(NETDB_NETWORKS, name);
}

const AmiNetdbEntry *ami_netdb_net_by_addr(ULONG net)
{
    return netdb_by_value(NETDB_NETWORKS, net);
}

const AmiNetdbEntry *ami_netdb_proto_by_name(const char *name)
{
    return netdb_by_name(NETDB_PROTOCOLS, name);
}

const AmiNetdbEntry *ami_netdb_proto_by_number(LONG number)
{
    return netdb_by_value(NETDB_PROTOCOLS, (ULONG)number);
}

/* A NULL protocol means "the first match", as in BSD getservbyname(). */
static BOOL serv_proto_matches(const AmiNetdbEntry *entry, const char *proto)
{
    if (proto == NULL || *proto == '\0')
        return TRUE;
    if (entry->proto == NULL)
        return FALSE;

    return (BOOL)(ami_cfg_stricmp(entry->proto, proto) == 0);
}

const AmiNetdbEntry *ami_netdb_serv_by_name(const char *name, const char *proto)
{
    const NetdbTable *table = netdb_table(NETDB_SERVICES);
    ULONG             i;

    if (name == NULL || *name == '\0')
        return NULL;

    for (i = 0; i < table->count; i++)
    {
        const AmiNetdbEntry *entry = &table->entries[i];

        if (serv_proto_matches(entry, proto) && netdb_name_matches(entry, name))
            return entry;
    }

    return NULL;
}

const AmiNetdbEntry *ami_netdb_serv_by_port(LONG port, const char *proto)
{
    const NetdbTable *table = netdb_table(NETDB_SERVICES);
    ULONG             i;

    for (i = 0; i < table->count; i++)
    {
        const AmiNetdbEntry *entry = &table->entries[i];

        if (entry->value == (ULONG)port && serv_proto_matches(entry, proto))
            return entry;
    }

    return NULL;
}

const AmiNetdbEntry *ami_netdb_net_entry(ULONG index)
{
    return netdb_entry(NETDB_NETWORKS, index);
}

const AmiNetdbEntry *ami_netdb_proto_entry(ULONG index)
{
    return netdb_entry(NETDB_PROTOCOLS, index);
}

const AmiNetdbEntry *ami_netdb_serv_entry(ULONG index)
{
    return netdb_entry(NETDB_SERVICES, index);
}
