/*
 * AmiNetXDuo, the interface registry and the two capture taps.
 *
 * This is the whole of the coupling between src/bpf/ and the rest of the
 * stack: two lines added to src/sana2/ and one registration call from
 * whatever code starts an interface. See the block comment above the tap
 * declarations in include/aminetxduo/bpf.h for the exact call sites.
 *
 * The two taps differ because of the SANA-II cooked framing (docs/RESEARCH.md
 * 3.4). On receive the shim has already synthesised an Ethernet header, so the
 * frame is complete and contiguous, and the filter reads it where it lies. On
 * transmit no header exists anywhere: NetX Duo does not build one, and the
 * shim does not either, because the device does. The tap therefore builds the
 * 14 bytes from the three facts that the CMD_WRITE carries, and presents them
 * as segment 0 of a scatter view over the untouched NX_PACKET chain. Nothing
 * is written into the packet, and nothing is copied except the captured
 * prefix.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpf_internal.h"

AmiBpfIf ami_bpf_iface[AMI_BPF_MAX_IFACES];

/* --------------------------------------------------------------- registry */

AmiBpfIf *ami_bpf_iface_by_cookie(APTR cookie)
{
    UWORD i;

    if (cookie == NULL)
        return NULL;

    for (i = 0; i < AMI_BPF_MAX_IFACES; i++)
    {
        if (ami_bpf_iface[i].used && ami_bpf_iface[i].cookie == cookie)
            return &ami_bpf_iface[i];
    }

    return NULL;
}

AmiBpfIf *ami_bpf_iface_by_name(const char *name)
{
    UWORD i;
    UWORD n;

    if (name == NULL)
        return NULL;

    for (i = 0; i < AMI_BPF_MAX_IFACES; i++)
    {
        AmiBpfIf *ifp = &ami_bpf_iface[i];

        if (!ifp->used)
            continue;

        for (n = 0; n < AMI_BPF_IFNAMSIZ; n++)
        {
            if (ifp->name[n] != name[n])
                break;
            if (ifp->name[n] == '\0')
                return ifp;
        }
    }

    return NULL;
}

static AmiBpfAddrFn ami_bpf_addr_hook;

VOID ami_bpf_set_address_hook(AmiBpfAddrFn fn)
{
    ami_bpf_addr_hook = fn;
}

ULONG ami_bpf_cookie_address(APTR cookie)
{
    AmiBpfAddrFn fn = ami_bpf_addr_hook;

    if (fn == NULL || cookie == NULL)
        return 0;

    return fn(cookie);
}

LONG ami_bpf_attach_interface(const char *name, APTR cookie, ULONG dlt,
                              ULONG mtu, AmiBpfInjectFn inject)
{
    AmiBpfIf *ifp;
    UWORD     i;

    if (name == NULL || cookie == NULL)
        return -1;

    ami_bpf_lock();

    ifp = ami_bpf_iface_by_cookie(cookie);

    if (ifp == NULL)
    {
        for (i = 0; i < AMI_BPF_MAX_IFACES; i++)
        {
            if (!ami_bpf_iface[i].used)
            {
                ifp = &ami_bpf_iface[i];
                break;
            }
        }
    }

    if (ifp == NULL)
    {
        ami_bpf_unlock();
        return -1;
    }

    ifp->used   = TRUE;
    ifp->cookie = cookie;
    ifp->dlt    = dlt;
    ifp->mtu    = mtu;
    ifp->inject = inject;

    for (i = 0; i < AMI_BPF_IFNAMSIZ - 1 && name[i] != '\0'; i++)
        ifp->name[i] = name[i];
    for (; i < AMI_BPF_IFNAMSIZ; i++)
        ifp->name[i] = '\0';

    /* A channel that asked for this name before it existed now gets it. */
    ami_bpf_chan_rebind_locked(ifp);

    ami_bpf_unlock();

    return 0;
}

VOID ami_bpf_detach_interface(APTR cookie)
{
    AmiBpfIf *ifp;

    ami_bpf_lock();
    ifp = ami_bpf_iface_by_cookie(cookie);
    if (ifp == NULL)
    {
        ami_bpf_unlock();
        return;
    }

    /* A registry row is reusable. Unbind and clear it before releasing the
       table lock, so a stale detach cannot act on a replacement row. */
    ami_bpf_chan_unbind_locked(ifp);
    ami_bpf_zero_bytes(ifp, (ULONG)sizeof(AmiBpfIf));
    ami_bpf_unlock();
}

/* ------------------------------------------------------------- the taps */

VOID ami_bpf_tap_view(APTR cookie, const AmiBpfView *view)
{
    AmiBpfIf *ifp;

    if (ami_bpf_bound_channels == 0 || view == NULL)
        return;

    ifp = ami_bpf_iface_by_cookie(cookie);
    if (ifp == NULL)
        return;

    ami_bpf_capture(ifp, view);
}

VOID ami_bpf_tap_rx(APTR cookie, const UBYTE *frame, ULONG len)
{
    AmiBpfView view;

    if (ami_bpf_bound_channels == 0 || frame == NULL || len == 0)
        return;

    /* Already a complete link-layer frame in one run. Cooked mode synthesised
       the header in ami_sana2_rx_complete(), raw mode got it from the wire.
       No copy: the filter reads the packet buffer in place. */
    ami_bpf_view_linear(&view, frame, len);

    ami_bpf_tap_view(cookie, &view);
}
