/*
 * AmiNetXDuo, Roadshow configuration file parsers.
 *
 * Per the Roadshow manual 1.15 sections 7.1.1, 7.1.2.3 and 7.1.2.6:
 * `keyword=value` per line, ';' and '#' comments, case-insensitive keywords,
 * AmigaDOS '*' escapes in quoted values.
 *
 * SPDX-License-Identifier: MIT
 */

#include "config_internal.h"
#include "aminetxduo/anxnet.h"
#include "aminetxduo/compat.h"

/* ------------------------------------------------------- interface files */

typedef enum
{
    IF_KEY_UNKNOWN = 0,
    IF_KEY_IGNORED,          /* real Roadshow keyword with no AmiIfConfig field */
    IF_KEY_DEVICE,
    IF_KEY_CARD,
    IF_KEY_ID,
    IF_KEY_UNIT,
    IF_KEY_ADDRESS,
    IF_KEY_NETMASK,
    IF_KEY_GATEWAY,
    IF_KEY_MTU,
    IF_KEY_CONFIGURE,
    IF_KEY_IPTYPE,
    IF_KEY_STATE,
    IF_KEY_ADDRESS6,
    IF_KEY_GATEWAY6,
    IF_KEY_CONFIGURE6,
    IF_KEY_MDNS,
    IF_KEY_DOWNGOESOFFLINE,
    IF_KEY_REQUIRESINITDELAY,
    IF_KEY_HARDWAREADDRESS
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
    { "card",               IF_KEY_CARD      },
    { "id",                 IF_KEY_ID        },
    { "unit",               IF_KEY_UNIT      },
    { "mdns",               IF_KEY_MDNS      },
    { "address",            IF_KEY_ADDRESS   },
    { "ipaddress",          IF_KEY_ADDRESS   },   /* AmiTCP spelling */
    { "netmask",            IF_KEY_NETMASK   },
    { "subnetmask",         IF_KEY_NETMASK   },   /* AmiTCP spelling */
    { "gateway",            IF_KEY_GATEWAY   },
    { "mtu",                IF_KEY_MTU       },
    { "configure",          IF_KEY_CONFIGURE },
    { "iptype",             IF_KEY_IPTYPE    },
    { "state",              IF_KEY_STATE     },
    { "downgoesoffline",    IF_KEY_DOWNGOESOFFLINE   },
    { "requiresinitdelay",  IF_KEY_REQUIRESINITDELAY },
    { "hardwareaddress",    IF_KEY_HARDWAREADDRESS   },

    /* IPv6 keywords: the IPv4 keyword plus a "6".  In the floor build (no
       AMINETXDUO_IPV6) they must stay RECOGNISED and be ignored, so the same
       file loads in both builds without an "unknown keyword" warning. */
    { "address6",           IF_KEY_ADDRESS6  },
    { "ipaddress6",         IF_KEY_ADDRESS6  },
    { "gateway6",           IF_KEY_GATEWAY6  },
    { "configure6",         IF_KEY_CONFIGURE6},
    { "iptype6",            IF_KEY_CONFIGURE6},

    /* Roadshow keywords with nowhere to put them; listed so a stock
       configuration file produces no warnings. */
    { "arptype",            IF_KEY_IGNORED   },
    { "iprequests",         IF_KEY_IGNORED   },
    { "writerequests",      IF_KEY_IGNORED   },
    { "arprequests",        IF_KEY_IGNORED   },
    { "debug",              IF_KEY_IGNORED   },
    { "pointtopoint",       IF_KEY_IGNORED   },
    { "multicast",          IF_KEY_IGNORED   },
    { "reportoffline",      IF_KEY_IGNORED   },
    { "copymode",           IF_KEY_IGNORED   },
    { "filter",             IF_KEY_IGNORED   },
    { "alias",              IF_KEY_IGNORED   },
    { "destination",        IF_KEY_IGNORED   },
    { "destinationaddr",    IF_KEY_IGNORED   },
    { "destinationaddress", IF_KEY_IGNORED   },
    { "hardwaretype",       IF_KEY_IGNORED   },
    { "broadcastaddress",   IF_KEY_IGNORED   },
    { "metric",             IF_KEY_IGNORED   },
    { "lease",              IF_KEY_IGNORED   },
    { "dhcpunicast",        IF_KEY_IGNORED   },
    { "linkstatuscommand",  IF_KEY_IGNORED   },
    { "priority",           IF_KEY_IGNORED   },
    { "pri",                IF_KEY_IGNORED   },

    /* Written by AmiTCP_NG's installer. Harmless, handled elsewhere. */
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

/* "did you mean DEVICE?": Levenshtein distance over the keyword table. */
#define CFG_SUGGEST_MAX     24      /* longer than any keyword in the table */

static ULONG edit_distance(const char *a, const char *b)
{
    ULONG prev[CFG_SUGGEST_MAX + 1];
    ULONG curr[CFG_SUGGEST_MAX + 1];
    ULONG la = ami_cfg_strlen(a);
    ULONG lb = ami_cfg_strlen(b);
    ULONG i;
    ULONG j;

    if (la > CFG_SUGGEST_MAX || lb > CFG_SUGGEST_MAX)
        return CFG_SUGGEST_MAX + 1;

    for (j = 0; j <= lb; j++)
        prev[j] = j;

    for (i = 1; i <= la; i++)
    {
        char ca = a[i - 1];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));

        curr[0] = i;

        for (j = 1; j <= lb; j++)
        {
            ULONG sub = prev[j - 1] + ((ca == b[j - 1]) ? 0UL : 1UL);
            ULONG del = prev[j] + 1UL;
            ULONG ins = curr[j - 1] + 1UL;
            ULONG best = sub;

            if (del < best)
                best = del;
            if (ins < best)
                best = ins;

            curr[j] = best;
        }

        for (j = 0; j <= lb; j++)
            prev[j] = curr[j];
    }

    return prev[lb];
}

/* Uppercase, as the keywords are written in the manual. */
static VOID upcase_into(char *dst, ULONG dstlen, const char *src)
{
    ULONG i;

    if (dst == NULL || dstlen == 0)
        return;

    for (i = 0; i + 1 < dstlen && src != NULL && src[i] != '\0'; i++)
    {
        char c = src[i];

        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
    }

    dst[i] = '\0';
}

/* The nearest keyword within two edits, or NULL when nothing is close. */
static const char *suggest_if_keyword(const char *name)
{
    const struct IfKeyword *k;
    const char             *best  = NULL;
    ULONG                   bestd = 3;

    for (k = ami_if_keywords; k->name != NULL; k++)
    {
        ULONG d = edit_distance(name, k->name);

        if (d < bestd)
        {
            bestd = d;
            best  = k->name;
        }
    }

    return best;
}

static VOID report_unknown_keyword(ULONG line, const char *key,
                                   const char *known)
{
    char        text[96];
    char        hint[192];
    const char *guess;

    if (!ami_cfg_problems_wanted())
        return;

    ami_cfg_join3(text, sizeof(text), "unknown keyword '", key, "'");

    guess = suggest_if_keyword(key);
    if (guess != NULL)
    {
        char upper[CFG_SUGGEST_MAX + 1];

        upcase_into(upper, sizeof(upper), guess);
        ami_cfg_join3(hint, sizeof(hint), "The nearest keyword is ", upper,
                      ".  The line was ignored.");
    }
    else
    {
        ami_cfg_join3(hint, sizeof(hint), known, NULL, NULL);
    }

    ami_cfg_problem(line, AMI_CFG_PROBLEM_WARN, text, hint);
}

/*
 * A Roadshow keyword that is read and does nothing.  Severity must stay
 * AMI_CFG_PROBLEM_NOTE, not _WARN: nothing is wrong with the file, and only
 * CheckNetConfig prints notes (see aminetxduo/config.h).
 */
static const struct { const char *key; const char *why; } cfg_inert_keys[] =
{
    { "alias",             "a second address on one interface is not supported" },
    { "arptype",           "this stack is Ethernet only, and ARPTYPE names another link type" },
    { "hardwaretype",      "this stack is Ethernet only, and HARDWARETYPE names another link type" },
    { "broadcastaddress",  "the broadcast address is derived from ADDRESS and NETMASK" },
    { "destinationaddress","point-to-point links are not supported" },
    { "copymode",          "the copy mode is taken from what the driver reports it can do" },
    { "debug",             "the driver's own debug output is not switched from here" },
    { "destination",       "point-to-point links are not supported" },
    { "destinationaddr",   "point-to-point links are not supported" },
    { "dhcpunicast",       "DHCP renewal is always broadcast here" },
    { "filter",            "there is no packet filter to give rules to" },
    { "iprequests",        "the number of queued requests is fixed" },
    { "arprequests",       "the number of queued requests is fixed" },
    { "writerequests",     "the number of queued requests is fixed" },
    { "lease",             "the lease time asked for is the server's to choose" },
    { "linkstatuscommand", "nothing is run when the link changes" },
    { "metric",            "routes have no metric here, so interfaces cannot be ordered by one" },
    { "multicast",         "multicast is asked for when something joins a group, not from here" },
    { "pointtopoint",      "point-to-point links are not supported" },
    { "pri",               "routes have no metric here, so interfaces cannot be ordered by one" },
    { "priority",          "routes have no metric here, so interfaces cannot be ordered by one" },
    { "reportoffline",     "an interface going offline is always reported" },
    { NULL, NULL }
};

static VOID report_inert_keyword(ULONG line, const char *key)
{
    char text[160];
    LONG i;

    if (!ami_cfg_problems_wanted())
        return;

    for (i = 0; cfg_inert_keys[i].key != NULL; i++)
    {
        if (ami_cfg_stricmp(key, cfg_inert_keys[i].key) == 0)
        {
            ami_cfg_join3(text, sizeof(text), key,
                          " is read and does nothing: ",
                          cfg_inert_keys[i].why);
            ami_cfg_problem(line, AMI_CFG_PROBLEM_NOTE, text,
                            "Roadshow acts on it.  This stack does not.  "
                            "The line is harmless and can stay.");
            return;
        }
    }

    /* NAMESERVER and DOMAIN in an interface file: handled elsewhere, and not
       inert. */
}

/* "bad ADDRESS '10.0.0.300'" + whatever the keyword's own advice is. */

static VOID report_bad_value(ULONG line, UWORD severity, const char *keyword,
                             const char *value, const char *hint)
{
    char text[128];
    char quoted[96];

    if (!ami_cfg_problems_wanted())
        return;

    ami_cfg_join3(quoted, sizeof(quoted), " cannot be '", value, "'");
    ami_cfg_join3(text, sizeof(text), keyword, quoted, NULL);

    ami_cfg_problem(line, severity, text, hint);
}

/* CARD names one board by name where UNIT only says "Nth in probe order".
   The names are the driver's, include/aminetxduo/anxnet.h. */
static const char *const cfg_card_names[] = ANXNET_CARD_NAMES;

#define CFG_CARD_COUNT \
    ((ULONG)(sizeof(cfg_card_names) / sizeof(cfg_card_names[0])))

static BOOL cfg_card_known(const char *name)
{
    ULONG i;

    for (i = 0; i < CFG_CARD_COUNT; i++)
    {
        if (ami_cfg_stricmp(name, cfg_card_names[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

/* "XSURF100, XSURF, ARIADNE2, ...", built from the same list it checks. */
static VOID cfg_card_list(char *dst, ULONG dstlen)
{
    ULONG at = 0;
    ULONG i;

    if (dst == NULL || dstlen == 0)
        return;

    dst[0] = '\0';

    for (i = 0; i < CFG_CARD_COUNT; i++)
    {
        const char *name = cfg_card_names[i];
        ULONG       need = ami_cfg_strlen(name) + (at != 0 ? 2UL : 0UL);
        ULONG       j;

        if (at + need + 1 > dstlen)
            break;

        if (at != 0)
        {
            dst[at++] = ',';
            dst[at++] = ' ';
        }

        for (j = 0; name[j] != '\0'; j++)
        {
            char c = name[j];

            dst[at++] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
        }

        dst[at] = '\0';
    }
}

static VOID report_bad_card(ULONG line, const char *value)
{
    char list[128];
    char hint[224];

    if (!ami_cfg_problems_wanted())
        return;

    cfg_card_list(list, sizeof(list));
    ami_cfg_join3(hint, sizeof(hint),
                  "CARD says which board the driver binds to, one of ", list,
                  ".  Leave CARD out and UNIT decides.");
    report_bad_value(line, AMI_CFG_PROBLEM_ERROR, "CARD", value, hint);
}

#define CFG_HINT_KEYWORDS \
    "The keywords an interface file understands are DEVICE, UNIT, CONFIGURE, " \
    "ADDRESS, NETMASK, GATEWAY, MTU, and CONFIGURE6, ADDRESS6 and GATEWAY6 " \
    "for IPv6.  The line was ignored."

#define CFG_HINT_IPV4 \
    "An address is four numbers from 0 to 255 with dots between them, for " \
    "example 192.168.1.10."

/* CONFIGURE=/IPTYPE= address-configuration modes. */
static const struct IpTypeName
{
    const char *name;
    AmiIpType   type;
}
ami_iptype_names[] =
{
    { "dhcp",     AMI_IPTYPE_DHCP      },
    { "bootp",    AMI_IPTYPE_DHCP      },   /* AmiTCP spelling, DHCP supersedes it */
    { "auto",     AMI_IPTYPE_LINKLOCAL },
    { "fastauto", AMI_IPTYPE_LINKLOCAL },
    { "zeroconf", AMI_IPTYPE_LINKLOCAL },
    { "linklocal",AMI_IPTYPE_LINKLOCAL },
    { "static",   AMI_IPTYPE_STATIC    },
    { "manual",   AMI_IPTYPE_STATIC    },
    { "none",     AMI_IPTYPE_NONE      },
    { "off",      AMI_IPTYPE_NONE      },
    { "no",       AMI_IPTYPE_NONE      },
    { "disabled", AMI_IPTYPE_NONE      },
    { NULL,       AMI_IPTYPE_STATIC    }
};

#ifdef AMINETXDUO_IPV6

/* CONFIGURE6=/IPTYPE6= address-configuration modes. */
static const struct Ip6TypeName
{
    const char *name;
    AmiIp6Type  type;
}
ami_ip6type_names[] =
{
    { "off",        AMI_IP6TYPE_OFF       },
    { "no",         AMI_IP6TYPE_OFF       },
    { "none",       AMI_IP6TYPE_OFF       },
    { "disabled",   AMI_IP6TYPE_OFF       },
    { "linklocal",  AMI_IP6TYPE_LINKLOCAL },
    { "link-local", AMI_IP6TYPE_LINKLOCAL },
    { "local",      AMI_IP6TYPE_LINKLOCAL },
    { "auto",       AMI_IP6TYPE_AUTO      },
    { "slaac",      AMI_IP6TYPE_AUTO      },
    { "stateless",  AMI_IP6TYPE_AUTO      },
    { "ra",         AMI_IP6TYPE_AUTO      },
    { "static",     AMI_IP6TYPE_STATIC    },
    { "manual",     AMI_IP6TYPE_STATIC    },
    { "dhcp",       AMI_IP6TYPE_DHCP      },
    { "dhcpv6",     AMI_IP6TYPE_DHCP      },
    { "stateful",   AMI_IP6TYPE_DHCP      },
    { NULL,         AMI_IP6TYPE_OFF       }
};

static BOOL lookup_ip6type(const char *value, AmiIp6Type *out)
{
    const struct Ip6TypeName *n;

    for (n = ami_ip6type_names; n->name != NULL; n++)
    {
        if (ami_cfg_stricmp(value, n->name) == 0)
        {
            *out = n->type;
            return TRUE;
        }
    }

    return FALSE;
}

#endif /* AMINETXDUO_IPV6 */


#ifdef AMINETXDUO_IPV6
/*
 * RFC 4007 11 "%zone" in an interface file.  Nothing is stored -- the file
 * name already says the zone.  TRUE to use the address, FALSE to reject.
 */
static BOOL cfg_zone_ok(const AmiIfConfig *out, const char *key,
                        const char *zone, const char *value)
{
    if (zone[0] == '\0')
        return TRUE;

    if (ami_cfg_stricmp(zone, out->name) == 0)
        return TRUE;

    AMI_WARN("config: %s: %s '%s' names interface '%s', not this one",
             out->name, key, value, zone);

    return FALSE;
}
#endif


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
    ULONG lineno = 0;
    BOOL  have_device = FALSE;
    BOOL  bad_card = FALSE;
#ifdef AMINETXDUO_IPV6
    BOOL  have_configure6 = FALSE;
#endif

    if (out == NULL)
        return AMI_CFG_ERR_SYNTAX;

    ami_cfg_zero(out, sizeof(*out));
    out->up     = TRUE;                 /* Roadshow's STATE default is "up" */
    out->iptype = AMI_IPTYPE_STATIC;

#ifdef AMINETXDUO_IPV6
    /* The IPv6 default is AUTO: link-local always, plus a global address if
       a router advertisement arrives.  CONFIGURE6=OFF turns IPv6 off. */
    out->ip6type = AMI_IP6TYPE_AUTO;
#endif

    if (name != NULL)
    {
        char short_name[AMI_CFG_IFNAME_MAX + 1];

        /* Roadshow truncates the interface name to 15 characters. */
        ami_cfg_copy_string(short_name, sizeof(short_name), name);
        if (ami_cfg_strlen(name) > AMI_CFG_IFNAME_MAX)
        {
            char text[128];

            AMI_WARN("config: interface name '%s' truncated to '%s'", name, short_name);
            ami_cfg_join3(text, sizeof(text), "the interface name is longer "
                          "than 15 characters, so it becomes '", short_name, "'");
            ami_cfg_problem(0, AMI_CFG_PROBLEM_WARN, text,
                            "Rename the file in DEVS:NetInterfaces to something "
                            "15 characters or shorter and use that name from "
                            "now on.");
        }
        ami_cfg_copy_string(out->name, sizeof(out->name), short_name);
    }

    if (buf == NULL)
        return AMI_CFG_ERR_SYNTAX;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;

        lineno++;

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
                    ami_cfg_problem(lineno, AMI_CFG_PROBLEM_ERROR,
                                    "DEVICE has no value",
                                    "DEVICE names the driver for the network "
                                    "card, for example DEVICE=a2065.device.  "
                                    "The driver itself belongs in "
                                    "DEVS:Networks/.");
                    break;
                }
                ami_cfg_copy_string(out->device, sizeof(out->device), value);
                have_device = TRUE;
                break;

            case IF_KEY_CARD:
                if (!cfg_card_known(value))
                {
                    AMI_WARN("config: %s: bad CARD '%s'", out->name, value);
                    report_bad_card(lineno, value);
                    bad_card = TRUE;
                    break;
                }
                ami_cfg_copy_string(out->card, sizeof(out->card), value);
                break;

            /* Free text, stored verbatim; ami_config_load() decides whether
               it is usable as a host name. */
            case IF_KEY_ID:
                ami_cfg_copy_string(out->id, sizeof(out->id), value);
                break;

            case IF_KEY_UNIT:
                if (ami_cfg_parse_ulong(value, &n))
                {
                    out->unit = n;
                }
                else
                {
                    AMI_WARN("config: %s: bad UNIT '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "UNIT",
                                     value,
                                     "UNIT is a plain number and is 0 on almost "
                                     "every card.  Unit 0 was assumed.");
                }
                break;

            case IF_KEY_ADDRESS:
                /* "address=dhcp" is legal Roadshow and means "ask the server". */
                if (lookup_iptype(value, &type))
                {
                    out->iptype = type;
                }
                else if (!ami_config_parse_ip(value, &out->address))
                {
                    AMI_WARN("config: %s: bad ADDRESS '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "ADDRESS",
                                     value,
                                     CFG_HINT_IPV4 "  Write ADDRESS=DHCP to "
                                     "have the address handed out "
                                     "automatically.");
                }
                break;

            case IF_KEY_NETMASK:
                if (lookup_iptype(value, &type))
                {
                    out->iptype = type;
                }
                else if (!ami_config_parse_ip(value, &out->netmask))
                {
                    AMI_WARN("config: %s: bad NETMASK '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "NETMASK",
                                     value,
                                     "A netmask looks like an address.  On a "
                                     "home network it is almost always "
                                     "255.255.255.0.");
                }
                break;

            case IF_KEY_GATEWAY:
                if (!ami_config_parse_ip(value, &out->gateway))
                {
                    AMI_WARN("config: %s: bad GATEWAY '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "GATEWAY",
                                     value,
                                     "The gateway is the address of the router. "
                                     CFG_HINT_IPV4);
                }
                break;

            case IF_KEY_MTU:
                if (ami_cfg_parse_ulong(value, &n))
                {
                    out->mtu = n;
                }
                else
                {
                    AMI_WARN("config: %s: bad MTU '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "MTU", value,
                                     "MTU is a plain number of bytes, normally "
                                     "1500.  Leave it out and the driver decides.");
                }
                break;

            case IF_KEY_CONFIGURE:
                if (lookup_iptype(value, &type))
                {
                    out->iptype = type;
                }
                else
                {
                    AMI_WARN("config: %s: bad CONFIGURE '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "CONFIGURE",
                                     value,
                                     "CONFIGURE is DHCP (let the network hand "
                                     "out an address), STATIC (use the ADDRESS "
                                     "below), AUTO (pick one without a server) "
                                     "or NONE (no IPv4 on this interface).  "
                                     "STATIC was assumed.");
                }
                break;

            case IF_KEY_IPTYPE:
                /* Numeric IPTYPE is the SANA-II packet type; alphabetic is
                   the AmiTCP-style address mode. */
                if (ami_cfg_parse_ulong(value, &n))
                    AMI_DEBUG("config: %s: SANA-II IPTYPE %lu (sana2 layer)",
                              out->name, (unsigned long)n);
                else if (lookup_iptype(value, &type))
                {
                    out->iptype = type;
                }
                else
                {
                    AMI_WARN("config: %s: bad IPTYPE '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "IPTYPE",
                                     value,
                                     "IPTYPE is either a packet type number "
                                     "(2048 for Ethernet) or one of DHCP, "
                                     "STATIC and AUTO.");
                }
                break;

            case IF_KEY_MDNS:
                if (!ami_cfg_parse_bool(value, &out->mdns))
                {
                    AMI_WARN("config: %s: bad MDNS '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "MDNS",
                                     value,
                                     "MDNS is YES or NO.  NO was assumed.");
                    out->mdns = FALSE;
                }
                break;

            case IF_KEY_DOWNGOESOFFLINE:
                if (!ami_cfg_parse_bool(value, &out->down_goes_offline))
                {
                    AMI_WARN("config: %s: bad DOWNGOESOFFLINE '%s'",
                             out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN,
                                     "DOWNGOESOFFLINE", value,
                                     "DOWNGOESOFFLINE is YES or NO.  NO was "
                                     "assumed.");
                    out->down_goes_offline = FALSE;
                }
                break;

            /* Roadshow defaults this to YES; the default here is NO. */
            case IF_KEY_REQUIRESINITDELAY:
                if (!ami_cfg_parse_bool(value, &out->requires_init_delay))
                {
                    AMI_WARN("config: %s: bad REQUIRESINITDELAY '%s'",
                             out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN,
                                     "REQUIRESINITDELAY", value,
                                     "REQUIRESINITDELAY is YES or NO.  NO was "
                                     "assumed.");
                    out->requires_init_delay = FALSE;
                }
                break;

            /* The address to configure the card with, "xx:xx:xx:xx:xx:xx". */
            case IF_KEY_HARDWAREADDRESS:
                if (ami_cfg_parse_mac(value, out->hw_address))
                {
                    out->have_hw_address = TRUE;
                }
                else
                {
                    AMI_WARN("config: %s: bad HARDWAREADDRESS '%s'",
                             out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN,
                                     "HARDWAREADDRESS", value,
                                     "HARDWAREADDRESS is six hexadecimal "
                                     "bytes, as in 02:00:00:12:34:56.  The "
                                     "card's own address was kept.");
                    out->have_hw_address = FALSE;
                }
                break;

            case IF_KEY_STATE:
                if (ami_cfg_stricmp(value, "up") == 0 ||
                    ami_cfg_stricmp(value, "online") == 0)
                {
                    out->up = TRUE;
                }
                else if (ami_cfg_stricmp(value, "down") == 0 ||
                         ami_cfg_stricmp(value, "offline") == 0)
                {
                    out->up = FALSE;
                }
                else
                {
                    AMI_WARN("config: %s: bad STATE '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "STATE",
                                     value,
                                     "STATE is UP or DOWN.  UP was assumed.");
                }
                break;

#ifdef AMINETXDUO_IPV6
            case IF_KEY_ADDRESS6:
                /* ADDRESS6 with no CONFIGURE6 implies STATIC.  Repeat the
                   keyword and the interface carries both addresses: an Amiga
                   holding a ULA and a global at once is what RFC 6724 rule 6
                   needs, and one line per interface could not describe it. */
            {
                char          zone[AMI_CFG_IP6_ZONE_LEN];
                AmiIp6Address parsed;

                parsed.prefix = 64;

                if (!ami_config_parse_ip6_zone(value, parsed.addr,
                                               &parsed.prefix, zone,
                                               sizeof(zone)))
                {
                    AMI_WARN("config: %s: bad ADDRESS6 '%s'", out->name, value);
                }
                else if (!cfg_zone_ok(out, "ADDRESS6", zone, value))
                {
                    /* Warned about above. The address is not taken. */
                }
                else if (out->address6_count >= AMI_CFG_MAX_ADDRESS6)
                {
                    /* The ceiling is the stack's, see AMI_CFG_MAX_ADDRESS6.
                       Refusing the extra line and keeping the ones already
                       taken is the only behaviour that leaves the interface
                       usable. */
                    AMI_WARN("config: %s: more than %ld ADDRESS6 lines, "
                             "'%s' ignored", out->name,
                             (long)AMI_CFG_MAX_ADDRESS6, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "ADDRESS6",
                                     value,
                                     "An interface carries at most two static "
                                     "IPv6 addresses, because the third slot "
                                     "the stack has per interface holds the "
                                     "link-local address.  This line was "
                                     "ignored.");
                }
                else
                {
                    out->address6[out->address6_count] = parsed;
                    out->address6_count++;

                    if (!have_configure6)
                        out->ip6type = AMI_IP6TYPE_STATIC;
                }
                break;
            }

            case IF_KEY_GATEWAY6:
            {
                char zone[AMI_CFG_IP6_ZONE_LEN];

                if (!ami_config_parse_ip6_zone(value, out->gateway6, NULL,
                                               zone, sizeof(zone)))
                    AMI_WARN("config: %s: bad GATEWAY6 '%s'", out->name, value);
                else if (cfg_zone_ok(out, "GATEWAY6", zone, value))
                    out->have_gateway6 = TRUE;
                break;
            }

            case IF_KEY_CONFIGURE6:
            {
                AmiIp6Type t6;

                if (lookup_ip6type(value, &t6))
                {
                    out->ip6type    = t6;
                    have_configure6 = TRUE;
                }
                else
                {
                    AMI_WARN("config: %s: bad CONFIGURE6 '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR,
                                     "CONFIGURE6", value,
                                     "CONFIGURE6 is AUTO (follow the router), "
                                     "DHCP (ask a DHCPv6 server), STATIC (use "
                                     "the ADDRESS6 below), LINKLOCAL (fe80:: "
                                     "only) or OFF.  AUTO was assumed.");
                }
                break;
            }
#else
            case IF_KEY_ADDRESS6:
            case IF_KEY_GATEWAY6:
            case IF_KEY_CONFIGURE6:
                AMI_DEBUG("config: %s: %s=%s needs an IPv6 build",
                          out->name, key, value);
                break;
#endif /* AMINETXDUO_IPV6 */

            case IF_KEY_IGNORED:
                AMI_DEBUG("config: %s: ignoring %s=%s", out->name, key, value);
                report_inert_keyword(lineno, key);
                break;

            case IF_KEY_UNKNOWN:
            default:
                AMI_WARN("config: %s: unknown keyword '%s'", out->name, key);
                report_unknown_keyword(lineno, key, CFG_HINT_KEYWORDS);
                break;
            }
        }
    }

    if (!have_device)
    {
        AMI_WARN("config: %s: no DEVICE keyword, ignoring interface", out->name);
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR,
                        "there is no DEVICE line, so the file does not say "
                        "which network card to use",
                        "Add a line such as  DEVICE = a2065.device  that "
                        "names the driver for the card, or let NetSetup "
                        "write the file.");
        return AMI_CFG_ERR_SYNTAX;
    }

    /* An unknown CARD must refuse the interface: coming up anyway binds to
       whatever UNIT points at. */
    if (bad_card)
    {
        AMI_WARN("config: %s: unknown CARD, ignoring interface", out->name);
        return AMI_CFG_ERR_SYNTAX;
    }

#ifdef AMINETXDUO_IPV6
    if (out->ip6type == AMI_IP6TYPE_STATIC && out->address6_count == 0)
    {
        /* Degrade rather than refuse: link-local always works, and every IPv6
           interface is required to have that address in any case. */
        AMI_WARN("config: %s: CONFIGURE6=STATIC with no ADDRESS6, using "
                 "link-local only", out->name);
        out->ip6type = AMI_IP6TYPE_LINKLOCAL;
    }
#endif

    /* "Will this interface ever have an address".  An IPv6 plan must be
       stated -- ADDRESS6, GATEWAY6, CONFIGURE6 or CONFIGURE=NONE -- because
       CONFIGURE6 defaults to AUTO and the default must not count. */
    {
        BOOL v4_plan = (out->iptype == AMI_IPTYPE_DHCP ||
                        out->iptype == AMI_IPTYPE_LINKLOCAL ||
                        (out->iptype == AMI_IPTYPE_STATIC &&
                         out->address != 0));
        BOOL v6_plan = FALSE;

#ifdef AMINETXDUO_IPV6
        if (out->ip6type != AMI_IP6TYPE_OFF)
            v6_plan = (BOOL)(have_configure6 || out->have_gateway6 ||
                             out->iptype == AMI_IPTYPE_NONE ||
                             out->address6_count != 0);
#endif

        if (!v4_plan && !v6_plan)
        {
            AMI_WARN("config: %s: no address of either family", out->name);
            ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR,
                            "the interface has no address: there is no ADDRESS "
                            "line, CONFIGURE does not say DHCP, and nothing "
                            "asks for IPv6 either",
                            "Add  CONFIGURE = DHCP  to have an address handed "
                            "out, or  ADDRESS = 192.168.1.10  and  NETMASK = "
                            "255.255.255.0  to set one by hand, or  CONFIGURE6 "
                            "= AUTO  for an IPv6-only interface.");
        }
    }

    out->configured = TRUE;

    return AMI_CFG_OK;
}

BOOL ami_config_iface_wants_ipv4(const AmiIfConfig *cfg)
{
    if (cfg == NULL)
        return FALSE;

    switch (cfg->iptype)
    {
    case AMI_IPTYPE_DHCP:
    case AMI_IPTYPE_LINKLOCAL:
        return TRUE;

    case AMI_IPTYPE_STATIC:
        /* A static interface with no ADDRESS has nothing coming: no server is
           being asked and no address was written down. */
        return (BOOL)(cfg->address != 0);

    case AMI_IPTYPE_NONE:
    default:
        return FALSE;
    }
}

BOOL ami_config_iface_wants_ipv6(const AmiIfConfig *cfg)
{
    if (cfg == NULL)
        return FALSE;

    /* In the floor build ip6type is always OFF, so this is always FALSE and
       the compiler folds every caller's branch away. */
    return (BOOL)(cfg->ip6type != AMI_IP6TYPE_OFF);
}

/* ------------------------------------------------------- name_resolution */

/* name_resolution is resolv.conf-shaped: `keyword value`, '#' comments.
   Also accepted: `keyword=value`, ';' comments, and AmiTCP netdb-myhost
   lines (HOST <addr> <name> [alias...]). */
VOID ami_cfg_parse_resolver(char *buf, AmiResolverConfig *out,
                            char *hostname, ULONG hostname_len)
{
    char *cursor = buf;
    char *line;
    ULONG lineno = 0;

    if (buf == NULL || out == NULL)
        return;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;

        lineno++;

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
                report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "NAMESERVER",
                                 value,
                                 "A name server is given by address, not by "
                                 "name.  On a home network it is usually the "
                                 "router, for example 192.168.1.1.");
            }
            else if (out->nameserver_count >= AMI_CFG_MAX_NAMESERVERS)
            {
                AMI_WARN("config: more than %ld name servers, ignoring '%s'",
                         (long)AMI_CFG_MAX_NAMESERVERS, value);
            }
            else
            {
                /* Negative: statically configured, one reference. */
                out->nameserver_use[out->nameserver_count] = -1;
                out->nameserver[out->nameserver_count++]   = addr;
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

            /* Everything up to here is the file's. A lease appends after it. */
            out->search_static = out->search_count;
            continue;   /* the rest of the line has been consumed */
        }
        else if (ami_cfg_stricmp(key, "prefer") == 0)
        {
            /* static|dynamic: whether DHCP servers override these entries. */
            AMI_DEBUG("config: name_resolution: PREFER=%s", value);
        }
        else if (ami_cfg_stricmp(key, "host") == 0)
        {
            /* HOST <address> <name> [alias...]: the first non-loopback
               entry names this machine. */
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
            AMI_TRACE("config: skipping hosts entry for '%s'", key);
        }
        else
        {
            char text[96];

            AMI_WARN("config: name_resolution: unknown keyword '%s'", key);
            ami_cfg_join3(text, sizeof(text), "unknown keyword '", key, "'");
            ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN, text,
                            "This file holds NAMESERVER, DOMAIN and SEARCH "
                            "lines.  The line was ignored.");
        }
    }
}

UWORD ami_config_search_list(const AmiResolverConfig *res, const char *out[],
                             UWORD max)
{
    UWORD n = 0;
    UWORD i;
    UWORD j;

    if (res == NULL || out == NULL)
        return 0;

    for (i = 0; i < res->search_static && n < max; i++)
        out[n++] = res->search[i];

    /* resolv.conf has SEARCH replace DOMAIN, and the shipped example file says
       so, so DOMAIN is a suffix only when the file has no SEARCH line. */
    if (res->search_static == 0 && res->domain[0] != '\0' && n < max)
        out[n++] = res->domain;

    for (i = res->search_static; i < res->search_count && n < max; i++)
    {
        for (j = 0; j < n; j++)
            if (ami_cfg_stricmp(out[j], res->search[i]) == 0)
                break;

        if (j == n)
            out[n++] = res->search[i];
    }

    return n;
}

BOOL ami_config_search_offer(AmiResolverConfig *res, const char *domain)
{
    UWORD i;

    if (res == NULL || domain == NULL || *domain == '\0')
        return FALSE;

    if (!ami_config_hostname_valid(domain))
    {
        AMI_WARN("config: the network offered '%s' as a search domain. "
                 "That is not a domain name, so it is ignored", domain);
        return FALSE;
    }

    for (i = 0; i < res->search_count; i++)
        if (ami_cfg_stricmp(res->search[i], domain) == 0)
            return FALSE;

    if (res->search_count >= AMI_CFG_MAX_SEARCH)
    {
        AMI_WARN("config: more than %ld search domains, ignoring '%s'",
                 (long)AMI_CFG_MAX_SEARCH, domain);
        return FALSE;
    }

    ami_cfg_copy_string(res->search[res->search_count], AMI_CFG_NAME_LEN,
                        domain);
    res->search_use[res->search_count] = 1;
    res->search_count++;

    return TRUE;
}

static BOOL cfg_ip6_same(const ULONG a[AMI_CFG_IP6_WORDS],
                         const ULONG b[AMI_CFG_IP6_WORDS])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] &&
                  a[2] == b[2] && a[3] == b[3]);
}

BOOL ami_config_nameserver6_offer(AmiResolverConfig *res,
                                  const ULONG addr[AMI_CFG_IP6_WORDS])
{
    UWORD i;

    if (res == NULL || addr == NULL)
        return FALSE;

    /* :: is not a name server. NetX Duo refuses it and so does this. */
    if (addr[0] == 0UL && addr[1] == 0UL && addr[2] == 0UL && addr[3] == 0UL)
        return FALSE;

    for (i = 0; i < res->nameserver6_count; i++)
        if (cfg_ip6_same(res->nameserver6[i], addr))
            return FALSE;

    if (res->nameserver6_count >= (UWORD)AMI_CFG_MAX_NAMESERVERS)
        return FALSE;

    i = res->nameserver6_count;

    res->nameserver6[i][0] = addr[0];
    res->nameserver6[i][1] = addr[1];
    res->nameserver6[i][2] = addr[2];
    res->nameserver6[i][3] = addr[3];
    /* One owner, acquired at run time: the same convention nameserver_use[]
       uses, so ObtainDomainNameServerList() can report both lists the same
       way. */
    res->nameserver6_use[i] = 1;

    res->nameserver6_count = (UWORD)(i + 1);

    return TRUE;
}

BOOL ami_config_nameserver6_withdraw(AmiResolverConfig *res,
                                     const ULONG addr[AMI_CFG_IP6_WORDS])
{
    UWORD i;
    UWORD j;

    if (res == NULL || addr == NULL)
        return FALSE;

    for (i = 0; i < res->nameserver6_count; i++)
    {
        if (!cfg_ip6_same(res->nameserver6[i], addr))
            continue;

        for (j = (UWORD)(i + 1); j < res->nameserver6_count; j++)
        {
            res->nameserver6[j - 1][0] = res->nameserver6[j][0];
            res->nameserver6[j - 1][1] = res->nameserver6[j][1];
            res->nameserver6[j - 1][2] = res->nameserver6[j][2];
            res->nameserver6[j - 1][3] = res->nameserver6[j][3];
            res->nameserver6_use[j - 1] = res->nameserver6_use[j];
        }

        res->nameserver6_count--;

        res->nameserver6[res->nameserver6_count][0] = 0UL;
        res->nameserver6[res->nameserver6_count][1] = 0UL;
        res->nameserver6[res->nameserver6_count][2] = 0UL;
        res->nameserver6[res->nameserver6_count][3] = 0UL;
        res->nameserver6_use[res->nameserver6_count] = 0;

        return TRUE;
    }

    return FALSE;
}

BOOL ami_config_search_withdraw(AmiResolverConfig *res, const char *domain)
{
    UWORD i;
    UWORD j;

    if (res == NULL || domain == NULL || *domain == '\0')
        return FALSE;

    for (i = res->search_static; i < res->search_count; i++)
    {
        if (ami_cfg_stricmp(res->search[i], domain) != 0)
            continue;

        for (j = (UWORD)(i + 1); j < res->search_count; j++)
        {
            ami_cfg_copy_string(res->search[j - 1], AMI_CFG_NAME_LEN,
                                res->search[j]);
            res->search_use[j - 1] = res->search_use[j];
        }

        res->search_count--;
        res->search[res->search_count][0] = '\0';
        res->search_use[res->search_count] = 0;

        return TRUE;
    }

    return FALSE;
}

BOOL ami_config_search_reference_add(AmiResolverConfig *res,
                                     const char *domain)
{
    UWORD i;

    if (res == NULL || domain == NULL || *domain == '\0' ||
        !ami_config_hostname_valid(domain))
        return FALSE;

    for (i = 0; i < res->search_count; i++)
    {
        if (ami_cfg_stricmp(res->search[i], domain) != 0)
            continue;

        if (i >= res->search_static && res->search_use[i] != (UWORD)~0U)
            res->search_use[i]++;
        return TRUE;
    }

    return ami_config_search_offer(res, domain);
}

BOOL ami_config_search_reference_remove(AmiResolverConfig *res,
                                        const char *domain)
{
    UWORD i;

    if (res == NULL || domain == NULL || *domain == '\0')
        return FALSE;

    for (i = 0; i < res->search_count; i++)
    {
        if (ami_cfg_stricmp(res->search[i], domain) != 0)
            continue;

        if (i < res->search_static)
            return TRUE;

        if (res->search_use[i] > 1U)
        {
            res->search_use[i]--;
            return TRUE;
        }

        return ami_config_search_withdraw(res, domain);
    }

    return FALSE;
}

/*
 * One RFC 1035 4.1.4 name out of an option 119 payload, starting at *pos.
 * *pos advances past the name AS WRITTEN, not to where decoding ended.
 */
#define AMI_CFG_RFC3397_JUMPS   8

static BOOL cfg_rfc3397_name(const UBYTE *data, ULONG len, ULONG *pos,
                             char *out, ULONG outlen)
{
    ULONG at    = *pos;
    ULONG n     = 0;
    UWORD jumps = 0;
    BOOL  moved = FALSE;

    for (;;)
    {
        UWORD label;

        if (at >= len)
            return FALSE;

        label = (UWORD)data[at];

        if ((label & 0xC0) == 0xC0)
        {
            ULONG target;

            if (at + 1 >= len || jumps++ >= AMI_CFG_RFC3397_JUMPS)
                return FALSE;

            target = (ULONG)(((label & 0x3F) << 8) | data[at + 1]);

            /* Strictly backwards. A pointer to itself or forwards is how a
               hostile option makes this walk run forever. */
            if (target >= at)
                return FALSE;

            if (!moved)
            {
                *pos  = at + 2;
                moved = TRUE;
            }

            at = target;
            continue;
        }

        if ((label & 0xC0) != 0)
            return FALSE;       /* reserved label type */

        at++;

        if (label == 0)
        {
            if (!moved)
                *pos = at;

            out[n] = '\0';

            return (BOOL)(n != 0);
        }

        if (at + label > len)
            return FALSE;

        if (n != 0)
        {
            if (n + 1 >= outlen)
                return FALSE;
            out[n++] = '.';
        }

        if (n + label >= outlen)
            return FALSE;

        while (label-- != 0)
            out[n++] = (char)data[at++];
    }
}

UWORD ami_config_search_from_rfc3397(AmiResolverConfig *res,
                                     const UBYTE *data, ULONG len)
{
    char  name[AMI_CFG_NAME_LEN];
    ULONG pos   = 0;
    UWORD added = 0;

    if (res == NULL || data == NULL)
        return 0;

    while (pos < len)
    {
        if (!cfg_rfc3397_name(data, len, &pos, name, (ULONG)sizeof(name)))
            break;

        if (ami_config_search_offer(res, name))
            added++;
    }

    return added;
}

UWORD ami_config_search_withdraw_rfc3397(AmiResolverConfig *res,
                                         const UBYTE *data, ULONG len)
{
    char  name[AMI_CFG_NAME_LEN];
    ULONG pos     = 0;
    UWORD removed = 0;

    if (res == NULL || data == NULL)
        return 0;

    while (pos < len)
    {
        if (!cfg_rfc3397_name(data, len, &pos, name, (ULONG)sizeof(name)))
            break;

        if (ami_config_search_withdraw(res, name))
            removed++;
    }

    return removed;
}

/* -------------------------------------------------- default_gateway/routes */

VOID ami_cfg_parse_gateway(char *buf, ULONG *out)
{
    char *cursor = buf;
    char *line;
    ULONG lineno = 0;

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

        lineno++;

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
                {
                    have_gw = TRUE;
                }
                else
                {
                    AMI_WARN("config: bad gateway address '%s'", value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_ERROR, "the gateway",
                                     value,
                                     "This is the address of the router, and it "
                                     "must be on the same network as this "
                                     "machine. " CFG_HINT_IPV4);
                }
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
                char text[96];

                AMI_WARN("config: routes: unknown keyword '%s'", key);
                ami_cfg_join3(text, sizeof(text), "unknown keyword '", key, "'");
                ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN, text,
                                "A routes file holds DEFAULT=<router address> "
                                "for the default route, and DST=/VIA= pairs for "
                                "anything else.  The line was ignored.");
            }
        }

        /* default_gateway has no DEFAULT keyword -- its GATEWAY is the
           default route.  In routes, only a DEFAULT= line is. */
        if (have_dst && !is_default)
            continue;

        if (have_gw && *out == 0)
            *out = gateway;
    }
}

/* --------------------------------------------------------- tcp_handler */

/* DEVS:Internet/tcp_handler: "OFF" alone on a line means "TCPHANDLER=OFF".
   Anything else warns and leaves the default alone. */
VOID ami_cfg_parse_tcp_handler(char *buf, BOOL *out)
{
    char *cursor = buf;
    char *line;
    ULONG lineno = 0;

    if (buf == NULL || out == NULL)
        return;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        char *pos;
        char *key;
        char *value;

        lineno++;

        ami_cfg_strip_comment(line, "#;");
        line = ami_cfg_trim(line);
        if (*line == '\0')
            continue;

        pos = line;
        while (ami_cfg_next_pair(&pos, &key, &value))
        {
            BOOL on;

            if (*value == '\0' && ami_cfg_parse_bool(key, &on))
            {
                *out = on;
                continue;
            }

            if (ami_cfg_stricmp(key, "tcphandler") != 0 &&
                ami_cfg_stricmp(key, "tcp") != 0)
            {
                char text[96];

                AMI_WARN("config: tcp_handler: unknown keyword '%s'", key);
                ami_cfg_join3(text, sizeof(text), "unknown keyword '", key, "'");
                ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN, text,
                                "This file switches the TCP: device on or off "
                                "and understands nothing else.  Write "
                                "TCPHANDLER=OFF, or OFF on its own.");
                continue;
            }

            if (ami_cfg_parse_bool(value, &on))
            {
                *out = on;
            }
            else
            {
                AMI_WARN("config: tcp_handler: bad value '%s'", value);
                report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "TCPHANDLER",
                                 value,
                                 "Write ON or OFF.  The TCP: device was left "
                                 "switched on.");
            }
        }
    }
}

/* ---------------------------------------------------- service_discovery */

/*
 * DEVS:Internet/service_discovery, one service per line:
 *     <type>  <port>  [instance name]  [txt=key=value;key=value]
 * ';' is a comment ONLY as the first character of a line: mid-line it is the
 * separator between key=value pairs inside a TXT record.
 */

/* One whitespace-delimited word, NUL-terminated in place. No quoting: neither
 * a service type nor a port number can contain a space. */
static char *dnssd_word(char **cursor)
{
    char *p = *cursor;
    char *start;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0')
    {
        *cursor = p;
        return NULL;
    }

    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        p++;

    if (*p != '\0')
        *p++ = '\0';

    *cursor = p;

    return start;
}

/*
 * RFC 6763 7: "_" then 1..15 of [A-Za-z0-9-], then "._tcp" or "._udp".
 * The 15 is what makes an accepted type fit both AMI_CFG_SD_TYPE_LEN and
 * NX_MDNS_TYPE_MAX (21) without truncation.
 */
static BOOL dnssd_type_ok(const char *s)
{
    ULONG n = 0;

    if (*s++ != '_')
        return FALSE;

    while (*s != '\0' && *s != '.')
    {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '-'))
            return FALSE;
        s++;
        n++;
    }

    if (n == 0 || n > 15)
        return FALSE;

    return (BOOL)(ami_cfg_stricmp(s, "._tcp") == 0 ||
                  ami_cfg_stricmp(s, "._udp") == 0);
}

/* Find "txt=" at a word boundary, case-insensitive. */
static char *dnssd_txt_field(char *rest)
{
    char *p = rest;

    for (;;)
    {
        if ((p == rest || p[-1] == ' ' || p[-1] == '\t') &&
            (p[0] == 't' || p[0] == 'T') &&
            (p[1] == 'x' || p[1] == 'X') &&
            (p[2] == 't' || p[2] == 'T') &&
            p[3] == '=')
            return p;

        if (*p == '\0')
            return NULL;
        p++;
    }
}

#define CFG_HINT_DNSSD \
    "A line is <type> <port>, for example:  _ftp._tcp  21.  The type is an " \
    "RFC 6763 name: an underscore, up to fifteen letters, digits or hyphens, " \
    "then ._tcp or ._udp."

VOID ami_cfg_parse_dnssd(char *buf, AmiSdService *out, UWORD max, UWORD *count)
{
    char *cursor = buf;
    char *line;
    ULONG lineno = 0;
    BOOL  full   = FALSE;

    if (buf == NULL || out == NULL || count == NULL)
        return;

    while ((line = ami_cfg_next_line(&cursor)) != NULL)
    {
        AmiSdService *svc;
        char         *rest;
        char         *type;
        char         *port_text;
        char         *txt = NULL;
        ULONG         port = 0;
        ULONG         namelen;

        lineno++;

        ami_cfg_strip_comment(line, "#");
        line = ami_cfg_trim(line);
        if (*line == '\0' || *line == ';')
            continue;

        rest      = line;
        type      = dnssd_word(&rest);
        port_text = dnssd_word(&rest);

        if (type == NULL)
            continue;

        if (!dnssd_type_ok(type))
        {
            AMI_WARN("config: service_discovery: bad type '%s'", type);
            report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "the service type",
                             type, CFG_HINT_DNSSD "  The line was ignored.");
            continue;
        }

        if (port_text == NULL || !ami_cfg_parse_ulong(port_text, &port) ||
            port == 0 || port > 65535UL)
        {
            AMI_WARN("config: service_discovery: bad port for '%s'", type);
            report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "the port",
                             (port_text != NULL) ? port_text : "",
                             "A port is a number from 1 to 65535, and it is "
                             "the port the server listens on.  "
                             "The line was ignored.");
            continue;
        }

        /* Split the remainder into the instance name and the txt= field. */
        rest = ami_cfg_trim(rest);
        txt  = dnssd_txt_field(rest);
        if (txt != NULL)
        {
            char *end = txt;

            while (end > rest && (end[-1] == ' ' || end[-1] == '\t'))
                end--;

            *end = '\0';    /* the name now ends where the field began */
            txt += 4;
        }

        ami_cfg_unquote(rest);
        if (txt != NULL)
            ami_cfg_unquote(txt);

        /* A dot in the instance name becomes a label boundary downstream,
           so the line is refused rather than renamed. */
        namelen = ami_cfg_strlen(rest);
        if (namelen > 0)
        {
            const char *p;

            for (p = rest; *p != '\0'; p++)
            {
                if (*p == '.')
                {
                    AMI_WARN("config: service_discovery: '%s' has a dot", rest);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN,
                                     "the service name", rest,
                                     "A service name is one label, so it "
                                     "cannot contain a dot.  The line was "
                                     "ignored.");
                    break;
                }
            }
            if (*p == '.')
                continue;
        }

        /* 4 characters of headroom: the module appends " (2)" on a collision. */
        if (namelen + 4 >= (ULONG)AMI_CFG_NAME_LEN)
        {
            AMI_WARN("config: service_discovery: name too long on line %lu",
                     (unsigned long)lineno);
            ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN,
                            "the service name is too long",
                            "Keep it under sixty characters.  The line was "
                            "ignored.");
            continue;
        }

        if (txt != NULL && ami_cfg_strlen(txt) >= (ULONG)AMI_CFG_SD_TXT_LEN)
        {
            AMI_WARN("config: service_discovery: txt too long on line %lu",
                     (unsigned long)lineno);
            ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN,
                            "the txt= field is too long",
                            "A TXT record holds at most 255 characters.  The "
                            "line was ignored.");
            continue;
        }

        if (*count >= max)
        {
            /* Once, however many lines follow. */
            if (!full)
            {
                full = TRUE;
                AMI_WARN("config: more than %ld services, ignoring the rest",
                         (long)max);
                ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN,
                                "there are more services here than can be "
                                "advertised",
                                "At most eight are announced.  The ones after "
                                "that were ignored.");
            }
            continue;
        }

        svc = &out[*count];
        ami_cfg_zero(svc, sizeof(*svc));
        ami_cfg_copy_string(svc->type, sizeof(svc->type), type);
        ami_cfg_copy_string(svc->name, sizeof(svc->name), rest);
        if (txt != NULL)
            ami_cfg_copy_string(svc->txt, sizeof(svc->txt), txt);
        svc->port = (UWORD)port;

        (*count)++;
    }
}
