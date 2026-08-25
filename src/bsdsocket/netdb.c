/*
 * bsdsocket.library, the local network database.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

/*
 * An entry with no aliases still needs a valid, NULL-terminated alias vector:
 * callers walk h_aliases/s_aliases without checking for NULL first. Never
 * written, so a shared static is safe here.
 */
static char *bsd_no_aliases[1] = { NULL };

static __STRPTR *bsd_aliases_of(const AmiNetdbEntry *entry)
{
    if (entry->aliases == NULL)
        return (__STRPTR *)bsd_no_aliases;

    /* Const is dropped because the ABI says char **. The store is read-only
       and nothing in the library writes through it. */
    return (__STRPTR *)(APTR)entry->aliases;
}

static struct servent *bsd_servent_fill(struct AmiSocketBase *base,
                                        const AmiNetdbEntry *entry)
{
    if (entry == NULL)
        return NULL;

    base->sb_ServEnt.s_name    = (__STRPTR)(APTR)entry->name;
    base->sb_ServEnt.s_aliases = bsd_aliases_of(entry);
    base->sb_ServEnt.s_port    = (__LONG)BSD_HTONS((UWORD)entry->value);
    base->sb_ServEnt.s_proto   = (__STRPTR)(APTR)entry->proto;

    return &base->sb_ServEnt;
}

struct servent *bsd_getservbyname(register STRPTR name  __asm("a0"),
                                  register STRPTR proto __asm("a1"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_servent_fill(SocketBase,
                            ami_netdb_serv_by_name((const char *)name,
                                                   (const char *)proto));
}

struct servent *bsd_getservbyport(register LONG port    __asm("d0"),
                                  register STRPTR proto __asm("a0"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /* The caller passes the port in network order (htons(21)). The store
       holds it in host order. Identity on m68k, spelled out anyway. */
    LONG host_port = (LONG)BSD_NTOHS((UWORD)port);

    return bsd_servent_fill(SocketBase,
                            ami_netdb_serv_by_port(host_port,
                                                   (const char *)proto));
}

VOID bsd_setservent(register LONG stay_open __asm("d0"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)stay_open;

    SocketBase->sb_ServCursor = 0;
}

VOID bsd_endservent(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    SocketBase->sb_ServCursor = 0;
}

struct servent *bsd_getservent(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiNetdbEntry *entry =
        ami_netdb_serv_entry(SocketBase->sb_ServCursor);

    if (entry == NULL)
        return NULL;

    SocketBase->sb_ServCursor++;

    return bsd_servent_fill(SocketBase, entry);
}

static struct protoent *bsd_protoent_fill(struct AmiSocketBase *base,
                                          const AmiNetdbEntry *entry)
{
    if (entry == NULL)
        return NULL;

    base->sb_ProtoEnt.p_name    = (__STRPTR)(APTR)entry->name;
    base->sb_ProtoEnt.p_aliases = bsd_aliases_of(entry);
    base->sb_ProtoEnt.p_proto   = (__LONG)entry->value;

    return &base->sb_ProtoEnt;
}

struct protoent *bsd_getprotobyname(register STRPTR name __asm("a0"),
                                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_protoent_fill(SocketBase,
                             ami_netdb_proto_by_name((const char *)name));
}

struct protoent *bsd_getprotobynumber(register LONG proto __asm("d0"),
                                      register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_protoent_fill(SocketBase, ami_netdb_proto_by_number(proto));
}

VOID bsd_setprotoent(register LONG stay_open __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)stay_open;

    SocketBase->sb_ProtoCursor = 0;
}

VOID bsd_endprotoent(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    SocketBase->sb_ProtoCursor = 0;
}

struct protoent *bsd_getprotoent(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiNetdbEntry *entry =
        ami_netdb_proto_entry(SocketBase->sb_ProtoCursor);

    if (entry == NULL)
        return NULL;

    SocketBase->sb_ProtoCursor++;

    return bsd_protoent_fill(SocketBase, entry);
}

static struct netent *bsd_netent_fill(struct AmiSocketBase *base,
                                      const AmiNetdbEntry *entry)
{
    if (entry == NULL)
        return NULL;

    base->sb_NetEnt.n_name     = (__STRPTR)(APTR)entry->name;
    base->sb_NetEnt.n_aliases  = bsd_aliases_of(entry);
    base->sb_NetEnt.n_addrtype = AF_INET;

    /*
     * n_net is a network number, not an address: 127, not 127.0.0.0. BSD
     * returns it in host order (getnetbyaddr() takes it the same way), which
     * is what src/config/netdb.c parses out of DEVS:Internet/networks.
     */
    base->sb_NetEnt.n_net = (in_addr_t)entry->value;

    return &base->sb_NetEnt;
}

struct netent *bsd_getnetbyname(register STRPTR name __asm("a0"),
                                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return bsd_netent_fill(SocketBase,
                           ami_netdb_net_by_name((const char *)name));
}

struct netent *bsd_getnetbyaddr(register in_addr_t net __asm("d0"),
                                register LONG type     __asm("d1"),
                                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    if (type != AF_INET && type != AF_UNSPEC)
        return NULL;

    return bsd_netent_fill(SocketBase, ami_netdb_net_by_addr((ULONG)net));
}

VOID bsd_setnetent(register LONG stay_open __asm("d0"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)stay_open;

    SocketBase->sb_NetCursor = 0;
}

VOID bsd_endnetent(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    SocketBase->sb_NetCursor = 0;
}

struct netent *bsd_getnetent(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    const AmiNetdbEntry *entry = ami_netdb_net_entry(SocketBase->sb_NetCursor);

    if (entry == NULL)
        return NULL;

    SocketBase->sb_NetCursor++;

    return bsd_netent_fill(SocketBase, entry);
}
