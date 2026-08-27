/*
 * The machine src/netstack/netstack.c runs on in the host tier.
 *
 * netstack.c is compiled whole and linked against netstack_host_env.c, which
 * answers every Exec call, every tx_amiga_* call, every NetX Duo entry point
 * and every other src/ symbol it names.  Nothing is stubbed inside netstack.c
 * itself, so what runs is the shipping netstack_startup(), netstack_shutdown(),
 * netstack_can_unload() and netstack_interface_dhcp_start().
 *
 * Everything a test needs to steer or to observe is in NetStackHostEnv below.
 * The rest of the environment succeeds silently: a call that is neither
 * scripted nor counted is one the tests make no claim about.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_HOST_ENV_H
#define AMINETXDUO_NETSTACK_HOST_ENV_H

#include "netstack_internal.h"

typedef struct NetStackHostEnv
{
    /* ---- ThreadX, the half the expunge refusal turns on ---------------- */

    UINT    tx_start_status;        /* what tx_amiga_kernel_start() answers  */
    UINT    tx_stop_status;         /* what tx_amiga_kernel_stop() answers   */
    UINT    tx_adopt_status;        /* tx_amiga_adopt_thread()               */

    ULONG   tx_starts;
    ULONG   tx_stops;               /* calls, whatever they answered         */
    ULONG   tx_stops_ok;            /* calls that answered TX_SUCCESS        */
    ULONG   baton_resets;           /* only legal after a successful stop    */

    /* ---- the DHCP client ---------------------------------------------- */

    UINT    dhcp_start_status;      /* first nx_dhcp_interface_start()       */
    UINT    dhcp_restart_status;    /* the one after a stop, if there is one */
    ULONG   dhcp_starts;
    ULONG   dhcp_stops;
    ULONG   dhcp_enables;
    ULONG   dhcp_requests;          /* nx_dhcp_interface_request_client_ip() */
    ULONG   dhcp_discovers;         /* the DISCOVER kick, a timer re-arm     */
    ULONG   dhcp_request_addr;
    UINT    dhcp_request_skip;      /* the skip_discover argument            */

    /* The call order, so "stopped, then started again" is answerable rather
       than inferred from two counters.  'e' enable, 's' start, 'x' stop,
       'r' request, 'd' the DISCOVER kick. */
    char    dhcp_trace[32];
    ULONG   dhcp_trace_len;

    UINT    dhcp_create_status;

    /* ---- the interface the configuration describes --------------------- */

    UWORD   cfg_interfaces;         /* how many ami_config_load() reports    */
    UWORD   cfg_iptype;             /* AmiIpType for every one of them       */
    BOOL    sana2_open_fails;
    LONG    sana2_open_error;

    /* ---- NetX Duo ------------------------------------------------------ */

    UINT    ip_create_status;
    ULONG   iface_address;          /* what nx_ip_interface_address_get() has */

    /* ---- Exec ---------------------------------------------------------- */

    ULONG   allocs;
    ULONG   frees;
    ULONG   forbids;                /* Forbid() minus Permit(), must end at 0 */
    LONG    forbid_depth;
    BOOL    attempt_semaphore_fails; /* the contended-lock arm of can_unload  */
} NetStackHostEnv;

extern NetStackHostEnv nsh;

/* Zero the environment and put back the defaults every test starts from:
   one DHCP interface, ThreadX and NetX Duo answering success. */
VOID nsh_reset(VOID);

/* Whether the DHCP call trace matches, e.g. "esxsd". */
int nsh_dhcp_trace_is(const char *want);

/* Forget the trace so far; bring-up writes into it too. */
VOID nsh_trace_clear(VOID);

#endif /* AMINETXDUO_NETSTACK_HOST_ENV_H */
