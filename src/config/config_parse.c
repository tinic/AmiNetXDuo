/*
 * AmiNetXDuo, Roadshow configuration file parsers.
 *
 * Formats implemented here, per the Roadshow manual (Olaf Barthel, 1.15,
 * 4 September 2023), sections 7.1.1 (DEVS:NetInterfaces), 7.1.2.3
 * (DEVS:Internet/name_resolution) and 7.1.2.6 (DEVS:Internet/routes):
 *
 *   - one `keyword=value` per line, whitespace around either side ignored;
 *   - lines starting with ';' or '#' are comments (a comment starting mid-line
 *     is dropped too, which Roadshow does not do; hand-written files in the
 *     wild use it and it can never make a valid file invalid);
 *   - keywords are case-insensitive (ReadArgs templates);
 *   - "quoted values" use the AmigaDOS '*' escape.
 *
 * Additions to the manual:
 *
 *   - Roadshow's IPTYPE is the SANA-II packet type number (default 2048), not
 *     an address-configuration mode. AmiTCP/Genesis-era documentation and
 *     several config generators use `IPTYPE=DHCP`/`STATIC` instead. Both are
 *     accepted: a numeric IPTYPE is the packet type, an alphabetic one the
 *     address mode, which is unambiguous.
 *   - GATEWAY= inside an interface file is not a Roadshow keyword (Roadshow
 *     puts the default route in DEVS:Internet/routes) but AmiTCP_NG writes it
 *     and AmiIfConfig has the field, so it is accepted.
 *   - DEVS:Internet/default_gateway does not exist in Roadshow 1.15 either;
 *     docs/RESEARCH.md and config.h call for it, so it is read first and
 *     DEVS:Internet/routes after it.
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

    /*
     * IPv6. Roadshow has no IPv6 keywords, no Amiga stack has ever had IPv6
     * so these are ours, named by appending "6" to the IPv4 keyword they
     * mirror. That is the one naming rule that needs no documentation to
     * guess, it cannot collide with a real Roadshow keyword (the manual has
     * none ending in a digit), and it keeps one interface file describing both
     * families:
     *
     *     DEVICE     = a2065.device
     *     UNIT       = 0
     *     CONFIGURE  = DHCP              ; IPv4
     *     CONFIGURE6 = AUTO              ; IPv6: OFF|LINKLOCAL|AUTO|STATIC
     *     ADDRESS6   = 2001:db8::10/64   ; STATIC only; /64 if the length is
     *     GATEWAY6   = fe80::1           ; omitted
     *
     * In the floor build (no AMINETXDUO_IPV6) these are parsed as far as being
     * recognised and are then ignored, so the same file works in both builds
     * without producing "unknown keyword" warnings.
     */
    { "address6",           IF_KEY_ADDRESS6  },
    { "ipaddress6",         IF_KEY_ADDRESS6  },
    { "gateway6",           IF_KEY_GATEWAY6  },
    { "configure6",         IF_KEY_CONFIGURE6},
    { "iptype6",            IF_KEY_CONFIGURE6},

    /*
     * Roadshow keywords we parse but have nowhere to put: they belong to the
     * SANA-II shim rather than to the IP configuration. Listed so that
     * a stock config file produces no warnings.
     */
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
    /* Roadshow's own AddNetInterface template spells the alias
       DESTINATION=DESTINATIONADDRESS and its ConfigureNetInterface spells it
       DESTINATION=DESTINATIONADDR, so both are here.  These three were
       missing entirely, which made a stock Roadshow interface file report
       three unknown keywords -- the opposite of what this list is for. */
    { "destinationaddress", IF_KEY_IGNORED   },
    { "hardwaretype",       IF_KEY_IGNORED   },
    { "broadcastaddress",   IF_KEY_IGNORED   },
    { "metric",             IF_KEY_IGNORED   },
    { "lease",              IF_KEY_IGNORED   },
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

/* ------------------------------------------------ "did you mean DEVICE?",
 *
 * A mistyped keyword is the commonest fault in a hand-edited interface file,
 * and "unknown keyword 'devcie'" alone leaves the reader hunting for the
 * difference. Levenshtein distance over the keyword table names the fix; the
 * table is 40 short words and this runs once per bad line.
 */
#define CFG_SUGGEST_MAX     24      /* longer than any keyword we know */

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

/*
 * "unknown keyword 'devcie'" + "Did you mean DEVICE? ...". Built here because
 * four call sites want the same shape.
 */
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
        ami_cfg_join3(hint, sizeof(hint), "Did you mean ", upper,
                      "?  The line was ignored.");
    }
    else
    {
        ami_cfg_join3(hint, sizeof(hint), known, NULL, NULL);
    }

    ami_cfg_problem(line, AMI_CFG_PROBLEM_WARN, text, hint);
}

/* "bad ADDRESS '10.0.0.300'" + whatever the keyword's own advice is. */

/*
 * A Roadshow keyword we read and do nothing with.
 *
 * Silence was the old behaviour and it was the wrong one: a user who wrote
 * ALIAS= or METRIC= got no error, no effect and nothing to read, which is
 * indistinguishable from a keyword that worked.  Saying so needs a reporter
 * installed, so the stack at boot still says nothing and CheckNetConfig --
 * whose whole job is to read the files and say what is wrong with them --
 * prints one line per keyword with the reason it is inert.
 *
 * The reason is per keyword because "unsupported" is not actionable and
 * "your card cannot do this" is.
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
            ami_cfg_problem(line, AMI_CFG_PROBLEM_WARN, text,
                            "Roadshow acts on it.  This stack does not.  "
                            "The line is harmless and can stay.");
            return;
        }
    }

    /* NAMESERVER and DOMAIN in an interface file: handled elsewhere, not
       inert, and saying they do nothing would be wrong. */
}

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

/* ------------------------------------------------------------------ CARD,
 *
 * One device file can cover a family of boards -- anxnet.device is opened for
 * every NE2000/DP8390 card -- and then UNIT alone only says "the Nth board in
 * probe order".  CARD says which board by name.
 *
 * The names are the driver's, include/aminetxduo/anxnet.h, read here so that
 * CARD=nonsense is a configuration error with the list beside it rather than
 * an OpenDevice that fails at boot with nothing to read.
 */
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
    "ADDRESS, NETMASK, GATEWAY and MTU.  The line was ignored."

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
 * RFC 4007 11's "%zone" in an interface file.
 *
 * The file already names the interface, DEVS:NetInterfaces/eth0, so a zone
 * on an address in it is either the same interface again, which is redundant
 * and harmless, or a different one, which contradicts the file it is written
 * in. Nothing is stored: there is no zone to remember that the file name does
 * not already say.
 *
 * TRUE to use the address, FALSE to reject the line.
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
    /*
     * The IPv6 default is AUTO, which settles the last open question in
     * docs/RESEARCH.md §9 ("built and off, or built and on when router
     * advertisements appear?").
     *
     * AUTO means: always configure the link-local address, and take a global
     * one from a router advertisement if one arrives. On a link with no IPv6
     * router that is indistinguishable from LINKLOCAL, one router
     * solicitation goes out and nothing answers, so the cost of defaulting
     * to it is three ICMPv6 packets, and the benefit is that IPv6 works on a
     * network that has it without anyone editing a file. It is also what every
     * other operating system on the same wire is already doing.
     *
     * A machine that wants no IPv6 at all sets CONFIGURE6=OFF, or builds the
     * floor configuration, where none of this exists.
     */
    out->ip6type = AMI_IP6TYPE_AUTO;
    out->prefix6 = 64;
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

            /*
             * Which board, when the device file covers a family of them.  An
             * unknown name is an ERROR and the field stays empty: acting on
             * half of it would open the first board in probe order, which is
             * the silent bind to the wrong card CARD exists to prevent.
             */
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

            /*
             * Roadshow's ID= is a free-text label for the interface and has no
             * IP meaning, which is why it was ignored. It is kept now because
             * ami_config_load() will name the machine after it when nothing
             * more deliberate did, see AmiHostnameSource. Stored verbatim;
             * whether it is usable as a host name is decided there.
             */
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
                                     "below) or AUTO (pick one without a "
                                     "server).  STATIC was assumed.");
                }
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
                /*
                 * Answering .local on this wire.  Off unless asked for: the
                 * responder is per interface and so is its cost, and on a
                 * 68000 it is enough to be felt by everything else.
                 */
                if (!ami_cfg_parse_bool(value, &out->mdns))
                {
                    AMI_WARN("config: %s: bad MDNS '%s'", out->name, value);
                    report_bad_value(lineno, AMI_CFG_PROBLEM_WARN, "MDNS",
                                     value,
                                     "MDNS is YES or NO.  NO was assumed.");
                    out->mdns = FALSE;
                }
                break;

            /*
             * Roadshow's DOWNGOESOFFLINE. The field it sets has existed since
             * the interface API landed and is set by IFA_DownGoesOffline, but
             * the config-file keyword that is supposed to set it was on the
             * ignored list, so `Offline eth0` sent S2_OFFLINE when a program
             * asked and never when the file did.
             */
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

            /*
             * A second between opening the device and the first packet.
             * Roadshow defaults this to YES and names the original Ariadne;
             * ours defaults to NO, because a second at every bring-up is a
             * high price on every card for the sake of the one that needs it.
             */
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
                /*
                 * An ADDRESS6 with no CONFIGURE6 implies STATIC, the same way
                 * a Roadshow ADDRESS implies a static IPv4 interface.
                 */
            {
                char zone[AMI_CFG_IP6_ZONE_LEN];

                if (!ami_config_parse_ip6_zone(value, out->address6,
                                               &out->prefix6, zone,
                                               sizeof(zone)))
                {
                    AMI_WARN("config: %s: bad ADDRESS6 '%s'", out->name, value);
                }
                else if (!cfg_zone_ok(out, "ADDRESS6", zone, value))
                {
                    /* Warned about above; the address is not taken. */
                }
                else if (!have_configure6)
                {
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
                }
                break;
            }
#else
            case IF_KEY_ADDRESS6:
            case IF_KEY_GATEWAY6:
            case IF_KEY_CONFIGURE6:
                /* Recognised, so the file is portable; acted on only in the
                   IPv6 build. */
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

    /*
     * A CARD nobody knows refuses the interface rather than dropping the line.
     * Coming up anyway would bind to whatever UNIT points at, which is the
     * silent wrong board this keyword exists to prevent; report_bad_card()
     * above has already named the value and the line.
     */
    if (bad_card)
    {
        AMI_WARN("config: %s: unknown CARD, ignoring interface", out->name);
        return AMI_CFG_ERR_SYNTAX;
    }

    if (out->iptype == AMI_IPTYPE_STATIC && out->address == 0)
    {
        AMI_WARN("config: %s: static but no ADDRESS", out->name);
        ami_cfg_problem(0, AMI_CFG_PROBLEM_ERROR,
                        "the interface has no address: there is no ADDRESS "
                        "line and CONFIGURE does not say DHCP",
                        "Add  CONFIGURE = DHCP  to have an address handed out, "
                        "or  ADDRESS = 192.168.1.10  and  NETMASK = "
                        "255.255.255.0  to set one by hand.");
    }

#ifdef AMINETXDUO_IPV6
    if (out->ip6type == AMI_IP6TYPE_STATIC &&
        (out->address6[0] | out->address6[1] |
         out->address6[2] | out->address6[3]) == 0)
    {
        /* Degrade rather than refuse: link-local always works, and an
           interface with no usable IPv6 address at all is worse than one with
           the address every IPv6 interface is required to have anyway. */
        AMI_WARN("config: %s: CONFIGURE6=STATIC with no ADDRESS6, using "
                 "link-local only", out->name);
        out->ip6type = AMI_IP6TYPE_LINKLOCAL;
    }
#endif

    out->configured = TRUE;

    return AMI_CFG_OK;
}

/* ------------------------------------------------------- name_resolution */

/*
 * Roadshow's name_resolution is resolv.conf-shaped: `keyword value`, one per
 * line, '#' comments. Also accepted are `keyword=value`, ';' comments, and
 * AmiTCP netdb-myhost lines (HOST <addr> <name> [alias...]), so the same
 * routine can be pointed at an AmiTCP database.
 */
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

            /* Everything up to here is the file's; a lease appends after it. */
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

    /* Off the network, and about to be pasted onto a name and queried. */
    if (!ami_config_hostname_valid(domain))
    {
        AMI_WARN("config: the network offered '%s' as a search domain; that is "
                 "not a domain name, ignoring it", domain);
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

    ami_cfg_copy_string(res->search[res->search_count++], AMI_CFG_NAME_LEN,
                        domain);

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

    /* :: is not a name server; NetX Duo refuses it and so does this. */
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

        /* Close the gap: the order of what is left is the order the resolver
           still asks them in. */
        for (j = (UWORD)(i + 1); j < res->nameserver6_count; j++)
        {
            res->nameserver6[j - 1][0] = res->nameserver6[j][0];
            res->nameserver6[j - 1][1] = res->nameserver6[j][1];
            res->nameserver6[j - 1][2] = res->nameserver6[j][2];
            res->nameserver6[j - 1][3] = res->nameserver6[j][3];
        }

        res->nameserver6_count--;

        res->nameserver6[res->nameserver6_count][0] = 0UL;
        res->nameserver6[res->nameserver6_count][1] = 0UL;
        res->nameserver6[res->nameserver6_count][2] = 0UL;
        res->nameserver6[res->nameserver6_count][3] = 0UL;

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

        /* Close the gap: the order of what is left is the order they are
           still tried in. */
        for (j = (UWORD)(i + 1); j < res->search_count; j++)
            ami_cfg_copy_string(res->search[j - 1], AMI_CFG_NAME_LEN,
                                res->search[j]);

        res->search_count--;
        res->search[res->search_count][0] = '\0';

        return TRUE;
    }

    return FALSE;
}

/*
 * One RFC 1035 4.1.4 name out of an option 119 payload, starting at *pos.
 *
 * *pos is advanced past the name as it is written in the option, which is not
 * where the decoding ended: a name that is a pointer occupies two bytes there
 * and expands from somewhere earlier, so the walk continues after the pointer.
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
                char text[96];

                AMI_WARN("config: routes: unknown keyword '%s'", key);
                ami_cfg_join3(text, sizeof(text), "unknown keyword '", key, "'");
                ami_cfg_problem(lineno, AMI_CFG_PROBLEM_WARN, text,
                                "A routes file holds DEFAULT=<router address> "
                                "for the default route, and DST=/VIA= pairs for "
                                "anything else.  The line was ignored.");
            }
        }

        /*
         * DEVS:Internet/default_gateway has no DEFAULT keyword: the GATEWAY in
         * it is the default route. In DEVS:Internet/routes a line is the
         * default route only when it carries DEFAULT=, a line with a
         * DST/HOSTDST/NETDST destination is a specific route we do not keep.
         */
        if (have_dst && !is_default)
            continue;

        if (have_gw && *out == 0)
            *out = gateway;
    }
}

/* --------------------------------------------------------- tcp_handler */

/*
 * DEVS:Internet/tcp_handler. One setting, so the file is allowed to be one
 * word: "OFF" on a line by itself means the same as "TCPHANDLER=OFF". Anything
 * else is a warning and leaves the default alone, because the default is the
 * behaviour a machine already has and a typo must not remove it.
 */
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
 *
 *     <type>  <port>  [instance name]  [txt=key=value;key=value]
 *
 * The file exists because the alternative does not reach anyone. AmiNetXDuo
 * ships clients only, so an API for a server to call would be an API no
 * server calls, the servers an Amiga runs are AmiFTPd and its kind, already
 * built, and they will not be recompiled against ours. What the user can do
 * is say what is listening, which is exactly the assertion a _ftp._tcp record
 * makes. AmiTCP's db/inetd.conf declares the same thing the same way.
 *
 * The instance name may contain spaces (RFC 6763 4.1.1 allows rich text) and
 * runs to the end of the line or to the txt= field, so it needs no quotes;
 * quotes are accepted anyway, since ami_cfg_unquote() costs nothing here.
 *
 * '#' starts a comment anywhere, as in the netdb files. ';' is a comment only
 * as the first character of a line, because ';' is the separator between
 * key=value pairs inside a TXT record and eating it mid-line would silently
 * truncate one.
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
 * RFC 6763 7: "_" then 1..15 characters of letters, digits and hyphens, then
 * "._tcp" or "._udp". The 15 is where NX_MDNS_TYPE_MAX's 21 comes from, so a
 * type this accepts always fits AMI_CFG_SD_TYPE_LEN and the module's own
 * limit, and no accepted line can be truncated on the way in.
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

/*
 * Find the txt= field: "txt=" at a word boundary, case-insensitive. The &&
 * chain stops at the first mismatch, so a string ending mid-keyword is never
 * read past its NUL.
 */
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

        /*
         * A dot in the instance name would become a label boundary: the module
         * builds "<name>.<type>.local" as text and encodes it by splitting on
         * dots, so "My.Server" would claim a name nothing asked for. Say so
         * rather than quietly renaming it.
         */
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
