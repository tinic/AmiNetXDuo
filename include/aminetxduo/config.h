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
