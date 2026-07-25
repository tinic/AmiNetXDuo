/*
 * AmiNetXDuo -- Roadshow configuration file parsers.
 *
 * Formats implemented here, per the Roadshow manual (Olaf Barthel, 1.15,
 * 4 September 2023), sections 7.1.1 (DEVS:NetInterfaces), 7.1.2.3
 * (DEVS:Internet/name_resolution) and 7.1.2.6 (DEVS:Internet/routes):
 *
 *   - one `keyword=value` per line, whitespace around either side ignored;
 *   - lines starting with ';' or '#' are comments (we also drop a comment
 *     that starts mid-line, which Roadshow does not, because hand-written
 *     files in the wild do it and it can never make a valid file invalid);
 *   - keywords are case-insensitive (ReadArgs templates);
 *   - "quoted values" use the AmigaDOS '*' escape.
 *
 * Deviations from the manual, all deliberate and all additive:
 *
 *   - Roadshow's IPTYPE is the SANA-II packet type number (default 2048), NOT
 *     an address-configuration mode. AmiTCP/Genesis-era documentation and
 *     several config generators use `IPTYPE=DHCP`/`STATIC` instead. Both are
 *     accepted: a numeric IPTYPE is the packet type, an alphabetic one is the
 *     address mode. This is unambiguous, so nothing is lost either way.
 *   - GATEWAY= inside an interface file is not a Roadshow keyword (the real
 *     stack puts the default route in DEVS:Internet/routes) but AmiTCP_NG
 *     writes it, and AmiIfConfig has the field, so it is accepted.
 *   - DEVS:Internet/default_gateway does not exist in Roadshow 1.15 either;
 *     docs/RESEARCH.md and the config.h contract call for it, so it is read
 *     first and DEVS:Internet/routes is read after it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/compat.h"

/* ------------------------------------------------------- interface files */

typedef enum
{
    IF_KEY_UNKNOWN = 0,
    IF_KEY_IGNORED,          /* real Roadshow keyword with no AmiIfConfig field */
    IF_KEY_DEVICE,
    IF_KEY_UNIT,
    IF_KEY_ADDRESS,
    IF_KEY_NETMASK,
    IF_KEY_GATEWAY,
    IF_KEY_MTU,
    IF_KEY_CONFIGURE,
    IF_KEY_IPTYPE,
    IF_KEY_STATE
} IfKey;

static const struct IfKeyword
{
    const char *name;
    IfKey       key;
}
ami_if_keywords[] =
{
    /* Keywords that map onto AmiIfConfig. */
    { "device",             IF_KEY_DEVICE    },
    { "unit",               IF_KEY_UNIT      },
    { "address",            IF_KEY_ADDRESS   },
    { "ipaddress",          IF_KEY_ADDRESS   },   /* AmiTCP spelling */
    { "netmask",            IF_KEY_NETMASK   },
    { "subnetmask",         IF_KEY_NETMASK   },   /* AmiTCP spelling */
    { "gateway",            IF_KEY_GATEWAY   },
    { "mtu",                IF_KEY_MTU       },
    { "configure",          IF_KEY_CONFIGURE },
    { "iptype",             IF_KEY_IPTYPE    },
    { "state",              IF_KEY_STATE     },

    /*
     * Roadshow keywords we parse but have nowhere to put: they belong to the
     * SANA-II shim (§6.5) rather than to the IP configuration. Listed so that
     * a stock config file produces no warnings.
     */
    { "arptype",            IF_KEY_IGNORED   },
    { "iprequests",         IF_KEY_IGNORED   },
    { "writerequests",      IF_KEY_IGNORED   },
    { "arprequests",        IF_KEY_IGNORED   },
    { "debug",              IF_KEY_IGNORED   },
    { "pointtopoint",       IF_KEY_IGNORED   },
    { "multicast",          IF_KEY_IGNORED   },
    { "downgoesoffline",    IF_KEY_IGNORED   },
    { "reportoffline",      IF_KEY_IGNORED   },
    { "requiresinitdelay",  IF_KEY_IGNORED   },
    { "copymode",           IF_KEY_IGNORED   },
    { "filter",             IF_KEY_IGNORED   },
    { "hardwareaddress",    IF_KEY_IGNORED   },
    { "alias",              IF_KEY_IGNORED   },
    { "destination",        IF_KEY_IGNORED   },
    { "destinationaddr",    IF_KEY_IGNORED   },
    { "metric",             IF_KEY_IGNORED   },
    { "lease",              IF_KEY_IGNORED   },
    { "id",                 IF_KEY_IGNORED   },
    { "dhcpunicast",        IF_KEY_IGNORED   },
    { "linkstatuscommand",  IF_KEY_IGNORED   },
    { "priority",           IF_KEY_IGNORED   },
    { "pri",                IF_KEY_IGNORED   },

    /* Written by AmiTCP_NG's installer; harmless, handled elsewhere. */
    { "nameserver",         IF_KEY_IGNORED   },
    { "domain",             IF_KEY_IGNORED   },

    { NULL,                 IF_KEY_UNKNOWN   }
};

static IfKey lookup_if_keyword(const char *name)
{
    const struct IfKeyword *k;

    for (k = ami_if_keywords; k->name != NULL; k++)
    {
        if (ami_cfg_stricmp(name, k->name) == 0)
            return k->key;
    }

    return IF_KEY_UNKNOWN;
}

/* CONFIGURE=/IPTYPE= address-configuration modes. */
static const struct IpTypeName
{
    const char *name;
    AmiIpType   type;
}
ami_iptype_names[] =
{
    { "dhcp",     AMI_IPTYPE_DHCP      },
    { "bootp",    AMI_IPTYPE_DHCP      },   /* AmiTCP spelling; DHCP supersedes it */
    { "auto",     AMI_IPTYPE_LINKLOCAL },
    { "fastauto", AMI_IPTYPE_LINKLOCAL },
    { "zeroconf", AMI_IPTYPE_LINKLOCAL },
    { "linklocal",AMI_IPTYPE_LINKLOCAL },
    { "static",   AMI_IPTYPE_STATIC    },
    { "manual",   AMI_IPTYPE_STATIC    },
    { "none",     AMI_IPTYPE_STATIC    },
    { NULL,       AMI_IPTYPE_STATIC    }
};

static BOOL lookup_iptype(const char *value, AmiIpType *out)
{
    const struct IpTypeName *n;

    for (n = ami_iptype_names; n->name != NULL; n++)
    {
        if (ami_cfg_stricmp(value, n->name) == 0)
        {
            *out = n->type;
            return TRUE;
        }
    }

    return FALSE;
}

LONG ami_cfg_parse_interface(const char *name, char *buf, AmiIfConfig *out)
{
    char *cursor = buf;
    char *line;
    BOOL  have_device = FALSE;

    if (out == NULL)
        return AMI_CFG_ERR_SYNTAX;

    ami_cfg_zero(out, sizeof(*out));
    out->up     = TRUE;                 /* Roadshow's STATE default is "up" */
    out->iptype = AMI_IPTYPE_STATIC;

    if (name != NULL)
    {
        char short_name[AMI_CFG_IFNAME_MAX + 1];

        /* Roadshow truncates the interface name to 15 characters. */
        ami_cfg_copy_string(short_name, sizeof(short_name), name);
        if (ami_cfg_strlen(name) > AMI_CFG_IFNAME_MAX)
            AMI_WARN("config: interface name '%s' truncated to '%s'", name, short_name);
        ami_cfg_copy_string(out->name, sizeof(out->name), short_name);
    }

    if (buf == NULL)
        return AMI_CFG_ERR_SYNTAX;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;

        ami_cfg_strip_comment(line, "#;");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        pos = line;
        while (ami_cfg_next_pair(&pos, &key, &value))
        {
            AmiIpType type;
            ULONG     n;

            switch (lookup_if_keyword(key))
            {
            case IF_KEY_DEVICE:
                if (*value == '\0')
                {
                    AMI_WARN("config: %s: empty DEVICE", out->name);
                    break;
                }
                ami_cfg_copy_string(out->device, sizeof(out->device), value);
                have_device = TRUE;
                break;

            case IF_KEY_UNIT:
                if (ami_cfg_parse_ulong(value, &n))
                    out->unit = n;
                else
                    AMI_WARN("config: %s: bad UNIT '%s'", out->name, value);
                break;

            case IF_KEY_ADDRESS:
                /* "address=dhcp" is legal Roadshow and means "ask the server". */
                if (lookup_iptype(value, &type))
                    out->iptype = type;
                else if (!ami_config_parse_ip(value, &out->address))
                    AMI_WARN("config: %s: bad ADDRESS '%s'", out->name, value);
                break;

            case IF_KEY_NETMASK:
                if (lookup_iptype(value, &type))
                    out->iptype = type;
                else if (!ami_config_parse_ip(value, &out->netmask))
                    AMI_WARN("config: %s: bad NETMASK '%s'", out->name, value);
                break;

            case IF_KEY_GATEWAY:
                if (!ami_config_parse_ip(value, &out->gateway))
                    AMI_WARN("config: %s: bad GATEWAY '%s'", out->name, value);
                break;

            case IF_KEY_MTU:
                if (ami_cfg_parse_ulong(value, &n))
                    out->mtu = n;
                else
                    AMI_WARN("config: %s: bad MTU '%s'", out->name, value);
                break;

            case IF_KEY_CONFIGURE:
                if (lookup_iptype(value, &type))
                    out->iptype = type;
                else
                    AMI_WARN("config: %s: bad CONFIGURE '%s'", out->name, value);
                break;

            case IF_KEY_IPTYPE:
                /*
                 * Numeric: the SANA-II packet type (2048 for Ethernet IPv4),
                 * which the SANA-II shim owns. Alphabetic: the AmiTCP-style
                 * address mode.
                 */
                if (ami_cfg_parse_ulong(value, &n))
                    AMI_DEBUG("config: %s: SANA-II IPTYPE %lu (sana2 layer)",
                              out->name, (unsigned long)n);
                else if (lookup_iptype(value, &type))
                    out->iptype = type;
                else
                    AMI_WARN("config: %s: bad IPTYPE '%s'", out->name, value);
                break;

            case IF_KEY_STATE:
                if (ami_cfg_stricmp(value, "up") == 0 ||
                    ami_cfg_stricmp(value, "online") == 0)
                    out->up = TRUE;
                else if (ami_cfg_stricmp(value, "down") == 0 ||
                         ami_cfg_stricmp(value, "offline") == 0)
                    out->up = FALSE;
                else
                    AMI_WARN("config: %s: bad STATE '%s'", out->name, value);
                break;

            case IF_KEY_IGNORED:
                AMI_DEBUG("config: %s: ignoring %s=%s", out->name, key, value);
                break;

            case IF_KEY_UNKNOWN:
            default:
                AMI_WARN("config: %s: unknown keyword '%s'", out->name, key);
                break;
            }
        }
    }

    if (!have_device)
    {
        AMI_WARN("config: %s: no DEVICE keyword, ignoring interface", out->name);
        return AMI_CFG_ERR_SYNTAX;
    }

    if (out->iptype == AMI_IPTYPE_STATIC && out->address == 0)
        AMI_WARN("config: %s: static but no ADDRESS", out->name);

    out->configured = TRUE;

    return AMI_CFG_OK;
}

/* ------------------------------------------------------- name_resolution */

/*
 * Roadshow's name_resolution is resolv.conf-shaped: `keyword value`, one per
 * line, '#' comments. We also accept `keyword=value` and ';' comments, and we
 * accept AmiTCP netdb-myhost lines (HOST <addr> <name> [alias...]) so that the
 * same routine can be pointed at an AmiTCP database.
 */
VOID ami_cfg_parse_resolver(char *buf, AmiResolverConfig *out,
                            char *hostname, ULONG hostname_len)
{
    char *cursor = buf;
    char *line;

    if (buf == NULL || out == NULL)
        return;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;

        ami_cfg_strip_comment(line, "#;");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        pos = line;
        if (!ami_cfg_next_pair(&pos, &key, &value))
            continue;

        if (ami_cfg_stricmp(key, "nameserver") == 0)
        {
            ULONG addr;

            if (!ami_config_parse_ip(value, &addr))
            {
                AMI_WARN("config: name_resolution: bad NAMESERVER '%s'", value);
            }
            else if (out->nameserver_count >= AMI_CFG_MAX_NAMESERVERS)
            {
                AMI_WARN("config: more than %ld name servers, ignoring '%s'",
                         (long)AMI_CFG_MAX_NAMESERVERS, value);
            }
            else
            {
                out->nameserver[out->nameserver_count++] = addr;
            }
        }
        else if (ami_cfg_stricmp(key, "domain") == 0)
        {
            ami_cfg_copy_string(out->domain, sizeof(out->domain), value);
        }
        else if (ami_cfg_stricmp(key, "search") == 0)
        {
            /* SEARCH takes a whitespace-separated list on one line. */
            char *tokens[AMI_CFG_MAX_SEARCH];
            ULONG count;
            ULONG i;

            out->search_count = 0;
            if (*value != '\0')
                ami_cfg_copy_string(out->search[out->search_count++],
                                    AMI_CFG_NAME_LEN, value);

            count = ami_cfg_tokenize(pos, tokens, AMI_CFG_MAX_SEARCH);
            for (i = 0; i < count; i++)
            {
                if (out->search_count >= AMI_CFG_MAX_SEARCH)
                {
                    AMI_WARN("config: more than %ld search domains",
                             (long)AMI_CFG_MAX_SEARCH);
                    break;
                }
                ami_cfg_copy_string(out->search[out->search_count++],
                                    AMI_CFG_NAME_LEN, tokens[i]);
            }
            continue;   /* the rest of the line has been consumed */
        }
        else if (ami_cfg_stricmp(key, "prefer") == 0)
        {
            /* static|dynamic: whether DHCP servers override these entries. */
            AMI_DEBUG("config: name_resolution: PREFER=%s", value);
        }
        else if (ami_cfg_stricmp(key, "host") == 0)
        {
            /*
             * AmiTCP netdb-myhost: HOST <address> <name> [alias...]. The first
             * non-loopback entry names this machine.
             */
            ULONG addr;
            char *name;
            char *dummy;

            if (!ami_config_parse_ip(value, &addr))
                continue;
            if (!ami_cfg_next_pair(&pos, &name, &dummy))
                continue;

            (VOID)dummy;
            if (hostname != NULL && hostname[0] == '\0' &&
                (addr >> 24) != 127UL && addr != 0)
                ami_cfg_copy_string(hostname, hostname_len, name);
        }
        else if (ami_cfg_stricmp(key, "hostname") == 0)
        {
            if (hostname != NULL)
                ami_cfg_copy_string(hostname, hostname_len, value);
        }
        else if (ami_config_parse_ip(key, NULL))
        {
            /*
             * A standard hosts-file line. ami_config_load() points this parser
             * at DEVS:Internet/hosts as well, to pick up the NAMESERVER and
             * DOMAIN lines an AmiTCP netdb keeps there; the host entries
             * themselves belong to netdb.c.
             */
            AMI_TRACE("config: skipping hosts entry for '%s'", key);
        }
        else
        {
            AMI_WARN("config: name_resolution: unknown keyword '%s'", key);
        }
    }
}

/* -------------------------------------------------- default_gateway/routes */

VOID ami_cfg_parse_gateway(char *buf, ULONG *out)
{
    char *cursor = buf;
    char *line;

    if (buf == NULL || out == NULL)
        return;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;
        ULONG gateway    = 0;
        BOOL  have_gw    = FALSE;
        BOOL  have_dst   = FALSE;
        BOOL  is_default = FALSE;

        ami_cfg_strip_comment(line, "#;");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        pos = line;
        while (ami_cfg_next_pair(&pos, &key, &value))
        {
            if (ami_cfg_stricmp(key, "gateway") == 0 ||
                ami_cfg_stricmp(key, "via") == 0)
            {
                if (ami_config_parse_ip(value, &gateway))
                    have_gw = TRUE;
                else
                    AMI_WARN("config: bad gateway address '%s'", value);
            }
            else if (ami_cfg_stricmp(key, "default") == 0 ||
                     ami_cfg_stricmp(key, "defaultgateway") == 0)
            {
                is_default = TRUE;
                /* Roadshow's routes file: DEFAULT=<address>. */
                if (*value != '\0' && ami_config_parse_ip(value, &gateway))
                    have_gw = TRUE;
            }
            else if (ami_cfg_stricmp(key, "device") == 0 ||
                     ami_cfg_stricmp(key, "unit") == 0)
            {
                /*
                 * default_gateway names the interface the route belongs to.
                 * AmiConfig has one global default gateway, so the binding is
                 * left to the routing layer.
                 */
                AMI_DEBUG("config: default_gateway: %s=%s", key, value);
            }
            else if (ami_cfg_stricmp(key, "dst") == 0 ||
                     ami_cfg_stricmp(key, "destination") == 0 ||
                     ami_cfg_stricmp(key, "hostdst") == 0 ||
                     ami_cfg_stricmp(key, "hostdestination") == 0 ||
                     ami_cfg_stricmp(key, "netdst") == 0 ||
                     ami_cfg_stricmp(key, "netdestination") == 0)
            {
                /* A specific route, not the default one. */
                have_dst = TRUE;
                AMI_DEBUG("config: routes: skipping %s=%s", key, value);
            }
            else
            {
                AMI_WARN("config: routes: unknown keyword '%s'", key);
            }
        }

        /*
         * DEVS:Internet/default_gateway has no DEFAULT keyword: the GATEWAY in
         * it is the default route. In DEVS:Internet/routes a line is the
         * default route only when it carries DEFAULT= -- a line with a
         * DST/HOSTDST/NETDST destination is a specific route we do not keep.
         */
        if (have_dst && !is_default)
            continue;

        if (have_gw && *out == 0)
            *out = gateway;
    }
}
