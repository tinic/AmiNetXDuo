/*
 * AmiNetXDuo, parsed configuration.
 *
 * The on-disk format is Roadshow's (see docs/RESEARCH.md §6.6):
 *   DEVS:NetInterfaces/<name>      one file per interface
 *   DEVS:Internet/name_resolution  NAMESERVER / DOMAIN / SEARCH
 *   DEVS:Internet/routes           where Roadshow 1.15 keeps the default route
 *   DEVS:Internet/default_gateway  DEVICE / UNIT / GATEWAY, not Roadshow's but
 *                                  read first, see src/config/config_parse.c:25
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

/*
 * A DEFINITION IS NOT AN ATTACHMENT, and conflating them was the defect.
 *
 * AMI_CFG_MAX_ATTACHED is how many interfaces can be ONLINE AT ONCE.  It is
 * NX_MAX_PHYSICAL_INTERFACES (port/netxduo-amiga/inc/nx_user.h), because every
 * attached interface costs an NX_INTERFACE inside the NX_IP plus a slot in
 * every per-interface table in src/netstack.  That is a real constraint, and
 * the ATTACH path is where it is refused -- out loud, naming what is already
 * up.  It is the only limit on interfaces anywhere in this tree.
 *
 * HOW MANY MAY BE DESCRIBED: any number.  A file in DEVS:NetInterfaces is a
 * description on disk.  It opens no device, creates no NetX Duo object and
 * costs nothing until somebody attaches it, so a machine that keeps five card
 * definitions and brings up whichever two it wants is doing something ordinary
 * and nothing here may stop it.  interfaces[] below is therefore a list that
 * grows, not an array with a ceiling.
 *
 * WHAT USED TO BE HERE was a single constant at 2, and the comment recorded
 * the wrong fix being chosen deliberately: "It was 4, which the stack could
 * parse and NetX Duo could not attach".  The PARSE limit was lowered to the
 * ATTACH limit instead of the attach being taught to refuse clearly, so the
 * third and later files were dropped by the parser (src/config/config_list.c)
 * behind an AMI_WARN that no shipped build compiles -- silently, and in
 * whatever order the directory happened to be read.  Nothing on the machine
 * said an interface had gone.  A card renamed from "eth0" to "wifi" could
 * disappear, and the machine would report the card and the driver as healthy,
 * because as far as the configuration went that interface did not exist.
 *
 * Raising that constant would have been the same defect with a bigger
 * number, and it would have been paid for by every machine that does not need
 * it.  Measured, m68k: sizeof(AmiIfConfig) is 404 bytes, so an array of eight
 * inside this struct is 3,232 of them -- 2,424 bytes (2.4 KB) more than the
 * array of two, on every machine, including the 2 MB A1200 with one card.
 * Holding the list on the end of a pointer instead takes sizeof(AmiConfig)
 * from 4,392 bytes to 3,592, and each description that EXISTS costs 404.  A
 * one-card machine pays 4,400 all in, eight bytes more than it used to; the
 * fixed array of eight would have charged it 6,816 for seven descriptions it
 * does not have.  Unlimited and cheaper, which is the usual shape of a limit
 * that was never buying anything.
 *
 * AMI_CFG_IFACE_FLOOR is a starting capacity and nothing else.  NO REFUSAL
 * HANGS OFF IT and nothing is ever dropped for exceeding it: the list grows
 * past it silently and without limit, which is the whole point.  It equals the
 * attach cap only so that the netstack -- which writes an attached interface
 * into this list at the NetX Duo slot number it was handed, by index -- always
 * has that index available even on a machine whose drawer was empty.
 */
#define AMI_CFG_MAX_ATTACHED        2
#define AMI_CFG_IFACE_FLOOR         AMI_CFG_MAX_ATTACHED
#define AMI_CFG_MAX_NAMESERVERS     4
#define AMI_CFG_MAX_SEARCH          6

/* What ami_config_search_list() can return: every search[] entry, plus the
   DOMAIN line for a file that has no SEARCH line. */
#define AMI_CFG_SEARCH_LIST_MAX     (AMI_CFG_MAX_SEARCH + 1)

#define AMI_CFG_NAME_LEN            64
/* An Ethernet address, for HARDWAREADDRESS.  Every SANA-II device this runs
   on is 6 bytes; a driver with a wider one keeps its own. */
#define AMI_CFG_MAC_SIZE            6
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
 * The type is bounded at RFC 6763 7's grammar, "_" + at most 15 characters
 * + "._tcp", which is 21, so a type the parser accepts always fits the
 * module's NX_MDNS_TYPE_MAX. The TXT field is bounded at the module's
 * NX_MDNS_NAME_MAX of 255.
 */
#define AMI_CFG_MAX_SD_SERVICES     8
#define AMI_CFG_SD_TYPE_LEN         22
#define AMI_CFG_SD_TXT_LEN          256

/*
 * How an interface gets its IPv4 address.
 *
 * STATIC is 0 so a zeroed AmiIfConfig means what the parser's default means,
 * and NONE is last for the same reason: nothing acquires "no IPv4" by being
 * memset.
 *
 * NONE is not "static with no ADDRESS". It is the operator saying this
 * interface carries no IPv4 at all, which is what CONFIGURE=NONE always looked
 * like it said and never did: it parsed to STATIC, and STATIC with no address
 * is the one configuration the parser refuses outright. An interface with
 * ADDRESS6 and no ADDRESS was therefore unconfigurable, and that is the whole
 * of the IPv6-only problem. Nothing waits for an address on a NONE interface
 * and nothing falls back to RFC 3927 on one.
 */
typedef enum {
    AMI_IPTYPE_STATIC = 0,
    AMI_IPTYPE_DHCP,
    AMI_IPTYPE_LINKLOCAL,       /* RFC 3927, used as DHCP fallback */
    AMI_IPTYPE_NONE             /* no IPv4 on this interface at all  */
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
    AMI_IP6TYPE_AUTO,           /* link-local + SLAAC, and whatever the RA's
                                   M and O bits ask DHCPv6 for               */
    AMI_IP6TYPE_STATIC,         /* link-local + the configured global address */
    AMI_IP6TYPE_DHCP            /* link-local + stateful DHCPv6, no RA needed */
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

    /*
     * CARD. Which board the driver should bind to, by name, when one device
     * file covers a family of them: anxnet.device is opened for every
     * NE2000/DP8390 card, so UNIT alone only says "the Nth board in probe
     * order". Empty means the driver decides from UNIT, which is right on a
     * machine with one card. The name goes to the driver as S2_AnxCardType and
     * a name the hardware does not match fails the open rather than binding to
     * a different board.
     */
    char        card[AMI_CFG_NAME_LEN];
    AmiIpType   iptype;
    ULONG       address;                     /* host byte order; 0 if unset      */
    ULONG       netmask;
    ULONG       gateway;
    ULONG       mtu;                         /* 0 = ask the driver               */
    BOOL        up;                          /* bring online at startup          */
    BOOL        configured;                  /* slot in use                      */
    BOOL        down_goes_offline;           /* IFA_DownGoesOffline; default FALSE */

    /*
     * REQUIRESINITDELAY. A second between opening the device and the first
     * packet, for a card that needs it. Roadshow's manual introduces the
     * option with "the original Ariadne is one such device", and its default
     * is YES; ours is NO, because a second at every bring-up on every card is
     * a high price for the one that needs it, and the cards we can test do
     * not.  A user with such a card can say so.
     */
    BOOL        requires_init_delay;

    /*
     * HARDWAREADDRESS. The address to configure the card with, instead of the
     * one it came with. Two emulated guests on one bridge ship the same
     * factory address and collide; so do two of some cards.
     */
    BOOL        have_hw_address;
    UBYTE       hw_address[AMI_CFG_MAC_SIZE];

    /*
     * Answer .local queries on this interface.  MDNS=YES in the interface
     * file; DEFAULT IS OFF, and a file that does not mention it gets no
     * responder.
     *
     * Off by default because the cost is not small and is paid whether or not
     * anything asks: the responder joins 224.0.0.251 on the interface, so
     * every multicast query on that wire becomes work for this machine, and
     * it measurably slows the stack on a 68000.  Per interface because that
     * cost is per interface.
     *
     * Present in every build, like the IPv6 fields below; only the parser and
     * the netstack act on it, and a build without mDNS ignores it.
     */
    BOOL        mdns;

    /*
     * IPv6. These fields exist in both build configurations so that one config
     * file, and one AmiConfig, work whether or not the stack was built with
     * AMINETXDUO_IPV6, only the parser and the netstack act on them. In the
     * floor build ip6type is always AMI_IP6TYPE_OFF.
     */
    AmiIp6Type  ip6type;
    ULONG       address6[AMI_CFG_IP6_WORDS]; /* static global address            */
    ULONG       prefix6;                     /* its prefix length, default 64    */
    ULONG       gateway6[AMI_CFG_IP6_WORDS]; /* static default router            */
    BOOL        have_gateway6;
} AmiIfConfig;

/*
 * Does this interface expect an IPv4 address, and does it expect an IPv6 one.
 *
 * "Expect" is the question bring-up asks before it waits, and the question a
 * tool asks before it reports "no address yet".  An interface with
 * CONFIGURE=NONE expects no IPv4; one whose CONFIGURE is STATIC with no
 * ADDRESS line expects none either, because there is nothing left that could
 * supply one.  Both used to be indistinguishable from "waiting", which is what
 * made an IPv6-only machine look broken all the way up the stack.
 *
 * ami_config_iface_wants_ipv6() answers for the IPv6 build only: in the floor
 * build ip6type is always AMI_IP6TYPE_OFF and it is always FALSE.  It is TRUE
 * for LINKLOCAL as well, because fe80::/64 is an address the interface will
 * have and can be reached on.
 */
BOOL ami_config_iface_wants_ipv4(const AmiIfConfig *cfg);
BOOL ami_config_iface_wants_ipv6(const AmiIfConfig *cfg);

/*
 * nameserver_use[] is what ObtainDomainNameServerList() reports as
 * dnsn_UseCount, in that call's own convention: negative means the server was
 * configured statically in DEVS:Internet/name_resolution, positive means it
 * was added at run time by DHCP or AddDomainNameServer(). The magnitude is the
 * number of references either way, because AddDomainNameServer() nests, see
 * netstack_dns_server_add().
 *
 * A slot in use is never 0, so a zeroed AmiResolverConfig with a non-zero
 * nameserver_count would be malformed; every writer of nameserver[] sets this
 * alongside it.
 *
 * nameserver6[] is the same list for IPv6 servers, which the file cannot hold:
 * name_resolution's NAMESERVER line is a dotted quad. Everything in it arrived
 * in a router advertisement's RFC 8106 option, so every use count is positive,
 * and it is a separate array rather than a family tag on the first because
 * every existing reader of nameserver[] takes a ULONG and would have had to
 * change to ignore entries it cannot represent.
 *
 * search[] holds the file's SEARCH line first and the network's domains after
 * it, and search_static says where the join is: entries below it came from
 * DEVS:Internet/name_resolution, entries from it up came off the network (the
 * DHCP lease's option 119 list in its own order, then its option 15, then a
 * router advertisement's RFC 8106 5.2 list). That is the order a name with no
 * dot is tried in, see ami_ns_search_list() in src/netstack/netstack_dns.c,
 * and it is the AmiHostnameSource ranking applied to a list: name_resolution
 * outranks the network, so it goes first rather than making the network's
 * domains go away.
 *
 * search_use[] counts independent network owners of each dynamic entry.  The
 * ordinary offer/withdraw API remains set-like for DHCP imports and Roadshow
 * compatibility; the reference pair is for a source such as RFC 8106 whose
 * entries can be withdrawn without taking an identical suffix learned from
 * another source with them.  Static entries do not need a count because
 * search_static makes them permanent.
 *
 * Between the two network sources there is no ranking to make -- neither is
 * more authoritative than the other -- so they are in arrival order, which is
 * the lease's and then the advertisement's: the lease is drained once at
 * ami_netstack_dns_start() and an advertisement is drained on the resolver
 * path, which is always later.
 */
typedef struct AmiResolverConfig {
    ULONG   nameserver[AMI_CFG_MAX_NAMESERVERS];
    LONG    nameserver_use[AMI_CFG_MAX_NAMESERVERS];
    UWORD   nameserver_count;
    ULONG   nameserver6[AMI_CFG_MAX_NAMESERVERS][AMI_CFG_IP6_WORDS];
    UWORD   nameserver6_count;
    char    domain[AMI_CFG_DOMAIN_LEN];
    char    search[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN];
    UWORD   search_use[AMI_CFG_MAX_SEARCH];
    UWORD   search_count;
    UWORD   search_static;
} AmiResolverConfig;

/*
 * One service the user says is listening on this machine. AmiNetXDuo ships no
 * servers, so this is always somebody else's, an AmiFTPd, a web server, a
 * Samba-alike, and the file is the user's claim that it is running. Nothing
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
 * never that ENV:HOSTNAME wins, it was that it won without saying so, and
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
    /*
     * Every interface DESCRIBED, and there is no limit on how many: see the
     * head of this file.  A LIST THAT GROWS, not an array -- interface_count
     * entries are live and interface_capacity are allocated.  NULL with a count of
     * zero is a valid empty configuration and is what a zeroed AmiConfig is.
     *
     * Subscripting is unchanged, `cfg->interfaces[i]` reads the same on a
     * pointer as it did on an array, which is why the forty-odd readers of it
     * across src/netstack, src/bsdsocket and src/tools did not have to move.
     * What DID have to move is the ownership: whoever loads one frees it, see
     * ami_config_free() below.
     *
     * interface_count can exceed AMI_CFG_MAX_ATTACHED, and does on any machine
     * with three cards described.  A reader that means "a slot NetX Duo has"
     * must bound its index by AMI_CFG_MAX_ATTACHED as well as by the count.
     */
    AmiIfConfig        *interfaces;
    UWORD               interface_count;
    UWORD               interface_capacity;
    AmiResolverConfig   resolver;
    char                hostname[AMI_CFG_NAME_LEN];
    UWORD               hostname_source;     /* AmiHostnameSource                */
    ULONG               default_gateway;     /* 0 = none / from DHCP             */

    /*
     * Publish the TCP: device. TRUE unless DEVS:Internet/tcp_handler turns it
     * off, because a machine that used `Type TCP:host/daytime` yesterday must
     * still do so after an upgrade. ami_config_load() sets it; anything that
     * merely zeroes an AmiConfig and reads this gets "off", which is why the
     * one caller that acts on it treats a missing config as "on".
     */
    BOOL                tcp_handler;

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
 *
 * OWNER FREES.  interfaces[] is allocated here, so every caller must call
 * ami_config_free() before it lets the AmiConfig go, and a command that can
 * leave from several places needs one exit that does it -- the shape
 * ami_netdb_free() already forced on ping, ShowNetStatus and AddNetRoute
 * (a body function called from a main() that does the cleanup).  A library
 * allocation leaks until the library is expunged and AmigaOS reclaims nothing
 * a Process did not free, so this is not a tidy-up: see docs/ALLOCATIONS.md.
 *
 * *cfg is OVERWRITTEN, not merged: the struct is zeroed first.  Loading twice
 * into the same AmiConfig without an ami_config_free() between the two loses
 * the first list.
 */
#define AMI_CFG_OK              0
#define AMI_CFG_ERR_NOMEM      (-1)
#define AMI_CFG_ERR_SYNTAX     (-2)
#define AMI_CFG_ERR_IO         (-3)

LONG ami_config_load(AmiConfig *cfg);

/*
 * Give back everything ami_config_load() took, and leave *cfg a valid empty
 * configuration.  Safe on a zeroed AmiConfig, safe to call twice, and safe on
 * one that a failed load left half-built.
 */
VOID ami_config_free(AmiConfig *cfg);

/*
 * Make sure interfaces[] can hold at least `want` descriptions, growing it if
 * it cannot.  TRUE when it can afterwards, FALSE only when memory ran out --
 * never because `want` was judged too large.  Existing entries are carried
 * over; new ones are zeroed.
 *
 * Callers that write a slot by index rather than by appending -- the netstack,
 * which places an attached interface at the NetX Duo slot number it was given
 * -- must reserve first.  The parser appends and grows on its own.
 */
BOOL ami_config_reserve(AmiConfig *cfg, UWORD want);

/* Parse one interface file by name (DEVS:NetInterfaces/<name>). */
LONG ami_config_load_interface(const char *name, AmiIfConfig *out);

/*
 * Offer `name` as this machine's host name on behalf of `source`. It is taken,
 * and TRUE returned, when `source` ranks at or above whatever set the current
 * name, so a fresh arrival from the same source (a DHCP renewal) replaces it
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

/*
 * The packet pool's share of free memory, as the divisor D in avail / D.
 *
 * `fallback` unless ENV:ANXDPOOLDIV holds a number from 4 to 64.  The variable
 * exists for the low-memory receive experiment of docs/PHYSICAL_RX_A1200.md:
 * one binary serves every arm of an A/B, because the arms differ by a SetEnv
 * and not by a build, and a measurement over two binaries measures the
 * binaries.  An absent variable is the shipped behaviour exactly.
 */
ULONG ami_config_pool_divisor(ULONG fallback);

/*
 * "amiga-490007" from 00:80:10:49:00:07: the name a machine none of the four
 * sources named answers to, so that two unconfigured machines on one segment
 * do not both claim amiga.local. The last three octets in lower-case hex,
 * which is stable for as long as the card is.
 *
 * FALSE, and *out untouched, when hw is NULL, shorter than three bytes, all
 * zero, or `size` cannot hold the twelve characters and the terminator. The
 * caller keeps whatever fallback it had; nothing here invents a suffix.
 */
BOOL ami_config_hostname_from_hwaddr(const UBYTE *hw, ULONG hwlen,
                                     char *out, ULONG size);

/* Short name for a source, for a report that says where the name came from.
   NULL for AMI_HOSTNAME_NONE, which is not a source. */
const char *ami_config_hostname_source_text(UWORD source);

/*
 * The suffixes a name with no dot is tried under, in the order they are tried,
 * as pointers into `res`. Returns how many were written, never more than
 * AMI_CFG_SEARCH_LIST_MAX.
 *
 *   1. SEARCH from DEVS:Internet/name_resolution, as written;
 *   2. DOMAIN from that file, when the file has no SEARCH line;
 *   3. what the DHCP lease supplied.
 *
 * That is the AmiHostnameSource ranking -- name_resolution above DHCP -- with
 * one difference, stated because it is a difference: a host name is one string
 * so the losing source is dropped, a suffix list is a list so the losing
 * source goes after rather than away. Nothing here decides which domain a
 * machine is on; every suffix is a guess, and the cheapest correct thing is to
 * try the administrator's guesses first.
 *
 * Duplicates are dropped case-insensitively (RFC 4343): a DHCP machine whose
 * file names no domain has the lease's domain in both DOMAIN and search[], and
 * must not pay two queries for it.
 */
UWORD ami_config_search_list(const AmiResolverConfig *res, const char *out[],
                             UWORD max);

/*
 * Offer one search domain on behalf of the network -- a DHCP lease, or a router
 * advertisement's RFC 8106 5.2 option. It is appended after whatever
 * DEVS:Internet/name_resolution put in search[], never before it, and a name
 * already in the list at any rank is dropped rather than repeated,
 * case-insensitively (RFC 4343) -- a source that names the domain another
 * already names must not cost a second query. Returns TRUE if it was stored.
 *
 * Anything that is not an RFC 1123 2.1 name is refused: this text arrives off
 * the network, and a suffix is pasted onto a name that is then queried.
 */
BOOL ami_config_search_offer(AmiResolverConfig *res, const char *domain);

/*
 * Take one search domain back, for a lifetime of zero (RFC 8106 5.2). Only a
 * domain the network put there is removed; an entry below search_static came
 * from DEVS:Internet/name_resolution and no router may retract what the
 * administrator wrote. Returns TRUE if one was removed.
 */
BOOL ami_config_search_withdraw(AmiResolverConfig *res, const char *domain);

/*
 * Add or remove one source's ownership of a network search suffix.  Adding an
 * existing dynamic suffix deepens its count; removing it only erases the
 * suffix when its last owner leaves.  A static suffix can be acquired and
 * released, but remains the administrator's throughout.  TRUE means the
 * requested ownership operation was accepted.
 */
BOOL ami_config_search_reference_add(AmiResolverConfig *res,
                                     const char *domain);
BOOL ami_config_search_reference_remove(AmiResolverConfig *res,
                                        const char *domain);

/*
 * The same pair for nameserver6[], the servers a router advertisement's RFC
 * 8106 5.1 option names. TRUE when the list changed.
 *
 * Offering one already there changes nothing and returns FALSE, because every
 * advertisement repeats the option; offering one past AMI_CFG_MAX_NAMESERVERS
 * is refused and leaves the servers that are answering alone, rather than
 * evicting one for a new arrival the router listed last.
 */
BOOL ami_config_nameserver6_offer(AmiResolverConfig *res,
                                  const ULONG addr[AMI_CFG_IP6_WORDS]);
BOOL ami_config_nameserver6_withdraw(AmiResolverConfig *res,
                                     const ULONG addr[AMI_CFG_IP6_WORDS]);

/*
 * RFC 3397 option 119: a run of RFC 1035 4.1.4 encoded names, root label
 * ending each one, with compression pointers taken from the start of the
 * option data. Each name decoded is handed to ami_config_search_offer().
 * Returns how many were stored.
 *
 * RFC 8106 5.2's DNSSL option carries the same encoding, minus compression,
 * and is decoded by the same function: the payload is padded to an 8-byte
 * boundary with zero octets, and a zero octet is a root label with no labels
 * in front of it, which ends the walk exactly as a truncated name would.
 *
 * `data` is off the network and is not trusted: a pointer that does not go
 * strictly backwards, a label that runs past the end, or a name longer than
 * the list can hold ends the walk, and what was decoded before it stands.
 */
UWORD ami_config_search_from_rfc3397(AmiResolverConfig *res,
                                     const UBYTE *data, ULONG len);

/*
 * The same walk, taking each name decoded back out of the list rather than
 * putting it in. Returns how many were removed.
 */
UWORD ami_config_search_withdraw_rfc3397(AmiResolverConfig *res,
                                         const UBYTE *data, ULONG len);

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
