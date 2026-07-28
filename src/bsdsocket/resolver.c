/*
 * bsdsocket.library -- gethostby*, gethostname, gethostid.
 *
 * Everything routes through netstack_resolve()/netstack_resolve_reverse()
 * (include/aminetxduo/netstack.h), which drives the NetX Duo DNS client and
 * the name_resolution config. This file only handles the Amiga side: a
 * per-opener struct hostent for the non-reentrant calls, a caller-supplied
 * buffer for the _r ones, and h_errno.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/* Resolver timeout, in ThreadX ticks. */
#define BSD_RESOLVE_TIMEOUT     (30UL * (ULONG)NX_IP_PERIODIC_RATE)

/* Fill in the per-opener hostent, which the non-reentrant calls return. */
static struct hostent *bsd_hostent_fill(struct AmiSocketBase *base,
                                        const char *name, ULONG addr)
{
    /* gethostbyaddr() resolves straight into sb_HostName, so skip the copy. */
    if (name != base->sb_HostName)
        bsd_strncpy(base->sb_HostName, name, sizeof(base->sb_HostName));

    base->sb_HostAddr = BSD_HTONL(addr);

    base->sb_HostAddrList[0] = (char *)&base->sb_HostAddr;
    base->sb_HostAddrList[1] = NULL;
    base->sb_HostAliases[0]  = NULL;

    base->sb_HostEnt.h_name      = (__STRPTR)base->sb_HostName;
    base->sb_HostEnt.h_aliases   = (__STRPTR *)base->sb_HostAliases;
    base->sb_HostEnt.h_addrtype  = AF_INET;
    base->sb_HostEnt.h_length    = 4;
    base->sb_HostEnt.h_addr_list = base->sb_HostAddrList;

    bsd_set_herrno(base, NETDB_SUCCESS);

    return &base->sb_HostEnt;
}

/*
 * The _r variants pack the same shape into the caller's buffer:
 *
 *   char *addr_list[2] | char *aliases[1] | ULONG address | name
 *
 * all longword aligned, which is what m68k needs.
 */
static struct hostent *bsd_hostent_pack(struct AmiSocketBase *base,
                                        struct hostent *hp, APTR buf,
                                        ULONG buflen, LONG *he,
                                        const char *name, ULONG addr)
{
    UBYTE  *p = (UBYTE *)buf;
    ULONG   namelen = bsd_strlen(name) + 1;
    ULONG   need    = 3 * sizeof(char *) + sizeof(ULONG) + namelen;
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

/* Shared lookup: dotted quad first, then the resolver. */
static LONG bsd_resolve_name(struct AmiSocketBase *base, const char *name,
                             ULONG *addr, LONG *he_out)
{
    in_addr_t literal;
    LONG      status;

    if (name == NULL || *name == '\0')
    {
        *he_out = HOST_NOT_FOUND;
        return -1;
    }

    literal = bsd_inet_addr((STRPTR)name, base);
    if (literal != (in_addr_t)0xffffffffUL)
    {
        *addr   = BSD_NTOHL(literal);
        *he_out = NETDB_SUCCESS;
        return 0;
    }

    status = netstack_resolve(name, addr, BSD_RESOLVE_TIMEOUT);
    if (status != AMI_NET_OK)
    {
        *he_out = (status == AMI_NET_ERR_STATE) ? TRY_AGAIN : HOST_NOT_FOUND;
        return -1;
    }

    *he_out = NETDB_SUCCESS;

    return 0;
}

/* ---------------------------------------------------------------- vectors -- */

struct hostent *bsd_gethostbyname(register STRPTR name __asm("a0"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr = 0;
    LONG  he   = NETDB_SUCCESS;

    if (bsd_resolve_name(SocketBase, (const char *)name, &addr, &he) != 0)
    {
        bsd_set_herrno(SocketBase, he);
        return NULL;
    }

    return bsd_hostent_fill(SocketBase, (const char *)name, addr);
}

struct hostent *bsd_gethostbyname_r(register STRPTR name           __asm("a0"),
                                    register struct hostent *hp    __asm("a1"),
                                    register APTR buf              __asm("a2"),
                                    register ULONG buflen          __asm("d0"),
                                    register LONG *he              __asm("a3"),
                                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr = 0;
    LONG  code = NETDB_SUCCESS;

    if (bsd_resolve_name(SocketBase, (const char *)name, &addr, &code) != 0)
    {
        if (he != NULL)
            *he = code;
        return NULL;
    }

    return bsd_hostent_pack(SocketBase, hp, buf, buflen, he,
                            (const char *)name, addr);
}

static LONG bsd_resolve_addr(struct AmiSocketBase *base, STRPTR addr_bytes,
                             LONG len, LONG type, ULONG *addr, char *name,
                             ULONG name_len, LONG *he_out)
{
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

    if (netstack_resolve_reverse(*addr, name, name_len,
                                 BSD_RESOLVE_TIMEOUT) != AMI_NET_OK)
    {
        *he_out = HOST_NOT_FOUND;
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
    ULONG value = 0;
    LONG  he    = NETDB_SUCCESS;

    if (bsd_resolve_addr(SocketBase, addr, len, type, &value,
                         SocketBase->sb_HostName,
                         sizeof(SocketBase->sb_HostName), &he) != 0)
    {
        bsd_set_herrno(SocketBase, he);
        return NULL;
    }

    /* sb_HostName already holds the answer; fill in around it. */
    return bsd_hostent_fill(SocketBase, SocketBase->sb_HostName, value);
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

int bsd_gethostname(register char *name     __asm("a0"),
                    register size_t namelen __asm("d0"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiConfig *cfg = netstack_config();

    if (name == NULL || namelen == 0)
        return (int)bsd_fail(SocketBase, AMI_EFAULT);

    if (cfg == NULL || cfg->hostname[0] == '\0')
    {
        bsd_strncpy(name, "amiga", (ULONG)namelen);
        return 0;
    }

    if (bsd_strlen(cfg->hostname) >= (ULONG)namelen)
        return (int)bsd_fail(SocketBase, AMI_ENAMETOOLONG);

    bsd_strncpy(name, cfg->hostname, (ULONG)namelen);

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
