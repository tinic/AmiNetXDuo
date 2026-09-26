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

/*
 * One interface as src/sana2 would hold it, and its place on the retained
 * list, which these stubs model: a close that is steered to fail puts it
 * there, and ami_sana2_retained_sweep() closes and frees it once the test says
 * the device gave everything back.
 */
#define NSH_SANA2_IFACES    8

#define NSH_IF_FREE         0
#define NSH_IF_OPEN         1       /* opened, with the netstack            */
#define NSH_IF_RETAINED     2       /* close refused, on the retained list  */

typedef struct NshSana2If
{
    UWORD   state;                  /* NSH_IF_*                              */
    char    device[AMI_CFG_PATH_LEN];
    ULONG   unit;
    ULONG   held;                   /* NETEVENT_HELD_*; the test clears it   */
    BOOL    join_pending;           /* the reader not joined, requests or no */
    ULONG   device_closes;          /* CloseDevice() on this one             */
    ULONG   frees;                  /* its memory given back                 */
} NshSana2If;

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

    /* nx_dhcp_interface_user_option_retrieve(): the status it answers and,
       on NX_SUCCESS, the option bytes it hands back (none is a 0-byte option). */
    UINT    dhcp_option_status;
    UCHAR   dhcp_option[16];
    UINT    dhcp_option_len;
    ULONG   dhcp_option_retrieves;

    UINT    dhcp_create_status;
    ULONG   packet_pool_creates;
    ULONG   packet_pool_deletes;

    /* ---- the interface the configuration describes --------------------- */

    UWORD   cfg_interfaces;         /* how many ami_config_load() reports    */
    UWORD   cfg_iptype;             /* AmiIpType for every one of them       */
    UWORD   cfg_static_routes;      /* zero or one persistent route          */
    ULONG   packet_releases;        /* bootstrap driver handed packets back */
    ULONG   cfg_full_loads;
    ULONG   cfg_base_loads;
    ULONG   sana2_opens;
    AmiIfConfig opened_cfg;         /* exact definition handed to SANA-II    */
    BOOL    sana2_open_fails;
    LONG    sana2_open_error;

    /* ---- SANA-II close, and the retained list ------------------------- */

    ULONG   sana2_close_held;       /* the next closes retain with this,
                                       NETEVENT_HELD_*; 0 closes            */
    BOOL    sana2_close_join;       /* ... and leave the reader unjoined     */
    BOOL    sana2_orphaned;         /* ami_sana2_orphaned(), the RX preflight*/
    ULONG   sana2_closes;           /* ami_sana2_close() calls               */
    ULONG   sana2_close_twice;      /* a close of one already closed         */
    ULONG   sana2_device_opens;     /* OpenDevice() that succeeded           */
    ULONG   sana2_device_closes;    /* CloseDevice(), close or sweep         */
    ULONG   sweeps;                 /* ami_sana2_retained_sweep() calls      */
    NshSana2If sana2[NSH_SANA2_IFACES];

    /* ---- the event ring ------------------------------------------------ */

    ULONG   events;
    UWORD   last_event;
    UWORD   last_event_index;
    ULONG   last_event_value;
    ULONG   iface_retained_events;
    ULONG   stack_retained_events;

    /* ---- live configuration API --------------------------------------- */

    BOOL    hostname_offer_accept; /* ami_config_hostname_offer() result   */
    ULONG   hostname_offers;
    ULONG   hostname_displaces;    /* DHCP's previous offer was displaced */

    /* ---- NetX Duo ------------------------------------------------------ */

    UINT    ip_create_status;
    ULONG   ip_deletes;
    UINT    ip_delete_status;       /* what the last nx_ip_delete() answered */
    ULONG   sock_resets;            /* nx_tcp_socket_disconnect(), no wait   */
    ULONG   sock_deletes;           /* TCP and UDP socket deletes accepted   */
    BOOL    sock_stuck;             /* every socket delete refuses           */
    ULONG   iface_attaches;
    ULONG   iface_detaches;
    ULONG   iface_address;          /* what nx_ip_interface_address_get() has */
    UINT    static_route_status;
    ULONG   static_route_adds;
    ULONG   static_route_dest;
    ULONG   static_route_mask;
    ULONG   static_route_gateway;

    /* ---- Exec ---------------------------------------------------------- */

    ULONG   allocs;
    ULONG   frees;
    BOOL    alloc_fails;
    ULONG   forbids;                /* Forbid() minus Permit(), must end at 0 */
    LONG    forbid_depth;
    BOOL    attempt_semaphore_fails; /* the contended-lock arm of can_unload  */
    APTR    watch_block;            /* FreeMem() of this block is recorded   */
    BOOL    watch_freed;
} NetStackHostEnv;

extern NetStackHostEnv nsh;

/* Zero the environment and put back the defaults every test starts from:
   one DHCP interface, ThreadX and NetX Duo answering success. */
VOID nsh_reset(VOID);

/* Whether the DHCP call trace matches, e.g. "esxsd". */
int nsh_dhcp_trace_is(const char *want);

/* Forget the trace so far; bring-up writes into it too. */
VOID nsh_trace_clear(VOID);

/* The modelled interface the netstack holds as `iface`, or NULL. */
NshSana2If *nsh_sana2_of(const AmiSana2If *iface);

/* Retained entries on the modelled list. */
UWORD nsh_retained(VOID);

#endif /* AMINETXDUO_NETSTACK_HOST_ENV_H */
