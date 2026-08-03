/*
 * AmiNetXDuo -- parsed configuration.
 *
 * The on-disk format is Roadshow's (see docs/RESEARCH.md §6.6):
 *   DEVS:NetInterfaces/<name>      one file per interface
 *   DEVS:Internet/name_resolution  NAMESERVER / DOMAIN / SEARCH
 *   DEVS:Internet/default_gateway  DEVICE / UNIT / GATEWAY
 *   DEVS:Internet/{hosts,networks,protocols,services}   netdb
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CONFIG_H
#define AMINETXDUO_CONFIG_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Two, matching NX_MAX_PHYSICAL_INTERFACES in port/netxduo-amiga/inc/nx_user.h.
   It was 4, which the stack could parse and NetX Duo could not attach: a third
   interface would have been accepted from the config and then had nowhere to
   go. */
#define AMI_CFG_MAX_INTERFACES      2
#define AMI_CFG_MAX_NAMESERVERS     4
#define AMI_CFG_MAX_SEARCH          6
#define AMI_CFG_NAME_LEN            64
#define AMI_CFG_PATH_LEN            128

/* The default domain gets its own cap: SetDefaultDomainName()'s autodoc says
   "cannot be longer than 255 characters", and AMI_CFG_NAME_LEN also sizes the
   interface names, the host name and the DNS-SD instance names, none of which
   wants 256 bytes. A CONFIGURED host name is therefore capped at 63
   characters, where gethostname()'s BUGS entry cites MAXHOSTNAMELEN (256); a
   name gethostname() derives by reverse resolution is not. */
#define AMI_CFG_DOMAIN_LEN          256

/*
 * DNS-SD services declared in DEVS:Internet/service_discovery.
 *
 * Eight is the cap because a machine advertising more than eight servers is
 * not the machine this stack runs on, and because everything the mDNS module
 * is handed has to fit a fixed local record cache.
 *
 * The type is bounded at RFC 6763 7's grammar -- "_" + at most 15 characters
 * + "._tcp" -- which is 21, so a type the parser accepts always fits the
 * module's NX_MDNS_TYPE_MAX. The TXT field is bounded at the module's
 * NX_MDNS_NAME_MAX of 255.
 */
#define AMI_CFG_MAX_SD_SERVICES     8
#define AMI_CFG_SD_TYPE_LEN         22
#define AMI_CFG_SD_TXT_LEN          256

typedef enum {
    AMI_IPTYPE_STATIC = 0,
    AMI_IPTYPE_DHCP,
    AMI_IPTYPE_LINKLOCAL        /* RFC 3927, used as DHCP fallback */
} AmiIpType;

/*
 * How an interface gets its IPv6 addresses. Every mode except OFF configures
 * the fe80::/64 link-local address derived from the MAC (RFC 4291 modified
 * EUI-64), which needs no router, no server and no configuration and is the
 * only mode that works on an isolated Amiga.
 */
typedef enum {
    AMI_IP6TYPE_OFF = 0,        /* no IPv6 on this interface                  */
    AMI_IP6TYPE_LINKLOCAL,      /* fe80::/64 from the MAC, nothing else       */
    AMI_IP6TYPE_AUTO,           /* link-local + RFC 4862 SLAAC from RAs       */
    AMI_IP6TYPE_STATIC          /* link-local + the configured global address */
} AmiIp6Type;

/*
 * An IPv6 address as four ULONGs in host byte order, [0] most significant.
 * This is NetX Duo's own representation (NXD_ADDRESS.nxd_ip_address.v6), and
 * matching it means no conversion anywhere between the config file and the
 * stack. It is not the byte order of struct in6_addr; src/bsdsocket/ has the
 * conversion, which on m68k is a straight copy but is spelled out anyway.
 */
#define AMI_CFG_IP6_WORDS           4

/* Longest RFC 5952 text form plus NUL: "0:0:0:0:0:ffff:255.255.255.255". */
#define AMI_CFG_IP6_STRLEN          46

/*
 * RFC 4007 11's "<address>%<zone_id>". 46 for the address, the '%', and a
 * zone as long as an interface name (IF_NAMESIZE, terminator included).
 */
#define AMI_CFG_IP6_ZONE_LEN        16
#define AMI_CFG_IP6_ZONE_STRLEN     (AMI_CFG_IP6_STRLEN + 1 + AMI_CFG_IP6_ZONE_LEN)

typedef struct AmiIfConfig {
    char        name[AMI_CFG_NAME_LEN];      /* interface name, e.g. "eth0"      */
    char        id[AMI_CFG_NAME_LEN];        /* Roadshow ID=; free text          */
    char        device[AMI_CFG_PATH_LEN];    /* SANA-II device, e.g. "a2065.device" */
    ULONG       unit;
    AmiIpType   iptype;
    ULONG       address;                     /* host byte order; 0 if unset      */
    ULONG       netmask;
    ULONG       gateway;
    ULONG       mtu;                         /* 0 = ask the driver               */
    BOOL        up;                          /* bring online at startup          */
    BOOL        configured;                  /* slot in use                      */
    BOOL        down_goes_offline;           /* IFA_DownGoesOffline; default FALSE */

    /*
     * IPv6. These fields exist in both build configurations so that one config
     * file, and one AmiConfig, work whether or not the stack was built with
     * AMINETXDUO_IPV6 -- only the parser and the netstack act on them. In the
     * floor build ip6type is always AMI_IP6TYPE_OFF.
     */
    AmiIp6Type  ip6type;
    ULONG       address6[AMI_CFG_IP6_WORDS]; /* static global address            */
    ULONG       prefix6;                     /* its prefix length, default 64    */
    ULONG       gateway6[AMI_CFG_IP6_WORDS]; /* static default router            */
    BOOL        have_gateway6;
} AmiIfConfig;

/*
 * nameserver_use[] is what ObtainDomainNameServerList() reports as
 * dnsn_UseCount, in that call's own convention: negative means the server was
 * configured statically in DEVS:Internet/name_resolution, positive means it
 * was added at run time by DHCP or AddDomainNameServer(). The magnitude is the
 * number of references either way, because AddDomainNameServer() nests -- see
 * netstack_dns_server_add().
 *
 * A slot in use is never 0, so a zeroed AmiResolverConfig with a non-zero
 * nameserver_count would be malformed; every writer of nameserver[] sets this
 * alongside it.
 */
typedef struct AmiResolverConfig {
    ULONG   nameserver[AMI_CFG_MAX_NAMESERVERS];
    LONG    nameserver_use[AMI_CFG_MAX_NAMESERVERS];
    UWORD   nameserver_count;
    char    domain[AMI_CFG_DOMAIN_LEN];
    char    search[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN];
    UWORD   search_count;
} AmiResolverConfig;

/*
 * One service the user says is listening on this machine. AmiNetXDuo ships no
 * servers, so this is always somebody else's -- an AmiFTPd, a web server, a
 * Samba-alike -- and the file is the user's claim that it is running. Nothing
 * here connects to the port to check.
 */
typedef struct AmiSdService {
    char    type[AMI_CFG_SD_TYPE_LEN];  /* "_ftp._tcp"                          */
    char    name[AMI_CFG_NAME_LEN];     /* instance name; empty = the host name */
    char    txt[AMI_CFG_SD_TXT_LEN];    /* "key=value;key=value"; empty = none  */
    UWORD   port;
} AmiSdService;

/*
 * Where hostname came from. The value is a rank: a source with a higher number
 * outranks every lower one and replaces the name it set.
 *
 * No RFC settles this order; it is an OS convention. name_resolution is the
 * file whose only job is naming this machine, so it wins. DHCP option 12 comes
 * next: it is current, and somebody configured the server on purpose.
 *
 * Interface ID= is last, below even an ambient ENV:HOSTNAME. Roadshow's ID= is
 * free text and is often descriptive; RFC 1123 2.1 refuses "Ariadne in the
 * study" but not "Ethernet", so ranking it higher would silently rename any
 * machine whose owner used the field as documented. The reported fault was
 * never that ENV:HOSTNAME wins -- it was that it won without saying so, and
 * that is answered by reporting the source rather than by reordering.
 */
typedef enum {
    AMI_HOSTNAME_NONE = 0,      /* nothing named this machine                */
    AMI_HOSTNAME_INTERFACE,     /* an interface file's ID=                   */
    AMI_HOSTNAME_ENV,           /* ENV:HOSTNAME                              */
    AMI_HOSTNAME_DHCP,          /* DHCP option 12, RFC 2132 3.14             */
    AMI_HOSTNAME_NAMERES        /* DEVS:Internet/name_resolution             */
} AmiHostnameSource;

typedef struct AmiConfig {
    AmiIfConfig         interfaces[AMI_CFG_MAX_INTERFACES];
    UWORD               interface_count;
    AmiResolverConfig   resolver;
    char                hostname[AMI_CFG_NAME_LEN];
    UWORD               hostname_source;     /* AmiHostnameSource                */
    ULONG               default_gateway;     /* 0 = none / from DHCP             */

    /*
     * Present in every build, like the IPv6 interface fields above: one
     * AmiConfig serves a build with AMINETXDUO_MDNS and one without, and only
     * the loader and the netstack act on these.
     */
    AmiSdService        sd_services[AMI_CFG_MAX_SD_SERVICES];
    UWORD               sd_service_count;
} AmiConfig;

/*
 * Read the Roadshow config layout into *cfg. Missing files are not an error: an
 * empty config yields interface_count == 0 and the caller decides.
 * Returns 0 on success, or a negative AMI_CFG_ERR_* code.
 */
#define AMI_CFG_OK              0
#define AMI_CFG_ERR_NOMEM      (-1)
#define AMI_CFG_ERR_SYNTAX     (-2)
#define AMI_CFG_ERR_IO         (-3)

LONG ami_config_load(AmiConfig *cfg);

/* Parse one interface file by name (DEVS:NetInterfaces/<name>). */
LONG ami_config_load_interface(const char *name, AmiIfConfig *out);

/*
 * Offer `name` as this machine's host name on behalf of `source`. It is taken,
 * and TRUE returned, when `source` ranks at or above whatever set the current
 * name -- so a fresh arrival from the same source (a DHCP renewal) replaces it
 * and a weaker one never does.
 *
 * A source whose text is not a host name by construction is checked against
 * RFC 1123 2.1 first and refused if it fails, which leaves the next source in
 * the chain to answer rather than naming the machine something invalid. An
 * interface ID is free text and DHCP option 12 arrives off the network, so
 * both are checked; a name an administrator wrote in name_resolution or
 * ENV:HOSTNAME is taken as given.
 */
BOOL ami_config_hostname_offer(AmiConfig *cfg, UWORD source, const char *name);

/* RFC 1123 2.1 syntax: dot-separated labels of letters, digits and hyphens,
   no label empty or beginning or ending with one. */
BOOL ami_config_hostname_valid(const char *name);

/* Short name for a source, for a report that says where the name came from.
   NULL for AMI_HOSTNAME_NONE, which is not a source. */
const char *ami_config_hostname_source_text(UWORD source);

/* ------------------------------------------------------------- diagnostics
 *
 * Everything wrong with a configuration file is reported twice: once to
 * ami_log() for whoever is reading the serial port, and once through this hook,
 * which is how a Shell command puts the same fact on the user's screen with a
 * file name, a line number and a suggestion.
 *
 * The hook is optional and global. src/netstack never installs one, because a
 * program that opens bsdsocket.library must not have configuration warnings
 * appear in its output, so the only reporters are the tools, which install one
 * for the duration of a load and remove it afterwards.
 *
 * The AmiCfgProblem and every string in it are valid only for the duration of
 * the call; copy anything you need to keep.
 */
#define AMI_CFG_PROBLEM_ERROR   0   /* the file cannot be used as written  */
#define AMI_CFG_PROBLEM_WARN    1   /* usable, but something was ignored   */

typedef struct AmiCfgProblem {
    const char *file;       /* "DEVS:NetInterfaces/eth0", never NULL       */
    ULONG       line;       /* 1-based; 0 when it is about the whole file  */
    UWORD       severity;   /* AMI_CFG_PROBLEM_*                           */
    const char *text;       /* what is wrong: one sentence, no full stop   */
    const char *hint;       /* what to do about it, or NULL                */
} AmiCfgProblem;

typedef VOID (*AmiCfgReporter)(const AmiCfgProblem *problem, APTR user);

VOID ami_config_set_reporter(AmiCfgReporter reporter, APTR user);

/* Dotted-quad <-> ULONG (host byte order). Returns FALSE on malformed input. */
BOOL  ami_config_parse_ip(const char *text, ULONG *out);
VOID  ami_config_format_ip(ULONG addr, char *buf, ULONG buflen);

/*
 * RFC 4291 text form <-> four host-order ULONGs.
 *
 * ami_config_parse_ip6() accepts the full grammar: eight groups, "::" run
 * compression (at most one), and a trailing dotted quad ("::ffff:10.0.0.1").
 * Leading zeroes inside a group are allowed, more than four hex digits is not.
 *
 * `prefix_out` selects the dialect, which is what lets the two callers share
 * one parser:
 *   - non-NULL: a "/N" suffix is accepted and written there (N in 0..128).
 *     Without a suffix the value is left untouched, so the caller pre-seeds
 *     its default. This is the config-file dialect.
 *   - NULL: a '/' is a syntax error. This is inet_pton()'s dialect.
 *
 * ami_config_format_ip6() writes the RFC 5952 canonical form: lower-case hex,
 * no leading zeroes, "::" over the longest run of two or more zero groups
 * (leftmost wins a tie), and the IPv4 dotted form for v4-mapped addresses.
 * `buflen` must be at least AMI_CFG_IP6_STRLEN; anything shorter yields "".
 */
/*
 * The RFC 4007 forms. ami_config_parse_ip6() refuses a "%zone" rather than
 * dropping it: an address whose zone went missing names a different
 * destination. _zone() takes it as text and leaves resolving a name to an
 * index to the caller, which is the layer that can call if_nametoindex().
 * ami_config_format_ip6_zone() appends one when `zone` is non-empty; deciding
 * whether an address should carry a zone at all is the caller's, since RFC
 * 4007 11.1 excludes global scope and loopback.
 */
BOOL  ami_config_parse_ip6_zone(const char *text, ULONG out[AMI_CFG_IP6_WORDS],
                                ULONG *prefix_out, char *zone_out,
                                ULONG zone_len);
VOID  ami_config_format_ip6_zone(const ULONG addr[AMI_CFG_IP6_WORDS],
                                 const char *zone, char *buf, ULONG buflen);

BOOL  ami_config_parse_ip6(const char *text, ULONG out[AMI_CFG_IP6_WORDS],
                           ULONG *prefix_out);
VOID  ami_config_format_ip6(const ULONG addr[AMI_CFG_IP6_WORDS],
                            char *buf, ULONG buflen);

/* ------------------------------------------------------------------ netdb */

/*
 * Backing store for get{host,net,proto,serv}by* in bsdsocket.library. Reads the
 * DEVS:Internet/{hosts,networks,protocols,services} files, cached in memory.
 */
typedef struct AmiNetdbEntry {
    const char  *name;
    const char **aliases;       /* NULL-terminated */
    ULONG        value;         /* address, net number, protocol number or port */
    const char  *proto;         /* services only, else NULL */
} AmiNetdbEntry;

LONG                 ami_netdb_load(VOID);
VOID                 ami_netdb_free(VOID);
const AmiNetdbEntry *ami_netdb_host_by_name(const char *name);
const AmiNetdbEntry *ami_netdb_host_by_addr(ULONG addr);
const AmiNetdbEntry *ami_netdb_net_by_name(const char *name);
const AmiNetdbEntry *ami_netdb_net_by_addr(ULONG net);
const AmiNetdbEntry *ami_netdb_proto_by_name(const char *name);
const AmiNetdbEntry *ami_netdb_proto_by_number(LONG number);
const AmiNetdbEntry *ami_netdb_serv_by_name(const char *name, const char *proto);
const AmiNetdbEntry *ami_netdb_serv_by_port(LONG port, const char *proto);

/* Iterator support for the get*ent() family. index starts at 0; NULL ends. */
const AmiNetdbEntry *ami_netdb_net_entry(ULONG index);
const AmiNetdbEntry *ami_netdb_proto_entry(ULONG index);
const AmiNetdbEntry *ami_netdb_serv_entry(ULONG index);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CONFIG_H */
