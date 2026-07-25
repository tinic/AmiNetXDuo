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

#define AMI_CFG_MAX_INTERFACES      4
#define AMI_CFG_MAX_NAMESERVERS     4
#define AMI_CFG_MAX_SEARCH          6
#define AMI_CFG_NAME_LEN            64
#define AMI_CFG_PATH_LEN            128

typedef enum {
    AMI_IPTYPE_STATIC = 0,
    AMI_IPTYPE_DHCP,
    AMI_IPTYPE_LINKLOCAL        /* RFC 3927, used as DHCP fallback */
} AmiIpType;

/*
 * How an interface gets its IPv6 addresses. Every mode except OFF configures
 * the fe80::/64 link-local address derived from the MAC (RFC 4291 modified
 * EUI-64) -- that one needs no router, no server and no configuration, and is
 * the only mode guaranteed to work on an isolated Amiga.
 */
typedef enum {
    AMI_IP6TYPE_OFF = 0,        /* no IPv6 on this interface                  */
    AMI_IP6TYPE_LINKLOCAL,      /* fe80::/64 from the MAC, nothing else       */
    AMI_IP6TYPE_AUTO,           /* link-local + RFC 4862 SLAAC from RAs       */
    AMI_IP6TYPE_STATIC          /* link-local + the configured global address */
} AmiIp6Type;

/*
 * An IPv6 address as four ULONGs in HOST byte order, [0] most significant.
 * This is NetX Duo's own representation (NXD_ADDRESS.nxd_ip_address.v6), and
 * matching it means no conversion anywhere between the config file and the
 * stack. It is NOT the byte order of struct in6_addr -- src/bsdsocket/ has the
 * conversion, which on m68k is a straight copy but is spelled out anyway.
 */
#define AMI_CFG_IP6_WORDS           4

/* Longest RFC 5952 text form plus NUL: "0:0:0:0:0:ffff:255.255.255.255". */
#define AMI_CFG_IP6_STRLEN          46

typedef struct AmiIfConfig {
    char        name[AMI_CFG_NAME_LEN];      /* interface name, e.g. "eth0"      */
    char        device[AMI_CFG_PATH_LEN];    /* SANA-II device, e.g. "a2065.device" */
    ULONG       unit;
    AmiIpType   iptype;
    ULONG       address;                     /* host byte order; 0 if unset      */
    ULONG       netmask;
    ULONG       gateway;
    ULONG       mtu;                         /* 0 = ask the driver               */
    BOOL        up;                          /* bring online at startup          */
    BOOL        configured;                  /* slot in use                      */

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

typedef struct AmiResolverConfig {
    ULONG   nameserver[AMI_CFG_MAX_NAMESERVERS];
    UWORD   nameserver_count;
    char    domain[AMI_CFG_NAME_LEN];
    char    search[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN];
    UWORD   search_count;
} AmiResolverConfig;

typedef struct AmiConfig {
    AmiIfConfig         interfaces[AMI_CFG_MAX_INTERFACES];
    UWORD               interface_count;
    AmiResolverConfig   resolver;
    char                hostname[AMI_CFG_NAME_LEN];
    ULONG               default_gateway;     /* 0 = none / from DHCP             */
} AmiConfig;

/*
 * Read the Roadshow config layout into *cfg. Missing files are not an error --
 * an empty config yields interface_count == 0 and the caller decides.
 * Returns 0 on success, or a negative AMI_CFG_ERR_* code.
 */
#define AMI_CFG_OK              0
#define AMI_CFG_ERR_NOMEM      (-1)
#define AMI_CFG_ERR_SYNTAX     (-2)
#define AMI_CFG_ERR_IO         (-3)

LONG ami_config_load(AmiConfig *cfg);

/* Parse one interface file by name (DEVS:NetInterfaces/<name>). */
LONG ami_config_load_interface(const char *name, AmiIfConfig *out);

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
 * `prefix_out` selects the dialect, which is the whole reason the two callers
 * can share one parser:
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
