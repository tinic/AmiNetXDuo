#ifndef NETSTACK_REXX_H
#define NETSTACK_REXX_H
/*
 * AmiNetXDuo -- what the AMITCP host and its variable space share.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/rdargs.h>

/* AmiTCP: KEYWORDLEN 24 (kern/amiga_config.h). */
#define RX_KEYWORDLEN   24

/*
 * AmiTCP's REPLYBUFLEN was 255 and its own netstat asks for 46 TCP counters in
 * one QUERY. Eleven digits each overruns 255, so on a machine that had moved
 * any traffic AmiTCP answered its own netstat with "Result too long". 1024
 * holds the longest query any of the corpus scripts sends.
 */
#define RX_REPLYBUFLEN  1024

/*
 * The reply, and the room for it. Starts as the host's stack buffer;
 * CONNECTIONS and ROUTES grow it, because their length is a function of how
 * many sockets and routes exist. AmiTCP did the same in CS_Alloc(), leaving
 * the old buffer for its caller to free.
 */
typedef struct AmiRxReply
{
    STRPTR  rr_Buffer;
    LONG    rr_Length;      /* usable bytes, excluding the NUL */
    LONG    rr_Used;
    STRPTR  rr_Alloc;       /* AllocVec()ed, or NULL while on the stack */
} AmiRxReply;

VOID ami_rx_reply_init(AmiRxReply *r, STRPTR buffer, LONG length);
VOID ami_rx_reply_done(AmiRxReply *r);

LONG ami_rx_getvalue(struct CSource *args, const char **errstr, AmiRxReply *r);
LONG ami_rx_setvalue(struct CSource *args, const char **errstr, AmiRxReply *r);

/* AmiTCP's kern/amiga_config.c strings, so a script that prints one sees what
   AmiTCP printed. The two with %s take "getvalue"/"setvalue" and the name. */
extern const char ami_rx_err_unknown[];
extern const char ami_rx_err_syntax[];
extern const char ami_rx_err_illegal_var[];
extern const char ami_rx_err_illegal_ind[];
extern const char ami_rx_err_too_long[];
extern const char ami_rx_err_memory[];
extern const char ami_rx_err_nowrite[];
extern const char ami_rx_err_unimpl[];
extern const char ami_rx_err_state[];

#endif /* NETSTACK_REXX_H */
