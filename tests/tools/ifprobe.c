/*
 * IfProbe -- exercises the Roadshow interface query API on a running stack.
 *
 * ObtainInterfaceList(), ReleaseInterfaceList() and QueryInterfaceTagList()
 * are the three vectors Roadie, NetMon and RoadshowControl reach for first.
 * They pass a struct List of Nodes and a tag list of pointers to caller
 * storage, neither of which a build can check: a list of the wrong node type
 * or a tag that writes a value where a pointer was expected compiles
 * perfectly and gurus inside the application.
 *
 * This is the call sequence a monitor makes, from a separate executable that
 * knows nothing about this stack's internals, against the library on a booted
 * machine:
 *
 *   1. list the interfaces and print ln_Name for each, so the caller walks the
 *      list with nothing but the published Node layout;
 *   2. query the first one for a bundle of tags in one call, which is how a
 *      monitor asks and the only way to catch a case that falls through into
 *      its neighbour;
 *   3. query a name that does not exist, which must fail with -1 and leave a
 *      sensible errno rather than succeed quietly;
 *   4. query with an empty tag list, which must succeed -- that is how a
 *      caller asks "does this interface exist?";
 *   5. release the list.
 *
 * The tag storage is poisoned first: every destination is filled with 0xA5
 * before the query, so a tag that was silently not answered is distinguishable
 * from one that answered zero.  Half of the published tags have no true value
 * on this stack and are documented to be left alone; pre-zeroing could not
 * tell that apart from a bug.
 *
 * Vectors are called by hand at the LVOs the ABI assigns, the same way
 * tests/tools/routeprobe.c and src/tools/toolsock.c do: the NDK inlines
 * assume a global SocketBase.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

/*
 * The IFQ_* tags come from the NDK's own header, not from a copy: a probe that
 * restated the numbers could agree with a wrong implementation.  That header
 * pulls in <sys/socket.h>, which uses size_t and ssize_t without declaring
 * them, so these two come first -- the same ordering bsdsocket_internal.h
 * documents.
 */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static struct List *p_obtain_interface_list(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register struct List    *res __asm("d0");

    __asm __volatile ("jsr a6@(-462:W)"     /* ObtainInterfaceList  -0x1ce */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static VOID p_release_interface_list(struct Library *base, struct List *list)
{
    register struct Library *a6 __asm("a6") = base;
    register struct List    *a0 __asm("a0") = list;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-456:W)"     /* ReleaseInterfaceList -0x1c8 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

static LONG p_query_interface(struct Library *base, const char *name,
                              struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            a1  __asm("a1") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-468:W)"     /* QueryInterfaceTagList -0x1d4 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG p_configure_interface(struct Library *base, const char *name,
                                  struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            a1  __asm("a1") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-450:W)"     /* ConfigureInterfaceTagList -0x1c2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG p_add_interface(struct Library *base, const char *name,
                            const char *device, LONG unit,
                            struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)device;
    register LONG            d0  __asm("d0") = unit;
    register APTR            a2  __asm("a2") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-444:W)"     /* AddInterfaceTagList -0x1bc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_remove_interface(struct Library *base, const char *name,
                               LONG force)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register LONG            d0  __asm("d0") = force;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-732:W)"     /* RemoveInterface -0x2dc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* ----------------------------------------------------------- little helpers */

/* 0xA5, not 0: a tag documented to be left alone must be visibly untouched. */
#define POISON_BYTE     0xA5
#define POISON_LONG     0xA5A5A5A5UL

static VOID p_poison(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- != 0)
        *b++ = POISON_BYTE;
}

static VOID p_dotted(ULONG net_addr, char *out)
{
    static const char digits[] = "0123456789";
    const UBYTE      *b        = (const UBYTE *)&net_addr;
    ULONG             pos      = 0;
    ULONG             i;

    for (i = 0; i < 4; i++)
    {
        ULONG v = b[i];

        if (v >= 100)
            out[pos++] = digits[v / 100];
        if (v >= 10)
            out[pos++] = digits[(v / 10) % 10];
        out[pos++] = digits[v % 10];

        if (i != 3)
            out[pos++] = '.';
    }
    out[pos] = '\0';
}

/* "unanswered" is a real result here, so it prints as one. */
static VOID p_show_long(const char *label, LONG value)
{
    if ((ULONG)value == POISON_LONG)
        Printf((CONST_STRPTR)"  %-24s unanswered\n", (LONG)label);
    else
        Printf((CONST_STRPTR)"  %-24s %ld\n", (LONG)label, value);
}

/* ------------------------------------------------------------------- query */

struct sockaddr_in_probe
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

static VOID p_show_addr(const char *label,
                        const struct sockaddr_in_probe *sa)
{
    char text[16];

    if (sa->sin_len == POISON_BYTE)
    {
        Printf((CONST_STRPTR)"  %-24s unanswered\n", (LONG)label);
        return;
    }

    p_dotted(sa->sin_addr, text);
    Printf((CONST_STRPTR)"  %-24s %s (len %ld family %ld)\n",
           (LONG)label, (LONG)text, (LONG)sa->sin_len, (LONG)sa->sin_family);
}

static VOID p_query_one(struct Library *base, const char *name)
{
    STRPTR                   device      = (STRPTR)POISON_LONG;
    LONG                     unit        = (LONG)POISON_LONG;
    LONG                     hw_bits     = (LONG)POISON_LONG;
    LONG                     hw_type     = (LONG)POISON_LONG;
    LONG                     mtu         = (LONG)POISON_LONG;
    LONG                     hw_mtu      = (LONG)POISON_LONG;
    LONG                     bps         = (LONG)POISON_LONG;
    LONG                     state       = (LONG)POISON_LONG;
    LONG                     bind_type   = (LONG)POISON_LONG;
    LONG                     metric      = (LONG)POISON_LONG;
    LONG                     debug_mode  = (LONG)POISON_LONG;
    LONG                     reads       = (LONG)POISON_LONG;
    LONG                     reads_busy  = (LONG)POISON_LONG;
    LONG                     writes      = (LONG)POISON_LONG;
    LONG                     writes_busy = (LONG)POISON_LONG;
    LONG                     ip_drops    = (LONG)POISON_LONG;
    ULONG                    rx          = POISON_LONG;
    ULONG                    tx          = POISON_LONG;
    UBYTE                    mac[8];
    struct sockaddr_in_probe addr;
    struct sockaddr_in_probe mask;
    struct sockaddr_in_probe bcast;
    struct sockaddr_in_probe dns;
    /* Documented to be left alone by this implementation -- proving it. */
    LONG                     bytes_in[2];
    LONG                     last_start[2];
    LONG                     rc;

    struct TagItem tags[] =
    {
        { IFQ_DeviceName,               (ULONG)&device      },
        { IFQ_DeviceUnit,               (ULONG)&unit        },
        { IFQ_HardwareAddressSize,      (ULONG)&hw_bits     },
        { IFQ_HardwareAddress,          (ULONG)mac          },
        { IFQ_HardwareType,             (ULONG)&hw_type     },
        { IFQ_MTU,                      (ULONG)&mtu         },
        { IFQ_HardwareMTU,              (ULONG)&hw_mtu      },
        { IFQ_BPS,                      (ULONG)&bps         },
        { IFQ_PacketsReceived,          (ULONG)&rx          },
        { IFQ_PacketsSent,              (ULONG)&tx          },
        { IFQ_Address,                  (ULONG)&addr        },
        { IFQ_NetMask,                  (ULONG)&mask        },
        { IFQ_BroadcastAddress,         (ULONG)&bcast       },
        { IFQ_PrimaryDNSAddress,        (ULONG)&dns         },
        { IFQ_State,                    (ULONG)&state       },
        { IFQ_AddressBindType,          (ULONG)&bind_type   },
        { IFQ_Metric,                   (ULONG)&metric      },
        { IFQ_GetDebugMode,             (ULONG)&debug_mode  },
        { IFQ_NumReadRequests,          (ULONG)&reads       },
        { IFQ_NumReadRequestsPending,   (ULONG)&reads_busy  },
        { IFQ_NumWriteRequests,         (ULONG)&writes      },
        { IFQ_NumWriteRequestsPending,  (ULONG)&writes_busy },
        { IFQ_IPDrops,                  (ULONG)&ip_drops    },
        { IFQ_GetBytesIn,               (ULONG)bytes_in     },
        { IFQ_LastStart,                (ULONG)last_start   },
        /* A tag nothing defines: must be ignored, not fallen through. */
        { TAG_USER + 31337,             0                   },
        { TAG_DONE,                     0                   }
    };

    p_poison(mac, sizeof(mac));
    p_poison(&addr, sizeof(addr));
    p_poison(&mask, sizeof(mask));
    p_poison(&bcast, sizeof(bcast));
    p_poison(&dns, sizeof(dns));
    p_poison(bytes_in, sizeof(bytes_in));
    p_poison(last_start, sizeof(last_start));

    rc = p_query_interface(base, name, tags);
    Printf((CONST_STRPTR)"query %s: rc %ld (errno %ld)\n",
           (LONG)name, rc, p_errno(base));

    if (rc != 0)
        return;

    if ((ULONG)device == POISON_LONG)
        Printf((CONST_STRPTR)"  %-24s unanswered\n", (LONG)"IFQ_DeviceName");
    else
        Printf((CONST_STRPTR)"  %-24s %s\n", (LONG)"IFQ_DeviceName",
               (LONG)(device != NULL ? (const char *)device : "(null)"));

    p_show_long("IFQ_DeviceUnit", unit);
    p_show_long("IFQ_HardwareAddressSize", hw_bits);

    if (mac[0] == POISON_BYTE && mac[1] == POISON_BYTE)
    {
        Printf((CONST_STRPTR)"  %-24s unanswered\n", (LONG)"IFQ_HardwareAddress");
    }
    else
    {
        Printf((CONST_STRPTR)"  %-24s %02lx:%02lx:%02lx:%02lx:%02lx:%02lx"
                             " (7th byte %02lx)\n",
               (LONG)"IFQ_HardwareAddress",
               (LONG)mac[0], (LONG)mac[1], (LONG)mac[2],
               (LONG)mac[3], (LONG)mac[4], (LONG)mac[5], (LONG)mac[6]);
    }

    p_show_long("IFQ_HardwareType", hw_type);
    p_show_long("IFQ_MTU", mtu);
    p_show_long("IFQ_HardwareMTU", hw_mtu);
    p_show_long("IFQ_BPS", bps);
    p_show_long("IFQ_PacketsReceived", (LONG)rx);
    p_show_long("IFQ_PacketsSent", (LONG)tx);

    p_show_addr("IFQ_Address", &addr);
    p_show_addr("IFQ_NetMask", &mask);
    p_show_addr("IFQ_BroadcastAddress", &bcast);
    p_show_addr("IFQ_PrimaryDNSAddress", &dns);

    p_show_long("IFQ_State", state);
    p_show_long("IFQ_AddressBindType", bind_type);
    p_show_long("IFQ_Metric", metric);
    p_show_long("IFQ_GetDebugMode", debug_mode);
    p_show_long("IFQ_NumReadRequests", reads);
    p_show_long("IFQ_NumReadRequestsPending", reads_busy);
    p_show_long("IFQ_NumWriteRequests", writes);
    p_show_long("IFQ_NumWriteRequestsPending", writes_busy);
    p_show_long("IFQ_IPDrops", ip_drops);

    /* These two must still read as poison: nothing here keeps them. */
    p_show_long("IFQ_GetBytesIn", bytes_in[0]);
    p_show_long("IFQ_LastStart", last_start[0]);
}

/* --------------------------------------------------------- configuration -- */

/* One IFQ_ tag, fetched on its own, for checking what a configure call did. */
static LONG p_read_long(struct Library *base, const char *name, Tag tag)
{
    LONG           value = (LONG)POISON_LONG;
    struct TagItem tags[2];

    tags[0].ti_Tag  = tag;
    tags[0].ti_Data = (ULONG)&value;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    if (p_query_interface(base, name, tags) != 0)
        return (LONG)POISON_LONG;

    return value;
}

static ULONG p_read_address(struct Library *base, const char *name, Tag tag)
{
    struct sockaddr_in_probe sa;
    struct TagItem           tags[2];

    p_poison(&sa, sizeof(sa));

    tags[0].ti_Tag  = tag;
    tags[0].ti_Data = (ULONG)&sa;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    if (p_query_interface(base, name, tags) != 0)
        return POISON_LONG;

    return sa.sin_addr;
}

/*
 * ConfigureInterfaceTagList(), exercised on the interface carrying the run.
 * Every change is put back afterwards, and the order matters: a failure
 * part-way must leave the machine reachable, so the address is restored
 * before the state is touched.
 */
static VOID p_config_phase(struct Library *base, const char *name)
{
    static char    addr_text[16];
    static char    mask_text[16];
    ULONG          original;
    ULONG          seen;
    LONG           rc;
    LONG           mtu;
    LONG           state;
    struct TagItem tags[4];

    original = p_read_address(base, name, IFQ_Address);
    if (original == POISON_LONG)
    {
        Printf((CONST_STRPTR)"config: cannot read the address, skipping\n");
        return;
    }

    p_dotted(original, addr_text);
    Printf((CONST_STRPTR)"config: starting from %s\n", (LONG)addr_text);

    /* ---- a tag this stack refuses, and the atomicity that goes with it ---
     *
     * A non-zero IFC_Metric asks for a routing cost this stack cannot spend,
     * so the call must fail.  The IFC_NetMask in front of it checks atomicity:
     * the whole list is validated before any of it is applied, so the mask
     * must not have changed.  A one-pass implementation passes the first half
     * of this assertion and fails the second.
     */
    p_dotted(0xFFFF0000UL, mask_text);          /* 255.255.0.0, not ours */

    tags[0].ti_Tag  = IFC_NetMask;
    tags[0].ti_Data = (ULONG)mask_text;
    tags[1].ti_Tag  = IFC_Metric;
    tags[1].ti_Data = 3;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);
    Printf((CONST_STRPTR)"config: mask+metric: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    seen = p_read_address(base, name, IFQ_NetMask);
    p_dotted(seen, mask_text);
    Printf((CONST_STRPTR)"config: mask after the refusal: %s%s\n",
           (LONG)mask_text,
           (LONG)((seen == 0xFFFFFF00UL) ? " -- unchanged, correctly"
                                         : " -- CHANGED, WRONG"));

    /* IFQ_Metric answers 0 for every interface here, so an IFC_Metric of 0
       names what the stack already does and is not a change to refuse. */
    tags[0].ti_Tag  = IFC_Metric;
    tags[0].ti_Data = 0;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);
    Printf((CONST_STRPTR)"config: metric 0: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == 0) ? " -- accepted, correctly" : " -- REFUSED, WRONG"));

    /* ---- an address string that is neither dotted-quad nor a host --------- */
    tags[0].ti_Tag  = IFC_Address;
    tags[0].ti_Data = (ULONG)"999.1.2.3.4";
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);
    Printf((CONST_STRPTR)"config: bad address: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- the BOOL tags at FALSE, which ask for nothing --------------------
     *
     * IFC_AssociatedRoute and IFC_SetDebugMode name behaviour this stack does
     * not have, but FALSE is not a request for it, and refusing a whole
     * configuration over a tag that asked for nothing is how a Roadshow tool
     * fails to configure an interface at all.  TRUE is still refused.
     */
    tags[0].ti_Tag  = IFC_AssociatedRoute;
    tags[0].ti_Data = FALSE;
    tags[1].ti_Tag  = IFC_SetDebugMode;
    tags[1].ti_Data = FALSE;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);
    Printf((CONST_STRPTR)"config: BOOL tags at FALSE: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == 0) ? " -- accepted, correctly" : " -- REFUSED, WRONG"));

    tags[0].ti_Tag  = IFC_AssociatedRoute;
    tags[0].ti_Data = TRUE;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);
    Printf((CONST_STRPTR)"config: IFC_AssociatedRoute TRUE: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- the MTU, down and back ------------------------------------------ */
    tags[0].ti_Tag  = IFC_LimitMTU;
    tags[0].ti_Data = 576;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc  = p_configure_interface(base, name, tags);
    mtu = p_read_long(base, name, IFQ_MTU);
    Printf((CONST_STRPTR)"config: IFC_LimitMTU 576: rc %ld, IFQ_MTU now %ld\n",
           rc, mtu);

    /* Above the hardware's 1500: clamped rather than refused, and the clamp
       is what puts the interface back the way it was. */
    tags[0].ti_Tag  = IFC_LimitMTU;
    tags[0].ti_Data = 9000;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc  = p_configure_interface(base, name, tags);
    mtu = p_read_long(base, name, IFQ_MTU);
    Printf((CONST_STRPTR)"config: IFC_LimitMTU 9000: rc %ld, IFQ_MTU now %ld\n",
           rc, mtu);

    /* ---- the address, changed and put straight back ---------------------- */
    p_dotted((original & 0xFFFFFF00UL) | 200UL, addr_text);

    tags[0].ti_Tag  = IFC_Address;
    tags[0].ti_Data = (ULONG)addr_text;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc   = p_configure_interface(base, name, tags);
    seen = p_read_address(base, name, IFQ_Address);
    p_dotted(seen, mask_text);
    Printf((CONST_STRPTR)"config: address -> %s: rc %ld, IFQ_Address now %s\n",
           (LONG)addr_text, rc, (LONG)mask_text);

    p_dotted(original, addr_text);
    tags[0].ti_Tag  = IFC_Address;
    tags[0].ti_Data = (ULONG)addr_text;
    tags[1].ti_Tag  = IFC_NetMask;
    tags[1].ti_Data = (ULONG)"255.255.255.0";
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc   = p_configure_interface(base, name, tags);
    seen = p_read_address(base, name, IFQ_Address);
    p_dotted(seen, mask_text);
    Printf((CONST_STRPTR)"config: address restored: rc %ld, IFQ_Address now %s\n",
           rc, (LONG)mask_text);

    /* ---- down, and back up ------------------------------------------------ */
    tags[0].ti_Tag  = IFC_State;
    tags[0].ti_Data = SM_Down;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc    = p_configure_interface(base, name, tags);
    state = p_read_long(base, name, IFQ_State);
    Printf((CONST_STRPTR)"config: SM_Down: rc %ld, IFQ_State now %ld%s\n",
           rc, state,
           (LONG)((state == SM_Down) ? " -- down, correctly" : " -- STILL UP, WRONG"));

    tags[0].ti_Tag  = IFC_State;
    tags[0].ti_Data = SM_Online;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc    = p_configure_interface(base, name, tags);
    state = p_read_long(base, name, IFQ_State);
    Printf((CONST_STRPTR)"config: SM_Online: rc %ld, IFQ_State now %ld%s\n",
           rc, state,
           (LONG)((state == SM_Up) ? " -- up, correctly" : " -- STILL DOWN, WRONG"));

    /* ---- a state value the API never defined ----------------------------- */
    tags[0].ti_Tag  = IFC_State;
    tags[0].ti_Data = 99;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);
    Printf((CONST_STRPTR)"config: IFC_State 99: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- and an interface that does not exist ---------------------------- */
    tags[0].ti_Tag  = IFC_State;
    tags[0].ti_Data = SM_Up;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_configure_interface(base, "nosuchif", tags);
    Printf((CONST_STRPTR)"config: nosuchif: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));
}

/* --------------------------------------------- add and remove at run time -- */

/* Interface names are file names in DEVS:NetInterfaces, and AmigaDOS file
   names are case-insensitive, so this is how the library compares them. */
static BOOL p_same_name(const char *a, const char *b)
{
    ULONG i;

    for (i = 0; ; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));

        if (ca != cb)
            return FALSE;
        if (ca == '\0')
            return TRUE;
    }
}

/* The names an interface list holds, so a caller can say what changed. */
static LONG p_count_interfaces(struct Library *base, const char *want,
                               BOOL *found)
{
    struct List *list;
    struct Node *node;
    LONG         count = 0;

    if (found != NULL)
        *found = FALSE;

    list = p_obtain_interface_list(base);
    if (list == NULL)
        return -1;

    for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
    {
        count++;

        if (want != NULL && found != NULL && node->ln_Name != NULL &&
            p_same_name((const char *)node->ln_Name, want))
            *found = TRUE;
    }

    p_release_interface_list(base, list);

    return count;
}

/*
 * RemoveInterface() and AddInterfaceTagList(), on the interface this run is
 * riding on.  "It tries to release all the resources associated with a
 * networking interface, thus permitting it to be added again with new
 * parameters" -- removing and re-adding is the documented use, and the only
 * way to find out whether the SANA-II device was really closed and reopened.
 *
 * The hardware address is the evidence.  It is read from the card by
 * S2_DEVICEQUERY at open time, so a re-added interface that reports the same
 * MAC went all the way down to the device and back; one that reports zeroes,
 * or the previous value out of memory that was never freed, did not.
 *
 * The addressing is the other half.  AddInterfaceTagList() has no address,
 * netmask, gateway or broadcast tag, so the interface comes back BARE and the
 * ConfigureInterfaceTagList() below is what addresses it -- "such as setting
 * interface addresses, status and routing metrics".  Both halves are printed:
 * what the freshly added interface is carrying, which must be nothing, and
 * what it is carrying after the configure, which must be what was asked for
 * and not the classful mask this stack falls back to when a caller supplies an
 * address with no mask.
 */
static VOID p_addremove_phase(struct Library *base, const char *name,
                              const char *device, LONG unit)
{
    UBYTE          mac_before[8];
    UBYTE          mac_after[8];
    struct TagItem tags[5];
    BOOL           present;
    ULONG          mask_before;
    ULONG          mask_after;
    char           mask_text[16];
    LONG           before;
    LONG           after;
    LONG           rc;
    ULONG          i;

    p_poison(mac_before, sizeof(mac_before));
    p_poison(mac_after, sizeof(mac_after));

    mask_before = p_read_address(base, name, IFQ_NetMask);

    tags[0].ti_Tag  = IFQ_HardwareAddress;
    tags[0].ti_Data = (ULONG)mac_before;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;
    (VOID)p_query_interface(base, name, tags);

    before = p_count_interfaces(base, name, &present);
    Printf((CONST_STRPTR)"addremove: %ld interface(s), %s is %s\n",
           before, (LONG)name, (LONG)(present ? "there" : "MISSING"));

    /* ---- a name that is not there ---------------------------------------- */
    rc = p_remove_interface(base, "nosuchif", 0);
    Printf((CONST_STRPTR)"remove nosuchif: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- and the real one ------------------------------------------------ */
    rc = p_remove_interface(base, name, 0);
    Printf((CONST_STRPTR)"remove %s: rc %ld (errno %ld)%s\n",
           (LONG)name, rc, p_errno(base),
           (LONG)((rc != 0) ? " -- removed, correctly" : " -- REFUSED, WRONG"));

    after = p_count_interfaces(base, name, &present);
    Printf((CONST_STRPTR)"after remove: %ld interface(s), %s is %s%s\n",
           after, (LONG)name, (LONG)(present ? "STILL THERE" : "gone"),
           (LONG)((after == before - 1 && !present) ? " -- correctly"
                                                    : " -- WRONG"));

    /* Everything about it must now be unanswerable. */
    rc = p_query_interface(base, name, NULL);
    Printf((CONST_STRPTR)"query the removed %s: rc %ld (errno %ld)%s\n",
           (LONG)name, rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- put it back ------------------------------------------------------
     *
     * With a tag this stack refuses in the list, first: AddInterfaceTagList
     * must fail as a whole and leave nothing half-created, or the retry below
     * would hit "an interface of that name already exists".
     *
     * PFM_Everything is the refusable kind of tag -- it asks for promiscuous
     * capture, and a caller told it got that while seeing local traffic only
     * would read a busy wire as a quiet one.
     */
    tags[0].ti_Tag  = IFA_PacketFilterMode;
    tags[0].ti_Data = PFM_Everything;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_add_interface(base, name, device, unit, tags);
    Printf((CONST_STRPTR)"add with an unsupported tag: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /*
     * And with the tuning tags a Roadshow caller passes as a matter of course.
     * None of them is implemented here and every one of them must be accepted:
     * they change nothing this API or the wire can show, and refusing one
     * refused the interface itself.
     */
    tags[0].ti_Tag  = IFA_NumReadRequests;
    tags[0].ti_Data = 64;
    tags[1].ti_Tag  = IFA_CopyMode;
    tags[1].ti_Data = CM_FastWordCopy;
    tags[2].ti_Tag  = IFA_PacketFilterMode;
    tags[2].ti_Data = PFM_Local;
    /* Honoured rather than ignored, and FALSE is the documented default, so
       the SM_Down below this run leaves the device on the network. */
    tags[3].ti_Tag  = IFA_DownGoesOffline;
    tags[3].ti_Data = FALSE;
    tags[4].ti_Tag  = TAG_DONE;
    tags[4].ti_Data = 0;

    rc = p_add_interface(base, name, device, unit, tags);
    Printf((CONST_STRPTR)"add %s (%s unit %ld): rc %ld (errno %ld)%s\n",
           (LONG)name, (LONG)device, unit, rc, p_errno(base),
           (LONG)((rc == 0) ? " -- added, correctly" : " -- REFUSED, WRONG"));

    after = p_count_interfaces(base, name, &present);
    Printf((CONST_STRPTR)"after add: %ld interface(s), %s is %s%s\n",
           after, (LONG)name, (LONG)(present ? "there" : "MISSING"),
           (LONG)((after == before && present) ? " -- correctly" : " -- WRONG"));

    /* A second add of the same name must be refused: "Each such device must
       be assigned a unique interface name." */
    rc = p_add_interface(base, name, device, unit, NULL);
    Printf((CONST_STRPTR)"add %s twice: rc %ld (errno %ld)%s\n",
           (LONG)name, rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- the evidence ----------------------------------------------------- */
    tags[0].ti_Tag  = IFQ_HardwareAddress;
    tags[0].ti_Data = (ULONG)mac_after;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;
    (VOID)p_query_interface(base, name, tags);

    for (i = 0; i < 6; i++)
    {
        if (mac_after[i] != mac_before[i])
            break;
    }

    Printf((CONST_STRPTR)"hardware address after the round trip: "
                         "%02lx:%02lx:%02lx:%02lx:%02lx:%02lx%s\n",
           (LONG)mac_after[0], (LONG)mac_after[1], (LONG)mac_after[2],
           (LONG)mac_after[3], (LONG)mac_after[4], (LONG)mac_after[5],
           (LONG)((i == 6 && mac_after[0] != POISON_BYTE)
                      ? " -- the device was reopened, correctly"
                      : " -- WRONG"));

    /* ---- bare, as the published API says ---------------------------------- */
    {
        ULONG addr_now = p_read_address(base, name, IFQ_Address);
        ULONG mask_now = p_read_address(base, name, IFQ_NetMask);
        char  addr_now_text[16];
        char  mask_now_text[16];

        p_dotted(addr_now, addr_now_text);
        p_dotted(mask_now, mask_now_text);

        Printf((CONST_STRPTR)"bare after add: address %s netmask %s%s\n",
               (LONG)addr_now_text, (LONG)mask_now_text,
               (LONG)((addr_now == 0 && mask_now == 0)
                          ? " -- bare, correctly"
                          : " -- STILL ADDRESSED, WRONG"));
    }

    /* ---- and it works again ----------------------------------------------- */
    {
        static char addr_text[16] = "10.0.2.15";
        char        mask_arg[16];

        p_dotted(mask_before, mask_arg);

        tags[0].ti_Tag  = IFC_Address;
        tags[0].ti_Data = (ULONG)addr_text;
        tags[1].ti_Tag  = IFC_NetMask;
        tags[1].ti_Data = (ULONG)mask_arg;
        tags[2].ti_Tag  = IFC_State;
        tags[2].ti_Data = SM_Online;
        tags[3].ti_Tag  = TAG_DONE;
        tags[3].ti_Data = 0;

        rc = p_configure_interface(base, name, tags);
        Printf((CONST_STRPTR)"reconfigure and bring up: rc %ld (errno %ld)\n",
               rc, p_errno(base));

        Printf((CONST_STRPTR)"state after: %ld, address %ld%s\n",
               p_read_long(base, name, IFQ_State),
               (LONG)p_read_address(base, name, IFQ_Address),
               (LONG)((p_read_long(base, name, IFQ_State) == SM_Up)
                          ? " -- up again, correctly" : " -- STILL DOWN, WRONG"));
    }

    /*
     * The mask the configure put on it, against the one the interface left
     * with, which is what IFC_NetMask asked for.  A stack that guessed the
     * classful mask anyway reads /8 here where it was handed a /24.
     */
    mask_after = p_read_address(base, name, IFQ_NetMask);
    p_dotted(mask_after, mask_text);

    Printf((CONST_STRPTR)"netmask after the round trip: %s%s\n",
           (LONG)mask_text,
           (LONG)((mask_after == mask_before)
                      ? " -- the mask it had, correctly"
                      : " -- NOT THE MASK IT HAD, WRONG"));
}

/* ------------------------------------------------------------------ main -- */

/*
 * `IfProbe DOWN|OFFLINE|UP` moves the first interface to that state and does
 * nothing else, so that `netstat` can be run after it.  SM_Down and SM_Offline
 * both report IFQ_State == SM_Down -- the difference between them is whether
 * the SANA-II device is still on the network, which the published API has no
 * way to ask.  netstat prints it, so the harness reads it from there.
 *
 * `word` is the first letter of the argument, read by ReadArgs before the
 * library is opened.  argv is not used: the startup this builds against does
 * not fill it in, which ClockSet documents by taking its own argument through
 * ReadArgs and casting argv to void.
 */
static LONG p_state_only(struct Library *base, char word)
{
    struct List    *list = p_obtain_interface_list(base);
    struct TagItem  tags[2];
    struct Node    *node;
    const char     *name  = NULL;
    const char     *label;
    LONG            state;
    LONG            rc;

    if (list == NULL)
        return RETURN_FAIL;

    node = list->lh_Head;
    if (node->ln_Succ != NULL)
        name = (const char *)node->ln_Name;

    if (name == NULL)
    {
        p_release_interface_list(base, list);
        Printf((CONST_STRPTR)"state: no interface\n");
        return RETURN_FAIL;
    }

    if (word == 'D' || word == 'd')
    {
        state = SM_Down;
        label = "DOWN";
    }
    else if (word == 'O' || word == 'o')
    {
        state = SM_Offline;
        label = "OFFLINE";
    }
    else
    {
        state = SM_Up;
        label = "UP";
    }

    tags[0].ti_Tag  = IFC_State;
    tags[0].ti_Data = (ULONG)state;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_configure_interface(base, name, tags);

    Printf((CONST_STRPTR)"state %s: rc %ld (errno %ld), IFQ_State now %ld\n",
           (LONG)label, rc, p_errno(base),
           p_read_long(base, name, IFQ_State));

    p_release_interface_list(base, list);

    return (rc == 0) ? RETURN_OK : RETURN_FAIL;
}


/* ---------------------------------------------- RFC 3493 4, revision 3 -- */
/*
 * -0x372..-0x384. No NDK glue names these, so they are called the same way
 * everything else in this file is. A library older than revision 3 has
 * MakeLibrary()'s (APTR)-1 in these slots, so lib_Revision is checked once
 * before any of them is entered.
 */

struct probe_if_nameindex { ULONG if_index; char *if_name; };

static ULONG p_if_nametoindex(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register ULONG           res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-882:W)"     /* if_nametoindex -0x372 */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (a0)
                      : "d2", "d3", "a1", "cc", "memory");
    return res;
}

static char *p_if_indextoname(struct Library *base, ULONG index, char *out)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = index;
    register char           *a0  __asm("a0") = out;
    register char           *res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-888:W)"     /* if_indextoname -0x378 */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (a0)
                      : "d2", "d3", "a1", "cc", "memory");
    return res;
}

static struct probe_if_nameindex *p_if_nameindex(struct Library *base)
{
    register struct Library           *a6  __asm("a6") = base;
    register struct probe_if_nameindex *res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-894:W)"     /* if_nameindex -0x37e */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6)
                      : "d2", "d3", "a0", "a1", "cc", "memory");
    return res;
}

static VOID p_if_freenameindex(struct Library *base,
                               struct probe_if_nameindex *ptr)
{
    register struct Library            *a6 __asm("a6") = base;
    register struct probe_if_nameindex *a0 __asm("a0") = ptr;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-900:W)"     /* if_freenameindex -0x384 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d2", "d3", "a1", "cc", "memory");
}

static VOID p_ifindex_phase(struct Library *base)
{
    struct probe_if_nameindex *ni;
    char                       name[16];
    ULONG                      n = 0;

    if (base->lib_Revision < 3)
    {
        Printf((CONST_STRPTR)"ifindex: library revision %ld, need 3 -- skipped\n",
               (LONG)base->lib_Revision);
        return;
    }

    ni = p_if_nameindex(base);
    if (ni == NULL)
    {
        Printf((CONST_STRPTR)"ifindex: if_nameindex returned NULL\n");
        return;
    }

    for (n = 0; ni[n].if_name != NULL; n++)
    {
        ULONG back;
        char *got;

        Printf((CONST_STRPTR)"ifindex: %ld = '%s'\n",
               (LONG)ni[n].if_index, (LONG)ni[n].if_name);

        /* 1-based, and the same numbers GetRouteInfo puts in rtm_index. */
        if (ni[n].if_index == 0)
            Printf((CONST_STRPTR)"ifindex: FAIL index 0 in the list\n");

        back = p_if_nametoindex(base, ni[n].if_name);
        if (back != ni[n].if_index)
            Printf((CONST_STRPTR)"ifindex: FAIL nametoindex('%s') = %ld want %ld\n",
                   (LONG)ni[n].if_name, (LONG)back, (LONG)ni[n].if_index);

        name[0] = '\0';
        got = p_if_indextoname(base, ni[n].if_index, name);
        if (got == NULL || got != name)
            Printf((CONST_STRPTR)"ifindex: FAIL indextoname(%ld) did not return its buffer\n",
                   (LONG)ni[n].if_index);
        else if (!p_same_name(name, ni[n].if_name))
            Printf((CONST_STRPTR)"ifindex: FAIL indextoname(%ld) = '%s' want '%s'\n",
                   (LONG)ni[n].if_index, (LONG)name, (LONG)ni[n].if_name);
    }

    Printf((CONST_STRPTR)"ifindex: %ld interface(s), list terminated\n", (LONG)n);

    /* "otherwise, it shall return zero. No errors are defined." */
    if (p_if_nametoindex(base, "nosuchif0") != 0)
        Printf((CONST_STRPTR)"ifindex: FAIL an unknown name did not answer 0\n");

    /* 0 is not an interface; neither is one past the table. */
    name[0] = '\0';
    if (p_if_indextoname(base, 0, name) != NULL)
        Printf((CONST_STRPTR)"ifindex: FAIL indextoname(0) answered\n");
    if (p_if_indextoname(base, 9999, name) != NULL)
        Printf((CONST_STRPTR)"ifindex: FAIL indextoname(9999) answered\n");

    p_if_freenameindex(base, ni);
    Printf((CONST_STRPTR)"ifindex: if_freenameindex returned\n");

    /* Documented to do nothing rather than to fault, as ReleaseInterfaceList
       is above. */
    p_if_freenameindex(base, NULL);
    Printf((CONST_STRPTR)"ifindex: if_freenameindex(NULL): returned\n");
}


int main(int argc, char **argv)
{
    struct Library *base;
    struct List    *list;
    struct Node    *node;
    char            first[16];
    char            state_word = '\0';
    LONG            count = 0;
    LONG            rc;

    (VOID)argv;

    /* Before the library is opened: opening it starts the whole stack, and
       ReadArgs wants the command line as the Shell left it. */
    if (argc != 0)
    {
        struct RDArgs *rda;
        LONG           args[1];

        args[0] = 0;

        rda = ReadArgs((CONST_STRPTR)"STATE", args, NULL);
        if (rda != NULL)
        {
            if (args[0] != 0)
                state_word = *(const char *)args[0];

            FreeArgs(rda);
        }
    }

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"IfProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    if (state_word != '\0')
    {
        rc = p_state_only(base, state_word);
        CloseLibrary(base);
        return rc;
    }

    first[0] = '\0';

    list = p_obtain_interface_list(base);
    if (list == NULL)
    {
        Printf((CONST_STRPTR)"ObtainInterfaceList: NULL (errno %ld)\n",
               p_errno(base));
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
    {
        const char *name = (const char *)node->ln_Name;

        count++;
        Printf((CONST_STRPTR)"interface %ld: %s\n", count,
               (LONG)(name != NULL ? name : "(no name)"));

        if (first[0] == '\0' && name != NULL)
        {
            ULONG i;

            for (i = 0; i + 1 < sizeof(first) && name[i] != '\0'; i++)
                first[i] = name[i];
            first[i] = '\0';
        }
    }

    Printf((CONST_STRPTR)"ObtainInterfaceList: %ld interface(s)\n", count);

    if (first[0] != '\0')
        p_query_one(base, first);

    /*
     * A name nothing answers to.  The autodoc gives no errno for this, so the
     * assertion is only that it fails: a query that returns 0 having written
     * nothing is the failure mode a monitor cannot see.
     */
    rc = p_query_interface(base, "nosuchif", NULL);
    Printf((CONST_STRPTR)"query nosuchif: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* An empty tag list on a real interface is "does this exist?", and is a
       success. */
    if (first[0] != '\0')
    {
        rc = p_query_interface(base, first, NULL);
        Printf((CONST_STRPTR)"query %s with no tags: rc %ld%s\n",
               (LONG)first, rc,
               (LONG)((rc == 0) ? " -- accepted, correctly" : " -- REFUSED, WRONG"));
    }

    /*
     * The configuration half, last, on the interface this run is riding on.
     * Everything it changes it puts back, and it runs after the list
     * assertions so that a machine it fails to restore has already produced
     * the transcript for those.
     */
    if (first[0] != '\0')
        p_config_phase(base, first);

    /*
     * The list obtained above is released before the interface it names is
     * removed.  It is documented as "a copy", but nothing published says a
     * list survives its interfaces, so the probe does not rely on it.
     */
    p_release_interface_list(base, list);
    list = NULL;

    if (first[0] != '\0')
        p_addremove_phase(base, first, "a2065.device", 0);

    Printf((CONST_STRPTR)"ReleaseInterfaceList: returned\n");

    /* Documented to do nothing rather than to fault. */
    p_release_interface_list(base, NULL);
    Printf((CONST_STRPTR)"ReleaseInterfaceList(NULL): returned\n");

    p_ifindex_phase(base);

    CloseLibrary(base);

    return RETURN_OK;
}
