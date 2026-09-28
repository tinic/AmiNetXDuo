/*
 * The receive window a TCP socket settles at, offline (#89): the pool
 * share it opens with and may grow to, then bsd_tcp_window_settle()'s
 * decision for one interface's TCPGROWRTT, RXBUFFER and TCPWINDOWMAX.
 * Uses the stack's own arithmetic (bsdsocket_window.c); the scale and
 * the growth guard mirror socket.c and nx_tcp_packet_send_syn.c.
 *
 *   predict_window [pool=N] [payload=B] [consumers=N] [bps=N] [rtt=MS]
 *                  [growrtt=MS] [hw=B] [mss=B] [windowmax=B] [peerscale=0|1]
 *
 * With no arguments, the #89 WiFiPi A1200 arms.  Output is key=value.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    ULONG pool, payload, consumers, bps, rtt, growrtt, hw, mss, windowmax;
    ULONG peerscale;
} Arm;

static ULONG scale_for(ULONG maximum)
{
    ULONG s = 0;

    while (s < 14UL && (maximum >> s) > 65535UL)
        s++;
    return s;
}

static void predict(const char *name, const Arm *a)
{
    ULONG created = ami_bsd_tcp_window_for(a->pool, a->payload, a->consumers);
    ULONG maximum = ami_bsd_tcp_window_max_for(a->pool, a->payload,
                                               a->consumers);
    ULONG scale, want, settled;

    if (!a->peerscale)                  /* the fork pins both at 65535 */
    {
        created = maximum = 65535UL;
        scale   = 0;
    }
    else
        scale = scale_for(maximum);

    want = ami_bsd_tcp_window_chosen(created, maximum, a->bps, a->rtt,
                                     a->growrtt, a->hw, a->mss, a->windowmax);
    settled = want;
    if (want > created && (want >> scale) > 65535UL)
        settled = created;              /* socket.c: growth not expressible */

    printf("arm=%s pool=%lu consumers=%lu rtt=%lu growrtt=%lu hw=%lu "
           "windowmax=%lu created=%lu maximum=%lu scale=%lu settled=%lu "
           "advertised=%lu\n",
           name, (unsigned long)a->pool, (unsigned long)a->consumers,
           (unsigned long)a->rtt, (unsigned long)a->growrtt,
           (unsigned long)a->hw, (unsigned long)a->windowmax,
           (unsigned long)created, (unsigned long)maximum,
           (unsigned long)scale, (unsigned long)settled,
           (unsigned long)((settled >> scale) << scale));
}

int main(int argc, char **argv)
{
    static const struct { const char *name; ULONG growrtt, windowmax; } arms[] = {
        { "default",          0UL,        0UL },
        { "growrtt2",         2UL,        0UL },
        { "growrtt2_cap512k", 2UL,   524288UL },
        { "growrtt2_cap256k", 2UL,   262144UL },
        { "growrtt2_cap128k", 2UL,   131072UL },
        { "growrtt2_cap64k",  2UL,    65535UL },
        { "cap256k",          0UL,   262144UL },
        { "cap64k",           0UL,    65535UL },
    };
    Arm a = { 4096UL, (ULONG)AMI_POOL_PAYLOAD, 3UL, 0UL, 2UL, 0UL, 0UL,
              1460UL, 0UL, 1UL };
    int i;

    if (argc < 2)
    {
        for (i = 0; i < (int)(sizeof(arms) / sizeof(arms[0])); i++)
        {
            a.growrtt   = arms[i].growrtt;
            a.windowmax = arms[i].windowmax;
            predict(arms[i].name, &a);
        }
        return 0;
    }

    for (i = 1; i < argc; i++)
    {
        static const char *const keys[] = { "pool", "payload", "consumers",
            "bps", "rtt", "growrtt", "hw", "mss", "windowmax", "peerscale" };
        ULONG *slots[] = { &a.pool, &a.payload, &a.consumers, &a.bps, &a.rtt,
                           &a.growrtt, &a.hw, &a.mss, &a.windowmax,
                           &a.peerscale };
        const char *eq = strchr(argv[i], '=');
        size_t      k;

        for (k = 0; eq != NULL && k < sizeof(keys) / sizeof(keys[0]); k++)
            if (strlen(keys[k]) == (size_t)(eq - argv[i]) &&
                strncmp(argv[i], keys[k], (size_t)(eq - argv[i])) == 0)
                break;
        if (eq == NULL || k == sizeof(keys) / sizeof(keys[0]))
        {
            fprintf(stderr, "error=unknown_argument arg=%s\n", argv[i]);
            return 2;
        }
        *slots[k] = strtoul(eq + 1, NULL, 0);
    }
    predict("custom", &a);
    return 0;
}
