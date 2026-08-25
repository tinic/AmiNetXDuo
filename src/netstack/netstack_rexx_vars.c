/*
 * AmiNetXDuo, QUERY and SET for the AMITCP ARexx host.
 *
 * The name space, the order and the shape of every answer come from AmiTCP/IP
 * 3.0b2.  Every field is fixed-width and every list carries all its entries
 * because netstat and rx.fingerd parse the answers positionally.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "netstack_rexx.h"

#include "aminetxduo/nx_queue.h"

#include <dos/dos.h>

#include <proto/dos.h>
#include <proto/exec.h>

const char ami_rx_err_unknown[]     = "Unknown command\n";
const char ami_rx_err_syntax[]      = "Syntax error\n";
static const char ami_rx_err_illegal_var[] = "%s: unknown variable %s\n";
static const char ami_rx_err_illegal_ind[] = "%s: unknown index %s\n";
static const char ami_rx_err_too_long[]    = "Result too long\n";
static const char ami_rx_err_memory[]      = "Memory exhausted\n";
static const char ami_rx_err_nowrite[]     = "%s: Variable %s is not writeable\n";
const char ami_rx_err_unimpl[]      = "Not implemented\n";
const char ami_rx_err_state[]       = "No active stack\n";

static const char ami_rx_vars[] =
    "WITH,IC=ICMP,ICH=ICMPHIST,IP,T=TCP,U=UDP,CONNECTIONS,HOSTNAME,ROUTES,"
    "MBS=MBUF_STAT,MBTS=MBUF_TYPE_STATS,MBC=MBUF_CONF,LOG,TASKNAME,"
    "NTH=NTHBASE,DBSANA=DEBUGSANA,DBICMP=DEBUGICMP,DBIP=DEBUGIP,"
    "GTW=GATEWAY,REDIR=IPSENDREDIRECTS,USENS=USENAMESERVER,"
    "ULO=USELOOPBACK,TCPSND=TCP_SENDSPACE,TCPRCV=TCP_RECVSPACE,"
    "CON=CONSOLENAME,LOGF=LOGFILENAME,"
    "SVC=SERVICES";

enum
{
    RXV_WITH = 0,
    RXV_ICMP,
    RXV_ICMPHIST,
    RXV_IP,
    RXV_TCP,
    RXV_UDP,
    RXV_CONNECTIONS,
    RXV_HOSTNAME,
    RXV_ROUTES,
    RXV_MBUF_STAT,
    RXV_MBUF_TYPE_STATS,
    RXV_MBUF_CONF,
    RXV_LOG,
    RXV_TASKNAME,
    RXV_NTHBASE,
    RXV_DEBUGSANA,
    RXV_DEBUGICMP,
    RXV_DEBUGIP,
    RXV_GATEWAY,
    RXV_IPSENDREDIRECTS,
    RXV_USENAMESERVER,
    RXV_USELOOPBACK,
    RXV_TCP_SENDSPACE,
    RXV_TCP_RECVSPACE,
    RXV_CONSOLENAME,
    RXV_LOGFILENAME,
    RXV_SERVICES,
    RXV_COUNT
};

static const char ami_rx_kw_icmp[] =
    "E=ERROR,S=SHORTOLD,I=ICMPOLD,CO=CODE,T=TOOSHORT,CH=CHKSUM,L=LENGTH,"
    "R=RESPONSES";

static const char ami_rx_kw_ip[] =
    "T=TOTAL,CH=CHKSUM,TOOSH=TOOSHORT,TOOSM=TOOSMALL,H=HEADER,"
    "LE=LENGTH,FS=FRAGMENTS,FD=FDROP,FT=FTIMEOUT,FO=FORWARD,FW=FWDCANT,"
    "RED=REDIRECTSENT,N=NOPROTO,D=DELIVER,LO=LOCALOUT,OD=ODROPPED,"
    "REA=REASSEMBLED,FE=FED,OF=OFRAGMENTS,FC=FCANT";

static const char ami_rx_kw_tcp[] =
    "CA=CATTEM,A=ACCEPTS,CO=CONNECT,DR=DROPS,CD=CDROPS,CL=CLOSED,"
    "SE=SEGSTIMED,RTT=RTTUPDATE,DE=DELACK,T=TIMEODROP,RE=REXMTT,"
    "P=PERSIST,KAT=KATIMEO,KAP=KAPROBE,KAD=KADROPS,ST=STOTAL,SP=SPACK,"
    "SB=SBYTE,SREP=SREPACK,SREB=SREBYTE,SA=SACKS,SWP=SWPROBE,"
    "SU=SURGENT,SWU=SWUPDATE,SC=SCTRL,RTO=RTOTAL,RPA=RPACK,RB=RBYTE,"
    "RC=RCHKSUM,ROF=ROFFSET,RPS=RPSHORT,RDUPP=RDUPPACK,RDUPB=RDUPBYTE,"
    "RPDUPD=RPDUPDATA,RPDUPB=RPDUPBYTE,ROOP=ROOPACK,ROOB=ROOBYTE,"
    "RPL=RPLATE,RBL=RBLATE,RAF=RAFTER,RWP=RWPROBE,RDUPA=RDUPACK,"
    "RACKT=RACKTOOM,RACKP=RACKPACK,RACKB=RACKBYTE,RWU=RWUPDATE";

static const char ami_rx_kw_udp[] =
    "I=ITOTAL,H=HEADSHORT,C=CHKSUM,L=LENGTH,N=NOPORT,B=BCNOPORT,"
    "F=FULLSOC,M=MISPCB,O=OTOTAL";

static const char ami_rx_kw_mbuf_stat[] =
    "M=MBUFS,CL=CLUSTERS,CLF=CLFREE,MD=MDROPS,NW=NWAITED,ND=NDRAINED,"
    "TMU=TOTALMEMORYUSED";

static const char ami_rx_kw_mbuf_conf[] =
    "I=INITIAL,CH=CHUNK,CL=CLCHUNK,MM=MAXMEM,CS=CLUSTERSIZE";

static const char ami_rx_kw_log[] = "COUNT,LEN";

/* Numeric order as in <sys/socket.h>; do not reorder.  The ROUTES argument is
   an address family number rather than a name, and netstat prints
   afamily.<number> from it. */
static const char ami_rx_kw_routes[] =
    "ALL,UNIX,INET,IMPLINK,PUP,CHAOS,NS,ISO,ECMA,DATAKIT,CCITT,SNA,DECnet,DLI,"
    "LAT,HYLINK,APPLETALK,ROUTE,LINK,XTP";

#define RXV_AF_ALL      0
#define RXV_AF_INET     2

#define RX_ICMP_COUNT       8
#define RX_IP_COUNT         20
#define RX_TCP_COUNT        46
#define RX_UDP_COUNT        9

#define RX_ICMP_MAXTYPE     18
#define RX_ICMP_HIST        (RX_ICMP_MAXTYPE + 1)

#define RX_ICMP_ECHO        8
#define RX_ICMP_ECHOREPLY   0

/* rvd_Index is the level-2 template, NULL both for a variable that takes no
   index and for one whose answer is a formatted list that parses its own
   argument.  The writeable set is empty; see ami_rx_setvalue(). */
typedef struct AmiRxVarDef
{
    const char *rvd_Index;
    UWORD       rvd_Count;
    BOOL        rvd_Read;
} AmiRxVarDef;

static const AmiRxVarDef ami_rx_vardefs[RXV_COUNT] =
{
    /* WITH                */ { NULL,                  0,              FALSE },
    /* ICMP                */ { ami_rx_kw_icmp,        RX_ICMP_COUNT,  TRUE  },
    /* ICMPHIST            */ { NULL,                  0,              TRUE  },
    /* IP                  */ { ami_rx_kw_ip,          RX_IP_COUNT,    TRUE  },
    /* TCP                 */ { ami_rx_kw_tcp,         RX_TCP_COUNT,   TRUE  },
    /* UDP                 */ { ami_rx_kw_udp,         RX_UDP_COUNT,   TRUE  },
    /* CONNECTIONS         */ { NULL,                  0,              TRUE  },
    /* HOSTNAME            */ { NULL,                  0,              TRUE  },
    /* ROUTES              */ { NULL,                  0,              TRUE  },
    /* MBUF_STAT           */ { ami_rx_kw_mbuf_stat,   7,              FALSE },
    /* MBUF_TYPE_STATS     */ { NULL,                  0,              FALSE },
    /* MBUF_CONF           */ { ami_rx_kw_mbuf_conf,   5,              FALSE },
    /* LOG                 */ { ami_rx_kw_log,         2,              FALSE },
    /* TASKNAME            */ { NULL,                  0,              TRUE  },
    /* NTHBASE             */ { NULL,                  0,              TRUE  },
    /* DEBUGSANA           */ { NULL,                  0,              TRUE  },
    /* DEBUGICMP           */ { NULL,                  0,              TRUE  },
    /* DEBUGIP             */ { NULL,                  0,              TRUE  },
    /* GATEWAY             */ { NULL,                  0,              TRUE  },
    /* IPSENDREDIRECTS     */ { NULL,                  0,              TRUE  },
    /* USENAMESERVER       */ { NULL,                  0,              TRUE  },
    /* USELOOPBACK         */ { NULL,                  0,              TRUE  },
    /* TCP_SENDSPACE       */ { NULL,                  0,              FALSE },
    /* TCP_RECVSPACE       */ { NULL,                  0,              FALSE },
    /* CONSOLENAME         */ { NULL,                  0,              FALSE },
    /* LOGFILENAME         */ { NULL,                  0,              FALSE },
#ifdef AMINETXDUO_MDNS
    /* SERVICES            */ { NULL,                  0,              TRUE  }
#else
    /* SERVICES            */ { NULL,                  0,              FALSE }
#endif
};

VOID ami_rx_reply_init(AmiRxReply *r, STRPTR buffer, LONG length)
{
    r->rr_Buffer = buffer;
    r->rr_Length = length;
    r->rr_Used   = 0;
    r->rr_Alloc  = NULL;
    buffer[0]    = '\0';
}

VOID ami_rx_reply_done(AmiRxReply *r)
{
    if (r->rr_Alloc != NULL)
    {
        FreeVec(r->rr_Alloc);
        r->rr_Alloc = NULL;
    }
}

static BOOL ami_rx_reply_room(AmiRxReply *r, LONG want)
{
    STRPTR next;
    LONG   i;

    if (want <= r->rr_Length)
        return TRUE;

    next = AllocVec((ULONG)want + 1, MEMF_ANY);
    if (next == NULL)
        return FALSE;

    for (i = 0; i < r->rr_Used; i++)
        next[i] = r->rr_Buffer[i];
    next[r->rr_Used] = '\0';

    if (r->rr_Alloc != NULL)
        FreeVec(r->rr_Alloc);

    r->rr_Alloc  = next;
    r->rr_Buffer = next;
    r->rr_Length = want;

    return TRUE;
}

static BOOL ami_rx_put(AmiRxReply *r, const char *text, LONG len)
{
    LONG i;

    if (r->rr_Used + len > r->rr_Length)
        return FALSE;

    for (i = 0; i < len; i++)
        r->rr_Buffer[r->rr_Used + i] = text[i];

    r->rr_Used += len;
    r->rr_Buffer[r->rr_Used] = '\0';

    return TRUE;
}

static BOOL ami_rx_put_str(AmiRxReply *r, const char *text)
{
    LONG len = 0;

    while (text[len] != '\0')
        len++;

    return ami_rx_put(r, text, len);
}

static BOOL ami_rx_put_dec(AmiRxReply *r, ULONG value)
{
    char digits[11];
    LONG n = 0;

    do
    {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    if (r->rr_Used + n > r->rr_Length)
        return FALSE;

    while (n-- > 0)
        r->rr_Buffer[r->rr_Used++] = digits[n];

    r->rr_Buffer[r->rr_Used] = '\0';

    return TRUE;
}

/* Fixed-width lower-case hex, which is what the netstat x2d() and substr()
   read the addresses, ports and queue lengths out of. */
static BOOL ami_rx_put_hex(AmiRxReply *r, ULONG value, UWORD width)
{
    static const char hex[] = "0123456789abcdef";
    UWORD             i;

    if (r->rr_Used + (LONG)width > r->rr_Length)
        return FALSE;

    for (i = 0; i < width; i++)
    {
        UWORD shift = (UWORD)((width - 1 - i) * 4);

        r->rr_Buffer[r->rr_Used + i] = hex[(value >> shift) & 0xF];
    }

    r->rr_Used += (LONG)width;
    r->rr_Buffer[r->rr_Used] = '\0';

    return TRUE;
}

static VOID ami_rx_error_named(AmiRxReply *r, const char **errstr,
                               const char *format, const char *func,
                               const char *name)
{
    ULONG i;

    r->rr_Used   = 0;
    r->rr_Buffer[0] = '\0';

    for (i = 0; format[i] != '\0'; i++)
    {
        if (format[i] == '%' && format[i + 1] == 's')
        {
            (VOID)ami_rx_put_str(r, func);
            func = name;
            i++;
            continue;
        }

        (VOID)ami_rx_put(r, &format[i], 1);
    }

    *errstr = (const char *)r->rr_Buffer;
}

/* The ARexx host is a plain Exec Process: every live NetX snapshot below must
   be bracketed by ami_netstack_enter_alloc()/leave_free(), and reply growth
   and Delay() must stay outside that bracket. */

static VOID ami_rx_zero(ULONG *out, UWORD count)
{
    while (count-- > 0)
        out[count] = 0;
}

static VOID ami_rx_icmp(NX_IP *ip, ULONG *out)
{
    ULONG sent = 0, timeouts = 0, suspended = 0, responses = 0;
    ULONG checksum = 0, unhandled = 0;

    ami_rx_zero(out, RX_ICMP_COUNT);

    if (nx_icmp_info_get(ip, &sent, &timeouts, &suspended, &responses,
                         &checksum, &unhandled) != NX_SUCCESS)
        return;

    out[5] = checksum;          /* CH=CHKSUM */
}

static VOID ami_rx_ip(NX_IP *ip, ULONG *out)
{
    ULONG sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
    ULONG invalid = 0, dropped = 0, checksum = 0, send_dropped = 0;
    ULONG frags_sent = 0, frags_received = 0;

    ami_rx_zero(out, RX_IP_COUNT);

    if (nx_ip_info_get(ip, &sent, &sent_bytes, &received, &received_bytes,
                       &invalid, &dropped, &checksum, &send_dropped,
                       &frags_sent, &frags_received) != NX_SUCCESS)
        return;

    out[0]  = received;
    out[1]  = checksum;
    out[6]  = frags_received;
    out[14] = sent;
    out[15] = send_dropped;
    out[18] = frags_sent;
}

static VOID ami_rx_tcp(NX_IP *ip, ULONG *out)
{
    ULONG sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
    ULONG invalid = 0, dropped = 0, checksum = 0;
    ULONG connections = 0, disconnections = 0, connections_dropped = 0;
    ULONG retransmits = 0;

    ami_rx_zero(out, RX_TCP_COUNT);

    if (nx_tcp_info_get(ip, &sent, &sent_bytes, &received, &received_bytes,
                        &invalid, &dropped, &checksum, &connections,
                        &disconnections, &connections_dropped,
                        &retransmits) != NX_SUCCESS)
        return;

    out[2]  = connections;
    out[3]  = connections_dropped;
    out[5]  = disconnections;
    out[15] = sent;
    out[17] = sent_bytes;
    out[18] = retransmits;
    out[25] = received;
    out[27] = received_bytes;
    out[28] = checksum;
}

static VOID ami_rx_udp(NX_IP *ip, ULONG *out)
{
    ULONG sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
    ULONG invalid = 0, dropped = 0, checksum = 0;

    ami_rx_zero(out, RX_UDP_COUNT);

    if (nx_udp_info_get(ip, &sent, &sent_bytes, &received, &received_bytes,
                        &invalid, &dropped, &checksum) != NX_SUCCESS)
        return;

    out[0] = received;
    out[1] = invalid;
    out[2] = checksum;
    out[6] = dropped;
    out[8] = sent;

#ifndef NX_DISABLE_UDP_INFO
    out[4] = ip->nx_ip_udp_no_port_for_delivery;
#endif
}

/* ICMPHIST: outhist[0..18] then inhist[0..18], decimal, space separated, no
   trailing space.  netstat indexes it positionally, so all 38 must be there. */
static LONG ami_rx_icmphist(NX_IP *ip, const char **errstr, AmiRxReply *r)
{
    AmiNetCaller *caller;
    ULONG hist[2 * RX_ICMP_HIST];
    ULONG sent = 0, timeouts = 0, suspended = 0, responses = 0;
    ULONG checksum = 0, unhandled = 0;
    UWORD i;

    for (i = 0; i < 2 * RX_ICMP_HIST; i++)
        hist[i] = 0;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        *errstr = ami_rx_err_state;
        return RETURN_ERROR;
    }

    if (nx_icmp_info_get(ip, &sent, &timeouts, &suspended, &responses,
                         &checksum, &unhandled) == NX_SUCCESS)
    {
        hist[RX_ICMP_ECHO]                    = sent;
        hist[RX_ICMP_HIST + RX_ICMP_ECHOREPLY] = responses;
    }

    ami_netstack_leave_free(caller);

    for (i = 0; i < 2 * RX_ICMP_HIST; i++)
    {
        if (i != 0 && !ami_rx_put(r, " ", 1))
            goto full;
        if (!ami_rx_put_dec(r, hist[i]))
            goto full;
    }

    return RETURN_OK;

full:
    *errstr = ami_rx_err_too_long;
    return RETURN_ERROR;
}

/* CONNECTIONS, the AmiTCP getsockets() format, space separated, addresses in
   host order because netstat parses them with substr() and x2d():
   "%lc %04lx %04lx %08lx %04lx %08lx %04lx %1lx" -- the state is t_state. */
#define RX_STATLEN  42

static LONG ami_rx_tcp_state(ULONG nx_state)
{
    static const UBYTE map[][2] =
    {
        { NX_TCP_CLOSED,       0 },  /* TCPS_CLOSED       */
        { NX_TCP_LISTEN_STATE, 1 },  /* TCPS_LISTEN       */
        { NX_TCP_SYN_SENT,     2 },  /* TCPS_SYN_SENT     */
        { NX_TCP_SYN_RECEIVED, 3 },  /* TCPS_SYN_RECEIVED */
        { NX_TCP_ESTABLISHED,  4 },  /* TCPS_ESTABLISHED  */
        { NX_TCP_CLOSE_WAIT,   5 },  /* TCPS_CLOSE_WAIT   */
        { NX_TCP_FIN_WAIT_1,   6 },  /* TCPS_FIN_WAIT_1   */
        { NX_TCP_CLOSING,      7 },  /* TCPS_CLOSING      */
        { NX_TCP_LAST_ACK,     8 },  /* TCPS_LAST_ACK     */
        { NX_TCP_FIN_WAIT_2,   9 },  /* TCPS_FIN_WAIT_2   */
        { NX_TCP_TIMED_WAIT,  10 }   /* TCPS_TIME_WAIT    */
    };
    UWORD i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (map[i][0] == (UBYTE)nx_state)
            return (LONG)map[i][1];
    }

    return 0;
}

static NX_TCP_SOCKET *ami_rx_listen_spare(NX_IP *ip, UINT port)
{
    NX_TCP_LISTEN *listen_ptr = ip->nx_ip_tcp_active_listen_requests;
    ULONG          n;

    for (n = 0; n < (ULONG)NX_MAX_LISTEN_REQUESTS && listen_ptr != NX_NULL; n++)
    {
        if (listen_ptr->nx_tcp_listen_port == port)
            return listen_ptr->nx_tcp_listen_socket_ptr;

        listen_ptr = listen_ptr->nx_tcp_listen_next;

        if (listen_ptr == ip->nx_ip_tcp_active_listen_requests)
            break;
    }

    return NX_NULL;
}

typedef struct AmiRxSocket
{
    char    rs_Proto;
    ULONG   rs_RecvQ;
    ULONG   rs_SendQ;
    ULONG   rs_Local;
    ULONG   rs_LocalPort;
    ULONG   rs_Foreign;
    ULONG   rs_ForeignPort;
    LONG    rs_State;
} AmiRxSocket;

/* Copy under nx_ip_protection and format outside it: the formatting can grow
   the reply buffer, and allocating inside the lock risks both an unbounded
   hold and a deadlock against the IP thread. */
static UWORD ami_rx_collect_sockets(NX_IP *ip, AmiRxSocket *out, UWORD room)
{
    NX_TCP_SOCKET *tcp;
    NX_UDP_SOCKET *udp;
    ULONG          n;
    UWORD          used = 0;

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

    tcp = ip->nx_ip_tcp_created_sockets_ptr;
    for (n = 0; n < ip->nx_ip_tcp_created_sockets_count && tcp != NX_NULL; n++)
    {
        NX_TCP_SOCKET *spare = ami_rx_listen_spare(ip, tcp->nx_tcp_socket_port);
        AmiRxSocket   *e;

        if (spare == tcp || used >= room)
        {
            tcp = tcp->nx_tcp_socket_created_next;
            continue;
        }

        e = &out[used++];
        e->rs_Proto       = 't';
        e->rs_LocalPort   = (ULONG)tcp->nx_tcp_socket_port;
        e->rs_ForeignPort = (ULONG)tcp->nx_tcp_socket_connect_port;
        e->rs_Foreign     = tcp->nx_tcp_socket_connect_ip.nxd_ip_address.v4;
        e->rs_Local       = (tcp->nx_tcp_socket_connect_interface != NX_NULL)
                                ? tcp->nx_tcp_socket_connect_interface
                                      ->nx_interface_ip_address
                                : 0;
        e->rs_State       = (spare != NX_NULL &&
                             tcp->nx_tcp_socket_state == NX_TCP_CLOSED)
                                ? 1     /* TCPS_LISTEN */
                                : ami_rx_tcp_state(tcp->nx_tcp_socket_state);
        e->rs_RecvQ = ami_nx_tcp_receive_bytes(tcp);
        e->rs_SendQ = ami_nx_tcp_send_bytes(tcp);

        tcp = tcp->nx_tcp_socket_created_next;
    }

    udp = ip->nx_ip_udp_created_sockets_ptr;
    for (n = 0; n < ip->nx_ip_udp_created_sockets_count && udp != NX_NULL; n++)
    {
        AmiRxSocket *e;

        if (used >= room)
            break;

        e = &out[used++];
        e->rs_Proto       = 'u';
        e->rs_LocalPort   = (ULONG)udp->nx_udp_socket_port;
        e->rs_ForeignPort = 0;
        e->rs_Foreign     = 0;
        e->rs_Local       = 0;
        e->rs_SendQ       = 0;
        e->rs_State       = 0;      /* AmiTCP: "NO state for UDP" */
        e->rs_RecvQ = ami_nx_udp_receive_bytes(udp);

        udp = udp->nx_udp_socket_created_next;
    }

    tx_mutex_put(&ip->nx_ip_protection);

    return used;
}

/* No ceiling on created sockets in NetX Duo; past this the list truncates. */
#define RX_MAX_SOCKETS  64

static LONG ami_rx_connections(NX_IP *ip, const char **errstr, AmiRxReply *r)
{
    AmiNetCaller *caller;
    AmiRxSocket *mem;
    UWORD        count;
    UWORD        i;
    LONG         rc = RETURN_OK;

    mem = AllocVec((ULONG)sizeof(AmiRxSocket) * RX_MAX_SOCKETS, MEMF_ANY);
    if (mem == NULL)
    {
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        FreeVec(mem);
        *errstr = ami_rx_err_state;
        return RETURN_ERROR;
    }

    count = ami_rx_collect_sockets(ip, mem, RX_MAX_SOCKETS);

    ami_netstack_leave_free(caller);

    if (count != 0 &&
        !ami_rx_reply_room(r, r->rr_Used + (LONG)count * RX_STATLEN + 1))
    {
        FreeVec(mem);
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    for (i = 0; i < count; i++)
    {
        const AmiRxSocket *e = &mem[i];
        BOOL               ok;

        ok = (i == 0 || ami_rx_put(r, " ", 1))
             && ami_rx_put(r, &e->rs_Proto, 1)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rs_RecvQ, 4)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rs_SendQ, 4)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rs_Local, 8)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rs_LocalPort, 4)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rs_Foreign, 8)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rs_ForeignPort, 4)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, (ULONG)e->rs_State, 1);

        if (!ok)
        {
            *errstr = ami_rx_err_too_long;
            rc      = RETURN_ERROR;
            break;
        }
    }

    FreeVec(mem);

    return rc;
}

/* ROUTES <family>, the AmiTCP getroutes() format: "%02lx %08lx %08lx %-8.8s
   %04lx %08lx %s%ld ", with a trailing space after every entry including the
   last, because netstat parses positionally.  No flag letters prints `""`. */
typedef struct AmiRxRoute
{
    ULONG   rr_Dest;
    ULONG   rr_Gateway;
    UWORD   rr_Flags;
    char    rr_Name[AMI_CFG_NAME_LEN];
} AmiRxRoute;

#define RX_RT_UP        0x0001
#define RX_RT_GATEWAY   0x0002
#define RX_RT_HOST      0x0004

#ifdef NX_ENABLE_IP_STATIC_ROUTING
#define RX_ROUTE_STATIC_MAX     NX_IP_ROUTING_TABLE_SIZE
#else
#define RX_ROUTE_STATIC_MAX     0
#endif

#define RX_MAX_ROUTES   (NX_MAX_PHYSICAL_INTERFACES + RX_ROUTE_STATIC_MAX + 1)

/* 45 is the fixed part: six hex fields, the flag field and their separators. */
#define RX_ROUTELEN     (45 + AMI_CFG_NAME_LEN + 1)

static VOID ami_rx_route_name(AmiRxRoute *route, const char *name)
{
    UWORD i = 0;

    if (name == NULL || name[0] == '\0')
        name = "none";

    while (name[i] != '\0' && i + 1 < (UWORD)sizeof(route->rr_Name))
    {
        route->rr_Name[i] = name[i];
        i++;
    }

    route->rr_Name[i] = '\0';
}

static UWORD ami_rx_collect_routes(NX_IP *ip, AmiRxRoute *out, UWORD room)
{
    UWORD used = 0;
    UINT  i;
    ULONG gateway = 0;

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES && used < room; i++)
    {
        NX_INTERFACE *nxif = &ip->nx_ip_interface[i];
        AmiRxRoute   *e;

        if (nxif->nx_interface_valid == 0 || nxif->nx_interface_ip_address == 0)
            continue;

        e = &out[used++];
        e->rr_Dest    = nxif->nx_interface_ip_address &
                        nxif->nx_interface_ip_network_mask;
        e->rr_Gateway = 0;
        e->rr_Flags   = RX_RT_UP;
        /* Carry the name across the mutex boundary rather than dereferencing
           the slot later: it is reusable and may be zeroed or reassigned. */
        ami_rx_route_name(e, (const char *)nxif->nx_interface_name);

        if (nxif->nx_interface_ip_network_mask == 0xFFFFFFFFUL)
            e->rr_Flags |= RX_RT_HOST;
    }

#ifdef NX_ENABLE_IP_STATIC_ROUTING
    for (i = 0; i < ip->nx_ip_routing_table_entry_count && used < room; i++)
    {
        const NX_IP_ROUTING_ENTRY *src = &ip->nx_ip_routing_table[i];
        AmiRxRoute                *e   = &out[used++];

        e->rr_Dest    = src->nx_ip_routing_dest_ip;
        e->rr_Gateway = src->nx_ip_routing_next_hop_address;
        e->rr_Flags   = RX_RT_UP | RX_RT_GATEWAY;
        ami_rx_route_name(e, "none");

        if (src->nx_ip_routing_net_mask == 0xFFFFFFFFUL)
            e->rr_Flags |= RX_RT_HOST;
    }
#endif

    tx_mutex_put(&ip->nx_ip_protection);

    if (used < room && nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS
        && gateway != 0)
    {
        AmiRxRoute *e = &out[used++];

        e->rr_Dest    = 0;
        e->rr_Gateway = gateway;
        e->rr_Flags   = RX_RT_UP | RX_RT_GATEWAY;
        ami_rx_route_name(e, "none");
    }

    return used;
}

static BOOL ami_rx_put_rtflags(AmiRxReply *r, UWORD flags)
{
    char letters[9];
    UWORD n = 0;

    /* The AmiTCP bits[] order: U G H D M C X L R. Only the first three can be
       set here.  The other six describe entries this stack never makes. */
    if ((flags & RX_RT_UP) != 0)
        letters[n++] = 'U';
    if ((flags & RX_RT_GATEWAY) != 0)
        letters[n++] = 'G';
    if ((flags & RX_RT_HOST) != 0)
        letters[n++] = 'H';

    if (n == 0)
    {
        letters[n++] = '"';
        letters[n++] = '"';
    }

    /* "%-8.8s ": left-justified in eight columns, then one space. */
    while (n < 8)
        letters[n++] = ' ';

    letters[8] = '\0';

    return ami_rx_put(r, letters, 8) && ami_rx_put(r, " ", 1);
}

static LONG ami_rx_routes(NX_IP *ip, struct CSource *args, const char **errstr,
                          AmiRxReply *r)
{
    AmiNetCaller *caller;
    char        buf[RX_KEYWORDLEN];
    AmiRxRoute *mem;
    LONG        af;
    UWORD       count;
    UWORD       i;
    LONG        rc = RETURN_OK;

    /* Required: ReadItem() leaves the buffer alone when there is nothing to
       read, and the error paths below name what they read. */
    buf[0] = '\0';

    if (ReadItem((STRPTR)buf, (LONG)sizeof(buf), args) <= 0)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_ERROR;
    }

    af = FindArg((CONST_STRPTR)ami_rx_kw_routes, (CONST_STRPTR)buf);
    if (af < 0)
    {
        ami_rx_error_named(r, errstr, ami_rx_err_illegal_var, "getroutes", buf);
        return RETURN_WARN;
    }

    /* Every route here is AF_INET: another family is empty, not an error. */
    if (af != RXV_AF_ALL && af != RXV_AF_INET)
        return RETURN_OK;

    mem = AllocVec((ULONG)sizeof(AmiRxRoute) * RX_MAX_ROUTES, MEMF_ANY);
    if (mem == NULL)
    {
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        FreeVec(mem);
        *errstr = ami_rx_err_state;
        return RETURN_ERROR;
    }

    count = ami_rx_collect_routes(ip, mem, RX_MAX_ROUTES);

    ami_netstack_leave_free(caller);

    if (count != 0
        && !ami_rx_reply_room(r, r->rr_Used + (LONG)count * RX_ROUTELEN + 1))
    {
        FreeVec(mem);
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    for (i = 0; i < count; i++)
    {
        const AmiRxRoute *e = &mem[i];
        BOOL              ok;

        ok = ami_rx_put_hex(r, RXV_AF_INET, 2)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rr_Dest, 8)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, e->rr_Gateway, 8)
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_rtflags(r, e->rr_Flags)
             && ami_rx_put_hex(r, 0, 4)          /* refs: not counted  */
             && ami_rx_put(r, " ", 1)
             && ami_rx_put_hex(r, 0, 8)          /* use: not counted   */
             && ami_rx_put(r, " ", 1);

        if (ok)
            ok = ami_rx_put_str(r, e->rr_Name) && ami_rx_put(r, " ", 1);

        if (!ok)
        {
            *errstr = ami_rx_err_too_long;
            rc      = RETURN_ERROR;
            break;
        }
    }

    FreeVec(mem);

    return rc;
}

#ifdef AMINETXDUO_MDNS

/* QUERY SERVICES <type|ALL> [<seconds>].  Blocks for the collection window:
   the wait is Delay() on the host's own task with no ThreadX bracket held,
   because a wait taken while adopted stops the whole stack. */

#define RX_SVC_MAX          48
#define RX_SVC_SECONDS      3
#define RX_SVC_SECONDS_MAX  30

/* Instance TAB type TAB host TAB address TAB port TAB txt, one row per line. */
#define RX_SVCLEN           380

static BOOL ami_rx_put_dotted(AmiRxReply *r, ULONG addr)
{
    return ami_rx_put_dec(r, (addr >> 24) & 0xFFUL)
           && ami_rx_put(r, ".", 1)
           && ami_rx_put_dec(r, (addr >> 16) & 0xFFUL)
           && ami_rx_put(r, ".", 1)
           && ami_rx_put_dec(r, (addr >> 8) & 0xFFUL)
           && ami_rx_put(r, ".", 1)
           && ami_rx_put_dec(r, addr & 0xFFUL);
}

/* A control character breaks the line format this answer is parsed with, and a
   TAB inside a field shifts every field after it, so both become '.'. */
static BOOL ami_rx_put_txt(AmiRxReply *r, const char *text)
{
    LONG i;

    for (i = 0; text[i] != '\0'; i++)
    {
        char c = text[i];

        if (c < ' ' || c == 0x7F)
            c = '.';

        if (!ami_rx_put(r, &c, 1))
            return FALSE;
    }

    return TRUE;
}

/* The optional collection window. Digits only, and the CSource is left where
   it was when the next item is not a number, because it is the next variable
   then. */
static ULONG ami_rx_svc_seconds(struct CSource *args)
{
    char  buf[RX_KEYWORDLEN];
    LONG  mark = args->CS_CurChr;
    ULONG value = 0;
    LONG  i;

    buf[0] = '\0';

    if (ReadItem((STRPTR)buf, (LONG)sizeof(buf), args) <= 0)
    {
        args->CS_CurChr = mark;
        return RX_SVC_SECONDS;
    }

    for (i = 0; buf[i] != '\0'; i++)
    {
        if (buf[i] < '0' || buf[i] > '9')
        {
            args->CS_CurChr = mark;
            return RX_SVC_SECONDS;
        }

        value = (value * 10UL) + (ULONG)(buf[i] - '0');

        if (value > RX_SVC_SECONDS_MAX)
            return RX_SVC_SECONDS_MAX;
    }

    return (value == 0UL) ? RX_SVC_SECONDS : value;
}

static LONG ami_rx_services(struct CSource *args, const char **errstr,
                            AmiRxReply *r)
{
    char            buf[RX_KEYWORDLEN];
    AmiMdnsService *rows;
    const char     *type;
    ULONG           seconds;
    UWORD           count;
    UWORD           i;
    LONG            rc = RETURN_OK;

    buf[0] = '\0';

    if (ReadItem((STRPTR)buf, (LONG)sizeof(buf), args) <= 0)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_ERROR;
    }

    type = (FindArg((CONST_STRPTR)"ALL", (CONST_STRPTR)buf) == 0) ? NULL : buf;

    seconds = ami_rx_svc_seconds(args);

    rows = AllocVec((ULONG)sizeof(AmiMdnsService) * RX_SVC_MAX, MEMF_ANY);
    if (rows == NULL)
    {
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    if (netstack_mdns_browse_start(type) != AMI_NET_OK)
    {
        FreeVec(rows);
        *errstr = ami_rx_err_state;
        return RETURN_ERROR;
    }

    Delay(seconds * 50UL);

    count = netstack_mdns_browse_collect(type, rows, RX_SVC_MAX, NULL);

    (VOID)netstack_mdns_browse_stop(type);

    if (count != 0
        && !ami_rx_reply_room(r, r->rr_Used + (LONG)count * RX_SVCLEN + 1))
    {
        FreeVec(rows);
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    for (i = 0; i < count; i++)
    {
        const AmiMdnsService *e = &rows[i];
        BOOL                  ok;

        ok = ami_rx_put_str(r, e->ams_Name)
             && ami_rx_put(r, "\t", 1)
             && ami_rx_put_str(r, e->ams_Type)
             && ami_rx_put(r, "\t", 1)
             && ami_rx_put_str(r, e->ams_Host)
             && ami_rx_put(r, "\t", 1);

        if (ok && e->ams_Address != 0UL)
            ok = ami_rx_put_dotted(r, e->ams_Address);

        ok = ok
             && ami_rx_put(r, "\t", 1)
             && ami_rx_put_dec(r, (ULONG)e->ams_Port)
             && ami_rx_put(r, "\t", 1)
             && ami_rx_put_txt(r, e->ams_Text)
             && ami_rx_put(r, "\n", 1);

        if (!ok)
        {
            *errstr = ami_rx_err_too_long;
            rc      = RETURN_ERROR;
            break;
        }
    }

    FreeVec(rows);

    return rc;
}
#endif /* AMINETXDUO_MDNS */

static const char *ami_rx_bool(BOOL value)
{
    return value ? "YES" : "NO";
}

static const char *ami_rx_scalar(UWORD var, char *buf, ULONG buflen)
{
    const AmiConfig *cfg = netstack_config();

    switch (var)
    {
        case RXV_HOSTNAME:
        {
            ULONG i;

            if (cfg == NULL || cfg->hostname[0] == '\0')
                return "amiga";

            for (i = 0; cfg->hostname[i] != '\0' && i + 1 < buflen; i++)
                buf[i] = cfg->hostname[i];
            buf[i] = '\0';

            return buf;
        }

        case RXV_TASKNAME:
            return "AmiNetXDuo IP";

        case RXV_NTHBASE:
            return "0";

        case RXV_DEBUGSANA:
        case RXV_DEBUGICMP:
        case RXV_DEBUGIP:
            return ami_rx_bool(FALSE);

        case RXV_GATEWAY:
            return ami_rx_bool(FALSE);

        case RXV_IPSENDREDIRECTS:
            return ami_rx_bool(FALSE);

        case RXV_USENAMESERVER:
            return "SECOND";

        case RXV_USELOOPBACK:
            return ami_rx_bool(TRUE);

        default:
            return NULL;
    }
}

/* An unknown or unreadable name aborts the whole command: the caller splits on
   position and silently misparses a partial answer. */
LONG ami_rx_getvalue(struct CSource *args, const char **errstr, AmiRxReply *r)
{
    NX_IP *ip    = netstack_ip();
    BOOL   any   = FALSE;
    LONG   item;

    if (ip == NULL)
    {
        *errstr = ami_rx_err_state;
        return RETURN_ERROR;
    }

    /* DHCP and router advertisements update the configuration from NetX
       threads that cannot apply it themselves.  Once per GETVALUE. */
    netstack_dns_absorb_pending();

    for (;;)
    {
        char  buf[RX_KEYWORDLEN];
        LONG  var;
        LONG  index = 0;
        ULONG counters[RX_TCP_COUNT];
        const AmiRxVarDef *def;
        const char        *text;
        char               scratch[AMI_CFG_NAME_LEN];

        buf[0] = '\0';

        item = ReadItem((STRPTR)buf, (LONG)sizeof(buf), args);
        if (item <= 0)
            break;

        var = FindArg((CONST_STRPTR)ami_rx_vars, (CONST_STRPTR)buf);
        if (var < 0 || var >= RXV_COUNT || !ami_rx_vardefs[var].rvd_Read)
        {
            ami_rx_error_named(r, errstr, ami_rx_err_illegal_var, "getvalue",
                               buf);
            return RETURN_WARN;
        }

        def = &ami_rx_vardefs[var];

        if (def->rvd_Index != NULL)
        {
            if (ReadItem((STRPTR)buf, (LONG)sizeof(buf), args) <= 0
                || (index = FindArg((CONST_STRPTR)def->rvd_Index,
                                    (CONST_STRPTR)buf)) < 0
                || index >= (LONG)def->rvd_Count)
            {
                ami_rx_error_named(r, errstr, ami_rx_err_illegal_ind,
                                   "getvalue", buf);
                return RETURN_WARN;
            }
        }

        /* The formatted answers write the whole reply themselves and are not
           space-separated from a neighbour. */
        switch (var)
        {
            case RXV_CONNECTIONS:
            {
                LONG rc = ami_rx_connections(ip, errstr, r);

                if (rc != RETURN_OK)
                    return rc;
                any = TRUE;
                continue;
            }

            case RXV_ICMPHIST:
            {
                LONG rc = ami_rx_icmphist(ip, errstr, r);

                if (rc != RETURN_OK)
                    return rc;
                any = TRUE;
                continue;
            }

            case RXV_ROUTES:
            {
                LONG rc = ami_rx_routes(ip, args, errstr, r);

                if (rc != RETURN_OK)
                    return rc;
                any = TRUE;
                continue;
            }

#ifdef AMINETXDUO_MDNS
            case RXV_SERVICES:
            {
                LONG rc = ami_rx_services(args, errstr, r);

                if (rc != RETURN_OK)
                    return rc;
                any = TRUE;
                continue;
            }
#endif

            default:
                break;
        }

        if (any && !ami_rx_put(r, " ", 1))
        {
            *errstr = ami_rx_err_too_long;
            return RETURN_ERROR;
        }

        if (def->rvd_Index != NULL)
        {
            /* The four info getters are NetX APIs.  The ARexx Process must
               be adopted for the call, but not for decimal formatting. */
            AmiNetCaller *caller = ami_netstack_enter_alloc();

            if (caller == NULL)
            {
                *errstr = ami_rx_err_state;
                return RETURN_ERROR;
            }

            switch (var)
            {
                case RXV_ICMP: ami_rx_icmp(ip, counters); break;
                case RXV_IP:   ami_rx_ip(ip, counters);   break;
                case RXV_TCP:  ami_rx_tcp(ip, counters);  break;
                case RXV_UDP:  ami_rx_udp(ip, counters);  break;

                /* Unreachable: only those four have both an index template and
                   rvd_Read. */
                default:       ami_rx_zero(counters, RX_TCP_COUNT); break;
            }

            ami_netstack_leave_free(caller);

            if (!ami_rx_put_dec(r, counters[index]))
            {
                *errstr = ami_rx_err_too_long;
                return RETURN_ERROR;
            }

            any = TRUE;
            continue;
        }

        text = ami_rx_scalar((UWORD)var, scratch, (ULONG)sizeof(scratch));
        if (text == NULL || !ami_rx_put_str(r, text))
        {
            *errstr = ami_rx_err_too_long;
            return RETURN_ERROR;
        }

        any = TRUE;
    }

    if (!any || item != ITEM_NOTHING)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_WARN;
    }

    return RETURN_OK;
}

/* The writeable set is empty: every AmiTCP writeable variable wrote a kernel
   global this stack does not have, so a recognised name is refused with the
   AmiTCP ERR_NOWRITE rather than reported unknown. */
LONG ami_rx_setvalue(struct CSource *args, const char **errstr, AmiRxReply *r)
{
    char buf[RX_KEYWORDLEN];
    LONG var;

    buf[0] = '\0';

    if (ReadItem((STRPTR)buf, (LONG)sizeof(buf), args) <= 0)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_WARN;
    }

    var = FindArg((CONST_STRPTR)ami_rx_vars, (CONST_STRPTR)buf);

    ami_rx_error_named(r, errstr,
                       (var < 0) ? ami_rx_err_illegal_var : ami_rx_err_nowrite,
                       "setvalue", buf);

    return RETURN_WARN;
}
