/*
 * AmiNetXDuo, the resolver's retransmission ladder.
 *
 * The DNS client in NetX Duo used to own this. It took the caller's timeout as
 * a per-query wait and spent it NX_DNS_MAX_RETRIES times over every server,
 * doubling between rounds, with its mutex held throughout. That is why one
 * gethostbyname() against unreachable servers blocked for minutes and could not
 * be interrupted. NX_DNS_MAX_RETRIES is 1 in port/netxduo-amiga/inc/nx_user.h
 * now, so a call into the DNS client is one round, and the loop below decides
 * what happens between rounds.
 *
 * Nothing here knows about DNS. It drives an opaque function that asks once and
 * waits a given time, which keeps it testable on the development host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_RETRY_H
#define AMINETXDUO_NETSTACK_RETRY_H

/* tx_api.h and nx_api.h before any exec header: <exec/types.h> turns VOID into
   a macro and that breaks the ThreadX typedefs. */
#include "tx_api.h"
#include "nx_api.h"

#include <exec/types.h>

#include "aminetxduo/netstack.h"

/* What one attempt did with the wait it was given. */
typedef enum
{
    AMI_NET_ASK_ANSWERED = 0,   /* got what the caller wanted, stop         */
    AMI_NET_ASK_SILENT,         /* nobody answered, ask again               */
    AMI_NET_ASK_REFUSED         /* answered without an address, stop        */
} AmiNetAskResult;

/* One attempt, narrowed to (wait) -> outcome. */
typedef AmiNetAskResult (*AmiNetAskFn)(VOID *arg, ULONG wait_ticks);

/* How the ladder ended. */
typedef enum
{
    AMI_NET_LADDER_ANSWERED = 0,
    AMI_NET_LADDER_REFUSED,     /* a server answered; the name is not there  */
    AMI_NET_LADDER_SILENT,      /* the budget ran out with nobody answering  */
    AMI_NET_LADDER_ABORTED      /* give_up() said so                         */
} AmiNetLadderResult;

/*
 * The first query's timeout and the ceiling the doubling stops at.
 *
 * RFC 1035 4.2.1 wants a client to try other servers before it repeats itself,
 * and to back off. The ceiling bounds how long a Ctrl-C waits, because the
 * break signal is sampled only between rungs. Two seconds against five
 * configured servers is a ten-second worst case. One or two servers, which is
 * what a lease hands out, is two to four.
 */
#define AMI_NET_ASK_FIRST       (1UL * (ULONG)NX_IP_PERIODIC_RATE)
#define AMI_NET_ASK_CEILING     (2UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * Ask until something answers, the budget is gone, or give_up() says stop.
 *
 * `budget` is the whole lookup in ticks. Each attempt is charged what it took
 * rather than what it was allowed, because one attempt walks every configured
 * server and so can cost several times its own wait.
 *
 * An attempt that comes back before its wait expired means every server it
 * asked answered something. A server that answers does not answer differently
 * a second later, so the ladder stops there rather than spend the rest of the
 * budget on re-asking. That also keeps a mistyped host name fast while a
 * black-holed one still gets the full budget.
 */
AmiNetLadderResult ami_net_ask_until(AmiNetAskFn ask, VOID *arg, ULONG budget,
                                     AmiNetGiveUpFn give_up,
                                     VOID *give_up_arg);

#endif /* AMINETXDUO_NETSTACK_RETRY_H */
