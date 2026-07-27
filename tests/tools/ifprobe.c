/*
 * IfProbe -- does the Roadshow interface QUERY API work on a running stack?
 *
 * ObtainInterfaceList(), ReleaseInterfaceList() and QueryInterfaceTagList()
 * are the three vectors Roadie, NetMon and RoadshowControl reach for first.
 * They pass a struct List of Nodes and a tag list of pointers to caller
 * storage, and both of those are shapes that a build cannot check: a list of
 * the wrong node type or a tag that writes a value where a pointer was
 * expected compiles perfectly and gurus inside the application.
 *
 * So this is not a unit test of the tag switch. It is the same call sequence
 * a monitor makes, made from a separate executable that knows nothing about
 * this stack's internals, against the library on a booted machine:
 *
 *   1. list the interfaces and print ln_Name for each -- proving the caller
 *      can walk the list with nothing but the published Node layout;
 *   2. query the FIRST one for a bundle of tags in one call, which is how a
 *      monitor asks and is also the only way to catch a case that falls
 *      through into its neighbour;
 *   3. query a name that does not exist, which must fail with -1 and leave a
 *      sensible errno rather than succeed quietly;
 *   4. query with an empty tag list, which must succeed -- that is how a
 *      caller asks "does this interface exist?";
 *   5. release the list.
 *
 * THE TAG STORAGE IS POISONED FIRST, on purpose. Every destination is filled
 * with 0xA5 before the query, so a tag that was silently not answered is
 * distinguishable in the transcript from one that answered zero. Half of the
 * published tags have no true value on this stack and are documented to be
 * left alone; a test that pre-zeroed could not tell that apart from a bug.
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
 * restated the numbers could agree with a wrong implementation. That header
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

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    struct Library *base;
    struct List    *list;
    struct Node    *node;
    char            first[16];
    LONG            count = 0;
    LONG            rc;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"IfProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
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
     * A name nothing answers to. The autodoc gives no errno for this, so what
     * is asserted is only that it FAILS -- a query that returns 0 having
     * written nothing is the failure mode a monitor cannot see.
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

    p_release_interface_list(base, list);
    Printf((CONST_STRPTR)"ReleaseInterfaceList: returned\n");

    /* Documented to do nothing rather than to fault. */
    p_release_interface_list(base, NULL);
    Printf((CONST_STRPTR)"ReleaseInterfaceList(NULL): returned\n");

    CloseLibrary(base);

    return RETURN_OK;
}
