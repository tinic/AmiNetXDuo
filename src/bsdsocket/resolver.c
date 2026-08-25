/*
 * bsdsocket.library, gethostby*, gethostname, gethostid.
 *
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/* Resolver timeout, in ThreadX ticks. */
#define BSD_RESOLVE_TIMEOUT     (30UL * (ULONG)NX_IP_PERIODIC_RATE)

#define BSD_HOSTNAME_TIMEOUT    (5UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * Fill in the per-opener hostent, which the non-reentrant calls return.
 */
static struct hostent *bsd_hostent_fill(struct AmiSocketBase *base,
                                        const char *name, ULONG addr,
                                        const char **aliases)
{
    /* gethostbyaddr() resolves straight into sb_HostName, so skip the copy. */
    if (name != base->sb_HostName)
        bsd_strncpy(base->sb_HostName, name, sizeof(base->sb_HostName));

    base->sb_HostAddr = BSD_HTONL(addr);

    base->sb_HostAddrList[0] = (char *)&base->sb_HostAddr;
    base->sb_HostAddrList[1] = NULL;
    base->sb_HostAliases[0]  = NULL;

    base->sb_HostEnt.h_name      = (__STRPTR)base->sb_HostName;
    base->sb_HostEnt.h_aliases   = (aliases != NULL)
                                       ? (__STRPTR *)aliases
                                       : (__STRPTR *)base->sb_HostAliases;
    base->sb_HostEnt.h_addrtype  = AF_INET;
    base->sb_HostEnt.h_length    = 4;
    base->sb_HostEnt.h_addr_list = base->sb_HostAddrList;

    bsd_set_herrno(base, NETDB_SUCCESS);

    return &base->sb_HostEnt;
}

/*
 * The _r variants pack the same shape into the caller's buffer:
 * all longword aligned, which is what m68k needs, and which the caller's
 * buffer does not supply.  `buf` is an APTR from an application.  Every field
 * above is written through a pointer derived from it, so an odd `buf` is an
 * address error on a 68000 at the first store.  The buffer is stepped up to a
 * longword boundary here and the slack counted against buflen.  The shape
 */
static struct hostent *bsd_hostent_pack(struct AmiSocketBase *base,
                                        struct hostent *hp, APTR buf,
                                        ULONG buflen, LONG *he,
                                        const char *name, ULONG addr)
{
    UBYTE  *p = (UBYTE *)buf;
    ULONG   namelen = bsd_strlen(name) + 1;
    ULONG   slack;
    ULONG   need;
    char  **addr_list;
    char  **aliases;
    ULONG  *address;

    if (hp == NULL || buf == NULL)
    {
        if (he != NULL)
            *he = NETDB_INTERNAL;
        bsd_set_errno(base, AMI_EFAULT);
        return NULL;
    }

    slack = (ULONG)((0UL - (ULONG)p) & 3UL);
    p    += slack;
    need  = slack + 3 * sizeof(char *) + sizeof(ULONG) + namelen;

    if (buflen < need)
    {
        if (he != NULL)
            *he = NETDB_INTERNAL;
        bsd_set_errno(base, AMI_ERANGE);
        return NULL;
    }

    addr_list = (char **)p;
    p += 2 * sizeof(char *);
    aliases   = (char **)p;
    p += sizeof(char *);
    address   = (ULONG *)p;
    p += sizeof(ULONG);

    *address     = BSD_HTONL(addr);
    addr_list[0] = (char *)address;
    addr_list[1] = NULL;
    aliases[0]   = NULL;

    bsd_strncpy((char *)p, name, namelen);

    hp->h_name      = (__STRPTR)p;
    hp->h_aliases   = (__STRPTR *)aliases;
    hp->h_addrtype  = AF_INET;
    hp->h_length    = 4;
    hp->h_addr_list = addr_list;

    if (he != NULL)
        *he = NETDB_SUCCESS;

    return hp;
}

static LONG bsd_herrno_of(LONG status)
{
    switch (status)
    {
        case AMI_NET_ERR_NONAME:    return HOST_NOT_FOUND;

        case AMI_NET_ERR_TIMEOUT:                   /* nobody answered      */
        case AMI_NET_ERR_NOSERVER:                  /* nobody to ask, yet   */
        case AMI_NET_ERR_STATE:                     /* stack not up, yet    */
        case AMI_NET_ERR_ABORTED:                   /* Ctrl-C, nothing learnt */
            return TRY_AGAIN;

        case AMI_NET_ERR_CONFIG:
        case AMI_NET_ERR_KERNEL:
        case AMI_NET_ERR_NOMEM:
            return NO_RECOVERY;

        default:                    return HOST_NOT_FOUND;
    }
}

/*
 * What lets Ctrl-C out of a lookup.
 */
BOOL bsd_resolve_break(VOID *arg)
{
    struct AmiSocketBase *base = (struct AmiSocketBase *)arg;

    if (base == NULL || base->sb_BreakMask == 0)
        return FALSE;

    return (BOOL)((SetSignal(0UL, 0UL) & base->sb_BreakMask) != 0);
}

/*
 * Shared lookup: dotted quad first, then the resolver.
 */
static LONG bsd_resolve_name(struct AmiSocketBase *base, const char *name,
                             ULONG *addr, LONG *he_out)
{
    struct in_addr literal;
    LONG           status;

    if (name == NULL || *name == '\0')
    {
        *he_out = HOST_NOT_FOUND;
        return -1;
    }

    literal.s_addr = 0;
    if (bsd_inet_aton((STRPTR)name, &literal, base) != 0)
    {
        *addr   = BSD_NTOHL((ULONG)literal.s_addr);
        *he_out = NETDB_SUCCESS;
        return 0;
    }

    status = netstack_resolve_until(name, addr, BSD_RESOLVE_TIMEOUT,
                                    bsd_resolve_break, base);
    if (status != AMI_NET_OK)
    {
        if (status == AMI_NET_ERR_ABORTED)
            bsd_set_errno(base, AMI_EINTR);

        *he_out = bsd_herrno_of(status);
        return -1;
    }

    *he_out = NETDB_SUCCESS;

    return 0;
}

/*
 * h_name is "official name of the host" (autodoc), which is not the string
 * the caller asked about whenever the answer came from an alias. The local
 * database knows both, so a DEVS:Internet/hosts answer gets that entry's own
 * name and alias vector.
 */
struct hostent *bsd_gethostbyname(register STRPTR name __asm("a0"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiNetdbEntry *entry;
    ULONG                addr = 0;
    LONG                 he   = NETDB_SUCCESS;

    if (bsd_resolve_name(SocketBase, (const char *)name, &addr, &he) != 0)
    {
        bsd_set_herrno(SocketBase, he);
        return NULL;
    }

    entry = ami_netdb_host_by_name((const char *)name);
    if (entry != NULL)
        return bsd_hostent_fill(SocketBase, entry->name, addr, entry->aliases);

    return bsd_hostent_fill(SocketBase, (const char *)name, addr, NULL);
}

struct hostent *bsd_gethostbyname_r(register STRPTR name           __asm("a0"),
                                    register struct hostent *hp    __asm("a1"),
                                    register APTR buf              __asm("a2"),
                                    register ULONG buflen          __asm("d0"),
                                    register LONG *he              __asm("a3"),
                                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiNetdbEntry *entry;
    ULONG                addr = 0;
    LONG                 code = NETDB_SUCCESS;

    if (bsd_resolve_name(SocketBase, (const char *)name, &addr, &code) != 0)
    {
        if (he != NULL)
            *he = code;
        return NULL;
    }

    entry = ami_netdb_host_by_name((const char *)name);

    return bsd_hostent_pack(SocketBase, hp, buf, buflen, he,
                            (entry != NULL) ? entry->name : (const char *)name,
                            addr);
}

static LONG bsd_resolve_addr(struct AmiSocketBase *base, STRPTR addr_bytes,
                             LONG len, LONG type, ULONG *addr, char *name,
                             ULONG name_len, LONG *he_out)
{
    LONG status;

    if (type != AF_INET || len != 4 || addr_bytes == NULL)
    {
        *he_out = NO_RECOVERY;
        bsd_set_errno(base, AMI_EAFNOSUPPORT);
        return -1;
    }

    /* The caller hands us four bytes in network order. */
    *addr = ((ULONG)(UBYTE)addr_bytes[0] << 24) |
            ((ULONG)(UBYTE)addr_bytes[1] << 16) |
            ((ULONG)(UBYTE)addr_bytes[2] <<  8) |
             (ULONG)(UBYTE)addr_bytes[3];

    status = netstack_resolve_reverse_until(*addr, name, name_len,
                                            BSD_RESOLVE_TIMEOUT,
                                            bsd_resolve_break, base);
    if (status != AMI_NET_OK)
    {
        if (status == AMI_NET_ERR_ABORTED)
            bsd_set_errno(base, AMI_EINTR);

        *he_out = bsd_herrno_of(status);
        return -1;
    }

    *he_out = NETDB_SUCCESS;

    return 0;
}

struct hostent *bsd_gethostbyaddr(register STRPTR addr __asm("a0"),
                                  register LONG len    __asm("d0"),
                                  register LONG type   __asm("d1"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiNetdbEntry *entry;
    ULONG                value = 0;
    LONG                 he    = NETDB_SUCCESS;

    if (bsd_resolve_addr(SocketBase, addr, len, type, &value,
                         SocketBase->sb_HostName,
                         sizeof(SocketBase->sb_HostName), &he) != 0)
    {
        bsd_set_herrno(SocketBase, he);
        return NULL;
    }

    entry = ami_netdb_host_by_addr(value);

    /* sb_HostName already holds the answer. Fill in around it. */
    return bsd_hostent_fill(SocketBase, SocketBase->sb_HostName, value,
                            (entry != NULL) ? entry->aliases : NULL);
}

struct hostent *bsd_gethostbyaddr_r(register STRPTR addr        __asm("a0"),
                                    register LONG len           __asm("d0"),
                                    register LONG type          __asm("d1"),
                                    register struct hostent *hp __asm("a1"),
                                    register APTR buf           __asm("a2"),
                                    register ULONG buflen       __asm("d2"),
                                    register LONG *he           __asm("a3"),
                                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    char  name[256];
    ULONG value = 0;
    LONG  code  = NETDB_SUCCESS;

    if (bsd_resolve_addr(SocketBase, addr, len, type, &value,
                         name, sizeof(name), &code) != 0)
    {
        if (he != NULL)
            *he = code;
        return NULL;
    }

    return bsd_hostent_pack(SocketBase, hp, buf, buflen, he, name, value);
}

/*
 * "The returned name is null-terminated unless insufficient space is
 * provided" (autodoc), and the ERRORS list is EFAULT and EPERM only, so a
 * name that does not fit is truncated into the buffer and the call still
 */
static VOID bsd_hostname_out(char *dst, ULONG size, const char *src)
{
    ULONG i;

    for (i = 0; i < size && src[i] != '\0'; i++)
        dst[i] = src[i];

    if (i < size)
        dst[i] = '\0';
}

/* "the first interface that is online" (autodoc NOTES). */
static ULONG bsd_first_online_address(VOID)
{
    NX_IP *ip = netstack_ip();
    UINT   i;

    if (ip == NULL)
        return 0;

    for (i = 0; i < NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NX_INTERFACE *nxif = &ip->nx_ip_interface[i];

        if (nxif->nx_interface_valid == 0 || nxif->nx_interface_link_up == 0)
            continue;
        if (nxif->nx_interface_ip_address != 0)
            return nxif->nx_interface_ip_address;
    }

    return 0;
}

/*
 * The autodoc's chain is: the first online interface's address looked up in
 * the host database, then reverse resolution, then the HOSTNAME environment
 * variable, then "localhost".
 */
int bsd_gethostname(register char *name     __asm("a0"),
                    register size_t namelen __asm("d0"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiConfig *cfg;
    char             derived[256];       /* MAXHOSTNAMELEN, per BUGS */
    ULONG            address;

    if (name == NULL || namelen == 0)
        return (int)bsd_fail(SocketBase, AMI_EFAULT);

    /* Option 12 arrives on a NetX task, which cannot apply it. Absorb before
       reading cfg->hostname, or this reports the name from before the lease
       until some unrelated lookup happens to do it. */
    netstack_dns_absorb_pending();

    cfg = netstack_config();

    if (cfg != NULL && cfg->hostname[0] != '\0')
    {
        bsd_hostname_out(name, (ULONG)namelen, cfg->hostname);
        return 0;
    }

    derived[0] = '\0';
    address    = bsd_first_online_address();

    if (address != 0 &&
        netstack_resolve_reverse(address, derived, sizeof(derived),
                                 BSD_HOSTNAME_TIMEOUT) == AMI_NET_OK &&
        derived[0] != '\0')
    {
        bsd_hostname_out(name, (ULONG)namelen, derived);
        return 0;
    }

    bsd_hostname_out(name, (ULONG)namelen, "localhost");

    return 0;
}

long bsd_gethostid(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP *ip = netstack_ip();

    (VOID)SocketBase;

    if (ip == NULL)
        return 0;

    /* The primary interface address, in network order (identity on m68k). */
    return (long)BSD_HTONL(ip->nx_ip_interface[0].nx_interface_ip_address);
}
