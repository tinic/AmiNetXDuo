/*
 * AmiNetXDuo, QUERY and SET for the AMITCP ARexx host.
 *
 * The names, their abbreviations, their order and the shape of every answer
 * come from the AmiTCP/IP 3.0b2 source: kern/variables.src for the name
 * space, kern/amiga_config.c for the parser and the error strings, and
 * kern/amiga_cstat.c for the three formatted answers (CONNECTIONS, ICMPHIST,
 * ROUTES). Nothing here is guessed from documentation.
 *
 * One name does not come from AmiTCP: SERVICES, which browses for mDNS
 * services and is documented at ami_rx_services(). AmiTCP had no discovery to
 * name, so there was nothing to copy.
 *
 * Two programs in the corpus read the variable space rather than only sending
 * KILL. The AmiTCP `netstat` asks for, in this order:
 *
 *     QUERY CONNECTIONS
 *     Q ICMP CHksum ICMP COde ...              8 names
 *     Q IP CH IP D ...                        20 names
 *     Q TCP Accepts TCP CAttem ...            46 names
 *     Q UDP Bcnoport UDP Chksum ...            9 names
 *     Q ICMPHIST
 *     QUERY ROUTES ALL
 *
 * and `rx.fingerd` sends `QUERY CONNECTIONS` alone, walking the answer eight
 * words at a time looking for a local port of 79. Both parse positionally,
 * which is why every field is fixed-width and every list has all its entries
 * whether or not they moved.
 *
 * A counter this stack does not keep answers 0, which is what AmiTCP answered
 * for a counter that had not moved. There is no way to say "unmeasured" in a
 * space-separated list of numbers, and src/bsdsocket/netstats.c makes the same
 * call for the same reason. Which of the 83 are real is listed at each fill.
 *
 * A variable this stack has no counterpart for at all -- the mbuf
 * configuration, the log console, the SANA-II debug switches -- is refused
 * with the message AmiTCP used for a variable that was not readable, rather
 * than answered 0. A zero there reads as a measurement.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "netstack_rexx.h"

#include "aminetxduo/nx_queue.h"

#include <dos/dos.h>

#include <proto/dos.h>
#include <proto/exec.h>

/* ---------------------------------------------------------------- errors, */

const char ami_rx_err_unknown[]     = "Unknown command\n";
const char ami_rx_err_syntax[]      = "Syntax error\n";
static const char ami_rx_err_illegal_var[] = "%s: unknown variable %s\n";
static const char ami_rx_err_illegal_ind[] = "%s: unknown index %s\n";
static const char ami_rx_err_too_long[]    = "Result too long\n";
static const char ami_rx_err_memory[]      = "Memory exhausted\n";
static const char ami_rx_err_nowrite[]     = "%s: Variable %s is not writeable\n";
const char ami_rx_err_unimpl[]      = "Not implemented\n";
const char ami_rx_err_state[]       = "No active stack\n";

/* ------------------------------------------------------------- name space, */

/*
 * KW_VARS, generated from variables.src by the AmiTCP kern/config_var.awk
 * with TARGETTI=C. The enum below is its order, which is what FindArg()
 * returns.
 */
static const char ami_rx_vars[] =
    "WITH,IC=ICMP,ICH=ICMPHIST,IP,T=TCP,U=UDP,CONNECTIONS,HOSTNAME,ROUTES,"
    "MBS=MBUF_STAT,MBTS=MBUF_TYPE_STATS,MBC=MBUF_CONF,LOG,TASKNAME,"
    "NTH=NTHBASE,DBSANA=DEBUGSANA,DBICMP=DEBUGICMP,DBIP=DEBUGIP,"
    "GTW=GATEWAY,REDIR=IPSENDREDIRECTS,USENS=USENAMESERVER,"
    "ULO=USELOOPBACK,TCPSND=TCP_SENDSPACE,TCPRCV=TCP_RECVSPACE,"
    "CON=CONSOLENAME,LOGF=LOGFILENAME,"
    /*
     * The one name added here, appended so that every index above it keeps the
     * value FindArg() has always returned for it.  AmiTCP had no service
     * discovery to name.  See ami_rx_services().
     */
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

/* The level-2 index templates, same generator, same order within each. */

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

/*
 * KW_ROUTES, with the warning from variables.src: "These are in the numeric
 * order as in <sys/socket.h>. Do not change the order!".  The ROUTES argument
 * is an address family number rather than a name lookup, and netstat prints
 * afamily.<number> from it.
 */
static const char ami_rx_kw_routes[] =
    "ALL,UNIX,INET,IMPLINK,PUP,CHAOS,NS,ISO,ECMA,DATAKIT,CCITT,SNA,DECnet,DLI,"
    "LAT,HYLINK,APPLETALK,ROUTE,LINK,XTP";

#define RXV_AF_ALL      0
#define RXV_AF_INET     2

/* How many counters each table has: the length of the index template. */
#define RX_ICMP_COUNT       8
#define RX_IP_COUNT         20
#define RX_TCP_COUNT        46
#define RX_UDP_COUNT        9

/* The icmpstat histograms are ICMP_MAXTYPE + 1 entries, out then in. */
#define RX_ICMP_MAXTYPE     18
#define RX_ICMP_HIST        (RX_ICMP_MAXTYPE + 1)

#define RX_ICMP_ECHO        8
#define RX_ICMP_ECHOREPLY   0

/*
 * What QUERY can answer, and what SET can change. The AmiTCP flags were
 * VF_READ/VF_WRITE/VF_CONF per variable.  The readable set here is smaller,
 * because several of those variables describe machinery this stack does not
 * have, and the writeable set is empty.  See ami_rx_setvalue().
 *
 * rvd_Index is the level-2 template, NULL for a variable that takes no index.
 * A variable whose answer is a formatted list (CONNECTIONS, ICMPHIST, ROUTES)
 * parses its own argument, exactly as the AmiTCP VAR_FUNC did, so it has no
 * template either.
 */
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

/* ------------------------------------------------------------- the reply, */

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

/* The AmiTCP CS_Alloc(), except that what is written is carried over. */
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

/*
 * An error that names the variable it is about: the AmiTCP ERR_ILLEGAL_VAR and
 * the related strings are printf formats taking the function name and the
 * keyword, and it built them into the reply buffer and pointed errstrp at it.
 * Same here, so a script that displays the message sees the name it got wrong.
 */
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

/* ------------------------------------------------------------- counters --- */

/*
 * The ARexx host is a plain CreateNewProc() Process, not a ThreadX thread.
 * Every live NetX snapshot below is therefore enclosed by
 * ami_netstack_enter_alloc()/leave_free().  Keep those brackets around the
 * snapshot only: reply growth and SERVICES' Delay() are Exec work and must
 * not hold the ThreadX baton.
 */

static VOID ami_rx_zero(ULONG *out, UWORD count)
{
    while (count-- > 0)
        out[count] = 0;
}

/*
 * ICMP, in KW_ICMP order. nx_icmp_info_get() gives one of the eight:
 * icmp_checksum_errors is CHKSUM. Its other five numbers count the outbound
 * pings of this host, which icmpstat has no member for: icmpstat is a per-type
 * histogram plus input failure causes.
 */
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

/*
 * IP, in KW_IP order, which is the member order of struct ipstat, so these
 * positions are the ones src/bsdsocket/netstats.c fills by name.
 *
 *   0  T=TOTAL       14 LO=LOCALOUT    6  FS=FRAGMENTS
 *   1  CH=CHKSUM     15 OD=ODROPPED    18 OF=OFRAGMENTS
 */
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

/*
 * TCP, in KW_TCP order, the first 46 members of struct tcpstat.
 *
 *   2  CO=CONNECT    15 ST=STOTAL      25 RTO=RTOTAL
 *   3  DR=DROPS      17 SB=SBYTE       27 RB=RBYTE
 *   5  CL=CLOSED     18 SREP=SREPACK   28 RC=RCHKSUM
 *
 * CATTEM and ACCEPTS stay 0 on purpose: the tcp_connections from
 * nx_tcp_info_get() is incoming and outgoing together and there is no basis
 * for splitting it, which is the same call netstats.c makes.
 */
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

/*
 * UDP, in KW_UDP order, the members of struct udpstat, all nine of them, and
 * six are real.
 *
 *   0  I=ITOTAL      2  C=CHKSUM       6  F=FULLSOC
 *   1  H=HEADSHORT   4  N=NOPORT       8  O=OTOTAL
 */
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

/* ------------------------------------------------------- formatted answers */

/*
 * ICMPHIST. The AmiTCP read_icmphist(): icps_outhist[0..18] then
 * icps_inhist[0..18], decimal, space separated, no trailing space. netstat
 * indexes it as subword(icmphist, a*19 + b + 1, 1), so all 38 have to be
 * there whether or not they moved.
 *
 * Two are real, the same two src/bsdsocket/netstats.c fills: pings sent is
 * outhist[ICMP_ECHO] and replies received is inhist[ICMP_ECHOREPLY].
 */
static LONG ami_rx_icmphist(NX_IP *ip, const char **errstr, AmiRxReply *r)
{
    AmiNetCaller *caller;
    ULONG hist[2 * RX_ICMP_HIST];
    ULONG sent = 0, timeouts = 0, suspended = 0, responses = 0;
    ULONG checksum = 0, unhandled = 0;
    UWORD i;

    for (i = 0; i < 2 * RX_ICMP_HIST; i++)
        hist[i] = 0;

    /* nx_icmp_info_get() takes live NetX state; format after leaving. */
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

/*
 * CONNECTIONS. The AmiTCP getsockets():
 *
 *     "%lc %04lx %04lx %08lx %04lx %08lx %04lx %1lx"
 *      proto recvq sendq laddr lport faddr fport state, entries space separated
 *
 * 't' or 'u', TCP first and then UDP, with the addresses in host order,
 * because netstat pulls them apart with substr() and x2d() rather than reading
 * them as a number. The state digit is the 4.4BSD t_state, and the netstat
 * state.0..state.A table indexes it directly.
 *
 * Which sockets, and why a listen() shows as one: the rules come from
 * src/bsdsocket/netstats.c, because a monitor reading GetNetworkStatistics()
 * and a script reading this must not be told different things. NetX Duo parks
 * a second socket on a listening port with the accept already posted.  It has
 * no descriptor, so it is skipped, and the socket of the descriptor itself,
 * which NetX Duo leaves in CLOSED, is reported as LISTEN.
 */
#define RX_STATLEN  42

/* The NetX Duo TCP states to the 4.4BSD ones. The two enumerations agree to
   CLOSE_WAIT and then diverge.  netstats.c has the full note. */
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

/*
 * Copy the sockets out under the IP's protection mutex, then format outside
 * it, which is the shape AmiTCP used with splnet(): the formatting can grow
 * the reply buffer, and allocating inside the lock risks both an unbounded
 * hold and a deadlock against the IP thread.
 */
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

/*
 * NetX Duo puts no ceiling on created sockets, so this is generous rather than
 * exact: past it the list is truncated, and the walk stops cleanly. AmiTCP
 * counted first and allocated exactly, which was possible because its counting
 * and its copying were both inside one splnet().
 */
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

    /* ami_rx_collect_sockets() takes nx_ip_protection directly. */
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

/*
 * ROUTES <family>. The AmiTCP getroutes():
 *
 *     "%02lx %08lx %08lx %-8.8s %04lx %08lx %s%ld "
 *      family dest gateway flags refs use interface
 *
 * with a trailing space after every entry, including the last, and the flag
 * field padded to eight characters, netstat parses positionally with
 * `parse value rest with af dest gate flags refs used iface rest`, so both
 * matter. An entry with no flag letters prints `""`, the AmiTCP guard against
 * confusing the parser with an empty word.
 *
 * The entries, and their flags, are the set from src/bsdsocket/routing.c: the
 * network of an interface, the NetX Duo static routing table when it is
 * compiled in, and the default gateway. Refs and use are 0, neither is
 * counted, and the netmask pseudo-family AmiTCP emitted as family 0 has no
 * counterpart, because NetX Duo has no radix tree to walk.
 */
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

/* The bound from src/bsdsocket/routing.c, minus its ARP entries: the attached
   prefix of each interface, the whole static table, and the default gateway.
   The AmiTCP netstat has no column for an ARP entry. */
#ifdef NX_ENABLE_IP_STATIC_ROUTING
#define RX_ROUTE_STATIC_MAX     NX_IP_ROUTING_TABLE_SIZE
#else
#define RX_ROUTE_STATIC_MAX     0
#endif

#define RX_MAX_ROUTES   (NX_MAX_PHYSICAL_INTERFACES + RX_ROUTE_STATIC_MAX + 1)

/* AmiTCP sized this 90, allowing 32 for the interface name.  Here it can be
   AMI_CFG_NAME_LEN. 45 is the fixed part: six hex fields, the eight-column
   flag field and their separators. */
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
        /* The interface row is reusable.  Carry its name across the mutex
           boundary with the route rather than dereferencing the numeric slot
           while formatting, after RemoveInterface() may have zeroed it or
           AddInterface() may have installed a different device there. */
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

    /* AmiTCP opens getvalue(), setvalue() and parseroute() with this line, and
       it is required: ReadItem() leaves the buffer alone when there is nothing
       to read, and the error paths below name what they read. */
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

    /* Every route here is AF_INET, so any other family is an empty answer
       rather than an error, which is what AmiTCP returned for a family whose
       radix tree was absent. */
    if (af != RXV_AF_ALL && af != RXV_AF_INET)
        return RETURN_OK;

    mem = AllocVec((ULONG)sizeof(AmiRxRoute) * RX_MAX_ROUTES, MEMF_ANY);
    if (mem == NULL)
    {
        *errstr = ami_rx_err_memory;
        return RETURN_FAIL;
    }

    /* ami_rx_collect_routes() takes nx_ip_protection and calls NetX. */
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

/* -------------------------------------------------------------- services, */

#ifdef AMINETXDUO_MDNS

/*
 * QUERY SERVICES <type|ALL> [<seconds>]
 *
 * ALL is the DNS-SD meta-query: it answers with the service types something on
 * the network offers, which is what a script asks first when it does not
 * already know what to look for. A type, "_http._tcp", answers with the
 * instances of it.
 *
 * The argument is mandatory for the same reason as the ROUTES argument:
 * getvalue() reads names until the line ends, so an optional first argument
 * cannot be told from the next variable. The seconds are optional, because a
 * number is not a variable name, so the CSource is rewound when the next item
 * is not one.
 *
 * This blocks, for three reasons.
 *
 *   mDNS answers arrive over seconds and a browse never completes, so there is
 *   nothing to return immediately and nothing to poll for except the same wait
 *   moved into the script. The alternative, one verb to start and another to
 *   read, makes every caller write the sleep itself and leaves a query running
 *   when a script exits between the two.
 *
 *   The host services one message at a time and always has, so a script that
 *   sends this stops the port for the window. That is why the window is capped
 *   and why the default is short: three seconds is longer than a responder on
 *   the same wire takes to answer and short enough that a second script waiting
 *   behind this one does not read as a hang.
 *
 *   The wait is Delay() on the task of the host itself, taken with no ThreadX
 *   bracket held.  browse_start() and browse_collect() each take and release
 *   their own. A wait taken while adopted stops the whole stack for the
 *   window, and a handshake through a MsgPort here repeats the port hang this
 *   file already fixed once.
 */

#define RX_SVC_MAX          48
#define RX_SVC_SECONDS      3
#define RX_SVC_SECONDS_MAX  30

/* Instance TAB type TAB host TAB address TAB port TAB txt, one per line.  TABs
   rather than the fixed-width hex the AmiTCP answers use, because an instance
   name is free text with spaces in it and positional parsing cannot survive
   that.  A type-only row has an empty instance, host and address and a port of
   0.  Roughly: 64 + 24 + 64 + 16 + 6 + 192 + 6 separators. */
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

/*
 * A TXT record is whatever the responder put in it. Control characters break
 * the line format this answer is parsed with, and a TAB inside a field shifts
 * every field after it, so both become '.'.  The record is still legible and
 * the row still parses.
 */
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

    /* ALL is the meta-query, which the module asks for with no type at all. */
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

/* --------------------------------------------------------------- scalars, */

/* The AmiTCP boolean_enum, and its answer is the first name of the pair. */
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

        /* In AmiTCP this was the name of its stack task, which a script used
           to find the process. Here it is the name of the NX_IP thread, the
           closest equivalent and the name that shows in a task list. */
        case RXV_TASKNAME:
            return "AmiNetXDuo IP";

        /* AmiTCP counted its own library bases, so that a second instance took
           a second port name. This library is a singleton. */
        case RXV_NTHBASE:
            return "0";

        /* No SANA-II, ICMP or IP debug logging switch exists to be on. */
        case RXV_DEBUGSANA:
        case RXV_DEBUGICMP:
        case RXV_DEBUGIP:
            return ami_rx_bool(FALSE);

        /* ipforwarding. This stack does not forward, and has no switch. */
        case RXV_GATEWAY:
            return ami_rx_bool(FALSE);

        /* Nothing sends ICMP redirects, which follows from not forwarding. */
        case RXV_IPSENDREDIRECTS:
            return ami_rx_bool(FALSE);

        /* SECOND: DEVS:Internet/hosts is consulted before the name servers,
           so that a pinned name resolves with the network down
           (src/netstack/netstack_dns.c). */
        case RXV_USENAMESERVER:
            return "SECOND";

        /* lo0 exists and carries traffic addressed to this machine. */
        case RXV_USELOOPBACK:
            return ami_rx_bool(TRUE);

        default:
            return NULL;
    }
}

/* --------------------------------------------------------------- getvalue, */

/*
 * The AmiTCP getvalue(): read names until the line runs out, appending one
 * value per name separated by a space. An unreadable or unknown name aborts
 * the whole command, because the caller splits on position and silently
 * misparses a partial answer.
 */
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

    /* HOSTNAME and the resolver variables read the live configuration, which
       DHCP and router advertisements update from NetX tasks that cannot apply
       the change themselves.  Once per GETVALUE, not once per variable named
       on it: a script asking for six of them absorbed six times. */
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

        /*
         * The formatted answers write the whole reply themselves and are not
         * space-separated from a neighbour, which is the AmiTCP VAR_FUNC path:
         * it wrote through the same CSource and skipped the separator.
         */
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

    /* AmiTCP: a QUERY that read nothing, or stopped on something that was not
       the end of the line, is a syntax error rather than an empty answer. */
    if (!any || item != ITEM_NOTHING)
    {
        *errstr = ami_rx_err_syntax;
        return RETURN_WARN;
    }

    return RETURN_OK;
}

/* --------------------------------------------------------------- setvalue, */

/*
 * SET. AmiTCP had fourteen writeable variables and three more writeable only
 * while it was reading its configuration file.  Every one of them wrote a
 * kernel global that this stack does not have. Its debug switches, log
 * console, log file and mbuf chunk sizes have no counterpart, the receive
 * window is computed per socket from the live packet pool
 * (src/bsdsocket/socket.c) so there is no tcp_recvspace to assign to, and
 * HOSTNAME comes from the configuration file.
 *
 * So the writeable set is empty, and a recognised name is refused with the
 * AmiTCP ERR_NOWRITE rather than reported unknown.
 *
 * One archive sends a SET: the startnet in TCP_Start_Stop, twice, both `SET
 * HOSTNAME`.  It is a dial-up front end, and the value it wants to install is
 * the name the local address of the link resolves to. Its other branch sets
 * the name to the one already in the configuration file, and both calls
 * discard the return code. So there is nothing here for a writeable HOSTNAME
 * to fix.
 */
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
