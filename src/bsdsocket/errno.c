/*
 * bsdsocket.library, errno, h_errno and SocketBaseTagList().
 *
 * errno is per-opener and is optionally mirrored into a caller-supplied
 * variable whose width the caller chooses (1, 2 or 4 bytes), so this cannot be
 * built on NetX Duo's own errno handling (docs/RESEARCH.md S6.4).
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/version.h"

#ifdef AMINETXDUO_BPF
#include "aminetxduo/bpf.h"
#endif

#include <proto/exec.h>
#include <stddef.h>

/* ------------------------------------------------------------------ errno, */

/*
 * SBTC_ERROR_HOOK. "Install or remove a hook which is called whenever the
 * global 'errno' or 'h_errno' variables are changed. The hook is always called
 * on the context of the caller." The register shape is the autodoc's:
 * error = hookfunc(hook, reserved, ehm) in A0, A2, A1, reserved NULL.
 *
 * The union is netmonitor.c's, for the same reason: utility/hooks.h declares
 * h_Entry with no parameters, and a plain cast trips -Wcast-function-type.
 */
typedef LONG (*BsdErrorHookFn)(register struct Hook *hook __asm("a0"),
                               register APTR reserved __asm("a2"),
                               register struct ErrorHookMsg *ehm __asm("a1"));

typedef union BsdErrorHookEntry
{
    ULONG          (*behe_Raw)(VOID);
    BsdErrorHookFn   behe_Fn;
} BsdErrorHookEntry;

static VOID bsd_error_hook(struct AmiSocketBase *base, ULONG action, LONG code)
{
    struct Hook        *hook = base->sb_ErrorHook;
    struct ErrorHookMsg ehm;
    BsdErrorHookEntry   entry;

    if (hook == NULL || hook->h_Entry == NULL)
        return;

    ehm.ehm_Size   = (ULONG)sizeof(ehm);
    ehm.ehm_Action = action;
    ehm.ehm_Code   = code;

    entry.behe_Raw = hook->h_Entry;
    (VOID)entry.behe_Fn(hook, NULL, &ehm);
}

VOID bsd_set_errno(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;

    bsd_error_hook(base, EHMA_Set_errno, code);

    if (base->sb_ErrnoPtr == NULL)
        return;

    switch (base->sb_ErrnoSize)
    {
        case 1:  *(BYTE *)base->sb_ErrnoPtr = (BYTE)code;  break;
        case 2:  *(WORD *)base->sb_ErrnoPtr = (WORD)code;  break;
        case 4:  *(LONG *)base->sb_ErrnoPtr = code;        break;
        default: break;
    }
}

VOID bsd_set_herrno(struct AmiSocketBase *base, LONG code)
{
    base->sb_HErrno = code;

    bsd_error_hook(base, EHMA_Set_h_errno, code);

    if (base->sb_HErrnoPtr != NULL)
        *base->sb_HErrnoPtr = code;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    bsd_set_errno(base, code);
    return -1;
}

/*
 * NetX Duo status -> BSD errno. Only the codes our call sites can produce are
 * listed. Anything else falls through to EIO.
 */
typedef struct
{
    UWORD   status;
    UWORD   code;
} BsdStatusMap;

static const BsdStatusMap bsd_status_map[] =
{
    { NX_SUCCESS,           0                    },
    /*
     * NX_NO_PACKET means both "nothing to read" and "the packet pool is
     * empty". EWOULDBLOCK is only right for the first. It holds here because
     * no caller can reach the second on a socket that was willing to wait.
     * Read bsd_wait_option() in select.c before a new call site is added that
     * does not go through it. New code must use bsd_wait_errno() below, which
     * checks the wait value rather than assumes this.
     */
    { NX_NO_PACKET,         AMI_EWOULDBLOCK      },
    { NX_UNDERFLOW,         AMI_EINVAL           },
    { NX_OVERFLOW,          AMI_EMSGSIZE         },
    { NX_NO_MAPPING,        AMI_EHOSTUNREACH     },
    { NX_DELETED,           AMI_EBADF            },
    { NX_POOL_ERROR,        AMI_ENOBUFS          },
    { NX_PTR_ERROR,         AMI_EFAULT           },
    { NX_WAIT_ERROR,        AMI_EINVAL           },
    { NX_SIZE_ERROR,        AMI_EMSGSIZE         },
    { NX_OPTION_ERROR,      AMI_EINVAL           },
    { NX_DELETE_ERROR,      AMI_EBUSY            },
    { NX_CALLER_ERROR,      AMI_EINVAL           },
    { NX_INVALID_PACKET,    AMI_EINVAL           },
    { NX_INVALID_SOCKET,    AMI_ENOTSOCK         },
    { NX_NOT_ENABLED,       AMI_EPROTONOSUPPORT  },
    { NX_ALREADY_ENABLED,   AMI_EEXIST           },
    { NX_ENTRY_NOT_FOUND,   AMI_ENOENT           },
    { NX_NO_MORE_ENTRIES,   AMI_ENOBUFS          },
    { NX_WAIT_ABORTED,      AMI_EINTR            },
    { NX_IP_INTERNAL_ERROR, AMI_EIO              },
    { NX_IP_ADDRESS_ERROR,  AMI_EADDRNOTAVAIL    },
    { NX_ALREADY_BOUND,     AMI_EINVAL           },
    { NX_PORT_UNAVAILABLE,  AMI_EADDRINUSE       },
    { NX_NOT_BOUND,         AMI_EDESTADDRREQ     },
    { NX_SOCKET_UNBOUND,    AMI_EDESTADDRREQ     },
    { NX_NOT_CREATED,       AMI_EBADF            },
    { NX_SOCKETS_BOUND,     AMI_EADDRINUSE       },
    { NX_NO_RESPONSE,       AMI_ETIMEDOUT        },
    { NX_POOL_DELETED,      AMI_ENOBUFS          },
    { NX_MAX_LISTEN,        AMI_ENOBUFS          },
    { NX_DUPLICATE_LISTEN,  AMI_EADDRINUSE       },
    { NX_NOT_CLOSED,        AMI_EISCONN          },
    { NX_NOT_LISTEN_STATE,  AMI_EINVAL           },
    { NX_IN_PROGRESS,       AMI_EINPROGRESS      },
    { NX_NOT_CONNECTED,     AMI_ENOTCONN         },
    { NX_WINDOW_OVERFLOW,   AMI_EMSGSIZE         },
    { NX_ALREADY_SUSPENDED, AMI_EALREADY         },
    { NX_DISCONNECT_FAILED, AMI_ENOTCONN         },
    { NX_STILL_BOUND,       AMI_EBUSY            },
    { NX_NO_FREE_PORTS,     AMI_EADDRNOTAVAIL    },
    { NX_INVALID_PORT,      AMI_EINVAL           },
    { NX_INVALID_RELISTEN,  AMI_EINVAL           },
    { NX_CONNECTION_PENDING, AMI_EALREADY        },
    { NX_TX_QUEUE_DEPTH,    AMI_ENOBUFS          },
    { NX_NOT_IMPLEMENTED,   AMI_ENOSYS           },
    { NX_NOT_SUPPORTED,     AMI_EOPNOTSUPP       },
    { NX_INVALID_INTERFACE, AMI_ENETDOWN         },
    { NX_INVALID_PARAMETERS, AMI_EINVAL          },
    { NX_NOT_FOUND,         AMI_ENOENT           },
    { NX_NO_INTERFACE_ADDRESS, AMI_EADDRNOTAVAIL },

    /* What an ICMP Destination Unreachable or Time Exceeded reported. BSD
       answers ECONNREFUSED for both protocol and port unreachable: a peer that
       runs neither the protocol nor a listener on the port has refused the
       same way as far as the caller is concerned. */
    { NX_NET_UNREACHABLE,      AMI_ENETUNREACH   },
    { NX_HOST_UNREACHABLE,     AMI_EHOSTUNREACH  },
    { NX_PROTOCOL_UNREACHABLE, AMI_ECONNREFUSED  },
    { NX_PORT_UNREACHABLE,     AMI_ECONNREFUSED  }
};

LONG bsd_errno_from_nx(UINT status)
{
    ULONG i;

    for (i = 0; i < sizeof(bsd_status_map) / sizeof(bsd_status_map[0]); i++)
    {
        if (bsd_status_map[i].status == (UWORD)status)
            return (LONG)bsd_status_map[i].code;
    }

    return AMI_EIO;
}

/*
 * The same, for a status from a call that was willing to wait.
 *
 * Three NetX Duo statuses mean "I would have had to wait": NX_NO_PACKET (no
 * data, or no packet in the pool), NX_TX_QUEUE_DEPTH and NX_WINDOW_OVERFLOW.
 * EWOULDBLOCK is right for all three only when the caller asked not to wait.
 * On a socket prepared to block for ever it tells the application to retry a
 * call that cannot succeed, the failure English Amiga Board thread 122501
 * reports against AmiTCP and Roadshow (docs/RESEARCH.md 37). ENOBUFS is what
 * that socket gets instead.
 *
 * That arm cannot be entered, and the reason is structural, not a question of
 * how hard the pool is pushed. The three statuses are what NetX Duo returns
 * when it is not allowed to wait. With a non-zero wait option it suspends
 * (nx_packet_allocate.c's `if (wait_option)`, nx_tcp_socket_send_internal.c's
 * suspension arm). The cleanup routines that resume a suspended thread with
 * one of the three, _nx_packet_pool_cleanup and _nx_tcp_transmit_cleanup, run
 * on the timeout, which NX_WAIT_FOREVER does not have. A caller that holds
 * NX_WAIT_FOREVER therefore never sees any of the three, and `impatient` is
 * true at every entry that reaches the case below. bsd_wait_sliced()
 * (select.c) closes it from the other side as well: those are the exact three
 * statuses it retries on, so a blocking call with no timeout does not even
 * return from there with one. tests/tcpdrill/scripts/blocking.drill b07
 * asserts the reachable half, on a booted guest, and says the same thing about
 * this half.
 *
 * The arm stays rather than becomes an assertion: `wait` is a parameter, the
 * statuses come from a third party, and an errno is cheaper than a mistake.
 *
 * NX_WAIT_ABORTED and NX_POOL_DELETED are handled here because the table alone
 * turns a signalled thread and a stack shutdown into "try again".
 *
 * `wait` is the value the call was given, straight from bsd_wait_option(), not
 * the socket: NX_NO_WAIT and an expired SO_RCVTIMEO/SO_SNDTIMEO both mean the
 * caller asked for a bounded wait, and BSD returns EWOULDBLOCK for either.
 */
LONG bsd_wait_errno(ULONG wait, UINT status)
{
    BOOL impatient = (wait != NX_WAIT_FOREVER);

    switch (status)
    {
        case NX_WAIT_ABORTED:
            return AMI_EINTR;

        case NX_POOL_DELETED:
            return AMI_ENOBUFS;

        case NX_NO_PACKET:
        case NX_TX_QUEUE_DEPTH:
        case NX_WINDOW_OVERFLOW:
            return impatient ? AMI_EWOULDBLOCK : AMI_ENOBUFS;

        default:
            return bsd_errno_from_nx(status);
    }
}

/* ------------------------------------------------------------ error texts, */

typedef struct
{
    UWORD       code;
    const char *text;
} BsdErrText;

static const BsdErrText bsd_errno_text[] =
{
    {  0,                    "No error"                          },
    { AMI_EPERM,             "Operation not permitted"           },
    { AMI_ENOENT,            "No such file or directory"         },
    { AMI_EINTR,             "Interrupted system call"           },
    { AMI_EIO,               "Input/output error"                },
    { AMI_ENXIO,             "Device not configured"             },
    { AMI_EBADF,             "Bad file descriptor"               },
    { AMI_ENOMEM,            "Cannot allocate memory"            },
    { AMI_EACCES,            "Permission denied"                 },
    { AMI_EFAULT,            "Bad address"                       },
    { AMI_EBUSY,             "Device busy"                       },
    { AMI_EEXIST,            "File exists"                       },
    { AMI_EINVAL,            "Invalid argument"                  },
    { AMI_ENFILE,            "Too many open files in system"     },
    { AMI_EMFILE,            "Too many open files"               },
    { AMI_EPIPE,             "Broken pipe"                       },
    { AMI_EWOULDBLOCK,       "Operation would block"             },
    { AMI_EINPROGRESS,       "Operation now in progress"         },
    { AMI_EALREADY,          "Operation already in progress"     },
    { AMI_ENOTSOCK,          "Socket operation on non-socket"    },
    { AMI_EDESTADDRREQ,      "Destination address required"      },
    { AMI_EMSGSIZE,          "Message too long"                  },
    { AMI_EPROTOTYPE,        "Protocol wrong type for socket"    },
    { AMI_ENOPROTOOPT,       "Protocol not available"            },
    { AMI_EPROTONOSUPPORT,   "Protocol not supported"            },
    { AMI_ESOCKTNOSUPPORT,   "Socket type not supported"         },
    { AMI_EOPNOTSUPP,        "Operation not supported"           },
    { AMI_EPFNOSUPPORT,      "Protocol family not supported"     },
    { AMI_EAFNOSUPPORT,      "Address family not supported"      },
    { AMI_EADDRINUSE,        "Address already in use"            },
    { AMI_EADDRNOTAVAIL,     "Can't assign requested address"    },
    { AMI_ENETDOWN,          "Network is down"                   },
    { AMI_ENETUNREACH,       "Network is unreachable"            },
    { AMI_ENETRESET,         "Network dropped connection"        },
    { AMI_ECONNABORTED,      "Software caused connection abort"  },
    { AMI_ECONNRESET,        "Connection reset by peer"          },
    { AMI_ENOBUFS,           "No buffer space available"         },
    { AMI_EISCONN,           "Socket is already connected"       },
    { AMI_ENOTCONN,          "Socket is not connected"           },
    { AMI_ESHUTDOWN,         "Can't send after socket shutdown"  },
    { AMI_ETOOMANYREFS,      "Too many references"               },
    { AMI_ETIMEDOUT,         "Operation timed out"               },
    { AMI_ECONNREFUSED,      "Connection refused"                },
    { AMI_ENAMETOOLONG,      "File name too long"                },
    { AMI_EHOSTDOWN,         "Host is down"                      },
    { AMI_EHOSTUNREACH,      "No route to host"                  },
    { AMI_ENOSYS,            "Function not implemented"          }
};

static const BsdErrText bsd_herrno_text[] =
{
    { NETDB_SUCCESS,  "Resolver error 0 (no error)"        },
    { HOST_NOT_FOUND, "Unknown host"                       },
    { TRY_AGAIN,      "Host name lookup failure"           },
    { NO_RECOVERY,    "Unknown server error"               },
    { NO_DATA,        "No address associated with name"    }
};

static const char *bsd_text_lookup(const BsdErrText *table, ULONG count,
                                   LONG code, const char *fallback)
{
    ULONG i;

    for (i = 0; i < count; i++)
    {
        if ((LONG)table[i].code == code)
            return table[i].text;
    }

    return fallback;
}

/* ---------------------------------------------------------------- vectors, */

LONG bsd_Errno(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return SocketBase->sb_Errno;
}

VOID bsd_SetErrnoPtr(register APTR errno_ptr __asm("a0"),
                     register LONG size      __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    if (errno_ptr == NULL || (size != 1 && size != 2 && size != 4))
    {
        SocketBase->sb_ErrnoPtr  = NULL;
        SocketBase->sb_ErrnoSize = 0;
        return;
    }

    SocketBase->sb_ErrnoPtr  = errno_ptr;
    SocketBase->sb_ErrnoSize = size;

    /* Publish the current value immediately, as AmiTCP does. */
    bsd_set_errno(SocketBase, SocketBase->sb_Errno);
}

LONG bsd_enosys(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /* Reached through any vector we have not implemented. -1 with ENOSYS is
       survivable. A NULL vector is not. */
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

APTR bsd_enosys_ptr(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /*
     * The same, for the vectors that return a pointer. -1 in d0 is not a
     * failure there: getservbyname(), getprotobyname(), ObtainInterfaceList(),
     * mbuf_get() and the rest are documented to return NULL and callers test
     * for NULL. 0xFFFFFFFF crashes the caller on the first dereference.
     */
    (VOID)bsd_fail(SocketBase, AMI_ENOSYS);

    return NULL;
}

BOOL bsd_enosys_bool(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /*
     * And again for the vectors that return BOOL, where -1 is all-bits-set.
     * `if (ChangeRoadshowData(...))` reads that as TRUE, and the caller
     * concludes the operation succeeded.
     */
    (VOID)bsd_fail(SocketBase, AMI_ENOSYS);

    return FALSE;
}

/* -------------------------------------------------- SocketBaseTagList() ---- */

#define SBT_RO  0       /* query only */
#define SBT_RW  1       /* query and set */

typedef struct
{
    UWORD   code;
    UWORD   access;
    UWORD   offset;     /* into struct AmiSocketBase */
} BsdSimpleTag;

/* Tags that are nothing more than a ULONG-sized field of the opener's base. */
static const BsdSimpleTag bsd_simple_tags[] =
{
    { SBTC_BREAKMASK,    SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_BreakMask)    },
    { SBTC_SIGIOMASK,    SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_SigIOMask)    },
    { SBTC_SIGURGMASK,   SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_SigUrgMask)   },
    { SBTC_SIGEVENTMASK, SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_SigEventMask) },
    { SBTC_ERRNO,        SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_Errno)        },
    { SBTC_HERRNO,       SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_HErrno)       },
    { SBTC_LOGTAGPTR,    SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_LogTag)       },
    { SBTC_LOGSTAT,      SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_LogStat)      },
    { SBTC_LOGFACILITY,  SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_LogFacility)  },
    { SBTC_LOGMASK,      SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_LogMask)      },
    /*
     * Stored and read back, never called. The NDK deprecates it in place,
     * "Link library fd allocation callback; don't use this in new code!", and
     * the same again over FDCB_FREE/ALLOC/CHECK. It is the AmiTCP 3 mechanism
     * by which a link library kept its own descriptor table in step with the
     * socket library's. A call to it means bsd_fd_alloc() and bsd_fd_free()
     * reach into caller code while they hold library state, for a facility
     * whose own header says not to use it. There is no SBTC_HAVE_ tag that
     * covers it, and a refused SET here costs more than it gains (see the note
     * below on what an unserviced tag discards). The decision is therefore
     * recorded here rather than signalled to the caller.
     */
    { SBTC_FDCALLBACK,   SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_FDCallback)   },
    { SBTC_ERROR_HOOK,   SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_ErrorHook)    },
    /* This is per-opener state a caller can set and read back. */
    { SBTC_SIG_ADDRESS_CHANGE_MASK, SBT_RW,
      (UWORD)offsetof(struct AmiSocketBase, sb_SigAddressChangeMask) },

    /*
     * Recorded per opener and never acted on, which is the honest shape for
     * this one. The capability is genuinely FALSE here -- signal masks and
     * timer.device state belong to the task that opened us, so a shared base
     * would deliver another task's completions -- and a GET of a base that
     * never set it reads the FALSE that bsd_child_create() writes.
     *
     * It is not a refusable constant. A SET that this library will not service
     * returns that tag's index and DISCARDS EVERY TAG AFTER IT in the same
     * call (docs/RESEARCH.md 55), and Amiga callers batch these: a program
     * asking to share and linking its errno in one SocketBaseTagList() would
     * lose the errno pointer and run with a stale one for its whole life.
     * Refusing the request costs more than the request does, because nothing
     * here reads the flag to decide anything.
     */
    { SBTC_CAN_SHARE_LIBRARY_BASES, SBT_RW,
      (UWORD)offsetof(struct AmiSocketBase, sb_CanShareBases) }
};

/*
 * Fixed answers, so Roadshow-aware callers can probe us.
 *
 * `settable` marks the ones the autodoc describes as "for querying and
 * changing". A SET on those is accepted when the value asked for is the value
 * already in force, and refused when it is a real change. Configuration code
 * routinely reads a tunable and writes it straight back. A refusal there
 * returns the tag's index and discards every tag after it in the same call,
 * which is how an SBTC_ERRNOPTR in the next slot goes missing. A caller that
 * asks to turn IP forwarding on is still told.
 *
 * The capability tags are query-only in the document and stay so. Nothing can
 * be set about whether an API exists.
 */
typedef struct
{
    UWORD   code;
    UWORD   settable;   /* SBT_RO / SBT_RW */
    ULONG   value;
} BsdConstTag;

/*
 * How many bpf_* capture channels this build has, all that
 * SBTC_NUM_PACKET_FILTER_CHANNELS asks. It is the first thing Roadshow's
 * tcpdump does: main() reads this code by reference and, if it comes back
 * zero, prints `"%s" V%ld.%ld does not support the raw packet access method
 * used by this program.` and exits. Answering 0 while bpf.c's eight vectors
 * are present turned tcpdump away (docs/RESEARCH.md 55).
 */
#ifdef AMINETXDUO_BPF
#  define BSD_PACKET_FILTER_CHANNELS    AMI_BPF_MAX_CHANNELS
#else
#  define BSD_PACKET_FILTER_CHANNELS    0
#endif

static const BsdConstTag bsd_const_tags[] =
{
    { SBTC_NUM_PACKET_FILTER_CHANNELS, SBT_RO, BSD_PACKET_FILTER_CHANNELS },
    /*
     * Signals and timer.device state belong to the task that opened us, so
     * the honest answer to "can this base be shared" is FALSE.
     *
     * SBT_RW rather than SBT_RO, and the value is what makes that safe: the
     * SET arm above accepts a write of the value already held and refuses
     * anything else. So a caller asking to share is told no, which is the
     * point, and a caller passing FALSE -- or reading it -- is not. A refused
     * SET returns that tag's index and DISCARDS EVERY TAG AFTER IT in the
     * same call, which is how an errno link in the next slot goes missing,
     * and Amiga callers batch these into one SocketBaseTagList().
     */

    /*
     * The tunables. SocketBaseTagList() is documented to return the index of
     * the first tag it cannot service and stop there, so one unserviced code
     * in a caller's list discards every tag after it. A client that probes a
     * group of tunables in a single call then gets nothing, and the failure
     * looks like a broken library rather than one unknown tag
     * (docs/RESEARCH.md 55).
     *
     * Every value below is what this stack does. Where the answer is "no", it
     * says no.
     */
    { SBTC_UDP_CHECKSUM,        SBT_RW, TRUE               }, /* always computed */
    { SBTC_IP_FORWARDING,       SBT_RW, FALSE              }, /* we are a host   */
    { SBTC_IP_DEFAULT_TTL,      SBT_RW, NX_IP_TIME_TO_LIVE },
    { SBTC_ICMP_MASK_REPLY,     SBT_RW, FALSE              }, /* not answered    */
    { SBTC_ICMP_SEND_REDIRECTS, SBT_RW, FALSE              }, /* we do not route */
    /* IR_Process (0) / IR_Ignore (1). Echo is answered, so ping works.
       Timestamp is not implemented by NetX Duo, so it is ignored. */
    { SBTC_ICMP_PROCESS_ECHO,   SBT_RW, 0 },
    { SBTC_ICMP_PROCESS_TSTAMP, SBT_RW, 1 },
    /* IDNCS_ASCII. No IDN support here. Host names go on the wire as ASCII. */
    { SBTC_IDN_DEFAULT_CHARACTER_SET, SBT_RW, 0 },
    /*
     * TRUE since routing.c: AddRouteTagList(), DeleteRouteTagList(),
     * GetRouteInfo() and FreeRouteInfo() are the whole routing API the autodoc
     * documents, and ChangeRouteTagList(), which the sfd gives an LVO and no
     * page anywhere, is implemented beside them. A caller that reads TRUE here
     * and calls it gets a route changed, not an ENOSYS.
     */
    { SBTC_HAVE_ROUTING_API,            SBT_RO, TRUE  },
    /*
     * TRUE since interfaces.c: the tag asks whether the interface API is
     * present, and ObtainInterfaceList(), ReleaseInterfaceList(),
     * QueryInterfaceTagList(), ConfigureInterfaceTagList(),
     * AddInterfaceTagList() and RemoveInterface() are. Individual tags this
     * stack cannot honour are refused there, which a caller reads out of
     * errno. FALSE here stops it from asking at all.
     */
    { SBTC_HAVE_INTERFACE_API,          SBT_RO, TRUE  },
    /*
     * TRUE since netmonitor.c: AddNetMonitorHookTagList() and
     * RemoveNetMonitorHook() are what this tag names, and they work for
     * MHT_Connect, MHT_Bind and MHT_Send. The four in-stack types answer the
     * documented EINVAL, which the caller reads out of errno. FALSE here stops
     * it from asking for the three that install.
     */
    { SBTC_HAVE_MONITORING_API,         SBT_RO, TRUE  },
    /*
     * TRUE since netstats.c: GetNetworkStatistics() is all this tag gates.
     * Four of its ten types are refused with EOPNOTSUPP, which the caller
     * reads out of errno. FALSE here stops it from asking about the six that
     * answer.
     */
    { SBTC_HAVE_STATUS_API,             SBT_RO, TRUE  },
    /*
     * TRUE since roadshow.c gained AddDomainNameServer(),
     * RemoveDomainNameServer() and SetDefaultDomainName(). The list and its
     * release were already there, so the DNS management API is complete.
     *
     * This tag gates more than its name says. Every form of Roadshow's
     * ShowNetStatus, interfaces, routes, DNS, IP, ICMP, TCP, UDP, sockets,
     * all ten, refuses outright when it reads FALSE (docs/RESEARCH.md 55).
     */
    { SBTC_HAVE_DNS_API,                SBT_RO, TRUE  },
    { SBTC_IPF_API_VERSION,             SBT_RO, 0     },
    /*
     * TRUE since netdb.c: the tag names setnetent() and getprotoent(), and all
     * nine of set/get/end {net,proto,serv}ent are there over the DEVS:Internet
     * store. This answered FALSE while they worked, the reverse of the
     * SBTC_HAVE_DNS_API bug in docs/RESEARCH.md 55. There we claimed an API we
     * did not have. Here we denied one we do.
     */
    { SBTC_HAVE_LOCAL_DATABASE_API,     SBT_RO, TRUE  },
    { SBTC_HAVE_ADDRESS_CONVERSION_API, SBT_RO, TRUE  },
    { SBTC_HAVE_KERNEL_MEMORY_API,      SBT_RO, FALSE },
    { SBTC_HAVE_SERVER_API,             SBT_RO, FALSE },
    { SBTC_HAVE_ROADSHOWDATA_API,       SBT_RO, FALSE },
    { SBTC_HAVE_GETHOSTADDR_R_API,      SBT_RO, TRUE  }
};

static BOOL bsd_tag_get(struct AmiSocketBase *base, struct TagItem *item,
                        UWORD code, BOOL by_ref);
static BOOL bsd_tag_set(struct AmiSocketBase *base, struct TagItem *item,
                        UWORD code, BOOL by_ref);

static VOID bsd_tag_store(struct TagItem *item, BOOL by_ref, ULONG value)
{
    if (by_ref)
    {
        if (item->ti_Data != 0)
            *(ULONG *)item->ti_Data = value;
    }
    else
    {
        item->ti_Data = value;
    }
}

static ULONG bsd_tag_fetch(struct TagItem *item, BOOL by_ref)
{
    if (by_ref)
        return (item->ti_Data != 0) ? *(ULONG *)item->ti_Data : 0;

    return item->ti_Data;
}

static BOOL bsd_tag_get(struct AmiSocketBase *base, struct TagItem *item,
                        UWORD code, BOOL by_ref)
{
    ULONG i;

    for (i = 0; i < sizeof(bsd_simple_tags) / sizeof(bsd_simple_tags[0]); i++)
    {
        if (bsd_simple_tags[i].code == code)
        {
            bsd_tag_store(item, by_ref,
                          *(ULONG *)((UBYTE *)base + bsd_simple_tags[i].offset));
            return TRUE;
        }
    }

    for (i = 0; i < sizeof(bsd_const_tags) / sizeof(bsd_const_tags[0]); i++)
    {
        if (bsd_const_tags[i].code == code)
        {
            bsd_tag_store(item, by_ref, bsd_const_tags[i].value);
            return TRUE;
        }
    }

    switch (code)
    {
        case SBTC_DTABLESIZE:
            bsd_tag_store(item, by_ref, (ULONG)bsd_table_size(base));
            return TRUE;

        /* The *STRPTR tags are in/out: the caller passes an error number and
           gets a string pointer back through the same slot. */
        case SBTC_ERRNOSTRPTR:
            bsd_tag_store(item, by_ref, (ULONG)bsd_text_lookup(
                bsd_errno_text,
                sizeof(bsd_errno_text) / sizeof(bsd_errno_text[0]),
                (LONG)bsd_tag_fetch(item, by_ref), "Unknown error"));
            return TRUE;

        case SBTC_HERRNOSTRPTR:
            bsd_tag_store(item, by_ref, (ULONG)bsd_text_lookup(
                bsd_herrno_text,
                sizeof(bsd_herrno_text) / sizeof(bsd_herrno_text[0]),
                (LONG)bsd_tag_fetch(item, by_ref), "Unknown resolver error"));
            return TRUE;

        case SBTC_IOERRNOSTRPTR:
            bsd_tag_store(item, by_ref, (ULONG)"Device error");
            return TRUE;

        case SBTC_S2ERRNOSTRPTR:
        case SBTC_S2WERRNOSTRPTR:
            bsd_tag_store(item, by_ref, (ULONG)"SANA-II device error");
            return TRUE;

        /* "the protocol stack's name and version information string", the
           name alone was neither half of that. */
        case SBTC_RELEASESTRPTR:
            bsd_tag_store(item, by_ref, (ULONG)AMINETXDUO_VERSION_LONG);
            return TRUE;

        /* The errno-mirror pointers read back as well as write. The width tags
           are three views of one pointer, so each reports it only when the
           caller's chosen width matches. */
        case SBTC_ERRNOBYTEPTR:
            bsd_tag_store(item, by_ref,
                          (base->sb_ErrnoSize == 1) ? (ULONG)base->sb_ErrnoPtr : 0);
            return TRUE;

        case SBTC_ERRNOWORDPTR:
            bsd_tag_store(item, by_ref,
                          (base->sb_ErrnoSize == 2) ? (ULONG)base->sb_ErrnoPtr : 0);
            return TRUE;

        case SBTC_ERRNOLONGPTR:
            bsd_tag_store(item, by_ref,
                          (base->sb_ErrnoSize == 4) ? (ULONG)base->sb_ErrnoPtr : 0);
            return TRUE;

        case SBTC_HERRNOLONGPTR:
            bsd_tag_store(item, by_ref, (ULONG)base->sb_HErrnoPtr);
            return TRUE;

        /*
         * Total bytes in and out, as an SBQUAD_T the caller supplies.
         * Roadshow's SampleNetSpeed measures throughput from these. Without
         * them it prints "Could not query data throughput statistics" and
         * exits (docs/RESEARCH.md 55).
         *
         * NetX Duo counts in ULONG, so the high word is always zero and the
         * count wraps after 4 GB. That is NetX Duo's counter. A wrap shows up
         * as a sudden drop rather than as garbage.
         *
         * By reference only: a quad does not fit in ti_Data, so a request
         * without SBTF_REF is refused rather than handed half the value.
         */
        case SBTC_GET_BYTES_RECEIVED:
        case SBTC_GET_BYTES_SENT:
        {
            NX_IP *ip = netstack_ip();
            ULONG  sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
            ULONG *quad;

            if (!by_ref || item->ti_Data == 0)
                return FALSE;
            if (ip == NULL)
                return FALSE;
            if (nx_ip_info_get(ip, &sent, &sent_bytes, &received,
                               &received_bytes, NULL, NULL, NULL, NULL,
                               NULL, NULL) != NX_SUCCESS)
                return FALSE;

            quad = (ULONG *)item->ti_Data;
            quad[0] = 0UL;      /* high */
            quad[1] = (code == SBTC_GET_BYTES_RECEIVED) ? received_bytes
                                                        : sent_bytes;
            return TRUE;
        }

        /*
         * All six bits, not just the first. Roadshow's GetNetStatus reports
         * one line per bit and its startup scripts wait on them,
         * "GetNetStatus CHECK=RESOLVER" is the documented way to hold a script
         * until the network is usable. Answering Interfaces alone made four of
         * its six lines wrong (docs/RESEARCH.md 55).
         *
         * Everything here is read from the configuration the stack is running,
         * so the answer changes as interfaces come up and go down. PTP is
         * never set: a point-to-point interface means SLIP or PPP, and this
         * stack does neither.
         */
        case SBTC_SYSTEM_STATUS:
        {
            const AmiConfig *cfg    = netstack_config();
            ULONG            status = 0UL;

            if (netstack_get() != NULL && cfg != NULL)
            {
                UWORD i;

                for (i = 0; i < cfg->interface_count; i++)
                {
                    if (!cfg->interfaces[i].configured)
                        continue;

                    status |= SBSYSSTAT_Interfaces;

                    /*
                     * Every SANA-II interface we can drive is Ethernet, so it
                     * carries broadcast. Keyed on an interface that is up
                     * rather than only configured: a down interface cannot
                     * carry a broadcast.
                     */
                    if (netstack_interface_is_up(i))
                    {
                        status |= SBSYSSTAT_BCast_Interfaces;

                        /* An interface that is up has a route to its own
                           network, whether or not one was added by hand. */
                        status |= SBSYSSTAT_Routes;
                    }
                }

                if (cfg->resolver.nameserver_count > 0)
                    status |= SBSYSSTAT_Resolver;

                /*
                 * The running gateway, not the configured one. A DHCP machine
                 * has default_gateway 0 in AmiConfig and a good default route
                 * that arrived in the lease. A read of the configuration
                 * therefore made GetNetStatus say "the default route is not
                 * configured" on a machine where ShowNetStatus printed
                 * "Default gateway address = 10.0.2.2". routing.c and
                 * netstatus.c ask NetX Duo too.
                 */
                {
                    NX_IP *ip = netstack_ip();
                    ULONG  gateway = 0UL;

                    if (ip != NULL &&
                        nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS &&
                        gateway != 0UL)
                    {
                        status |= (SBSYSSTAT_DefaultRoute | SBSYSSTAT_Routes);
                    }
                }
            }

            bsd_tag_store(item, by_ref, status);
        }
            return TRUE;

        default:
            return FALSE;
    }
}

static BOOL bsd_tag_set(struct AmiSocketBase *base, struct TagItem *item,
                        UWORD code, BOOL by_ref)
{
    ULONG value = bsd_tag_fetch(item, by_ref);
    ULONG i;

    for (i = 0; i < sizeof(bsd_simple_tags) / sizeof(bsd_simple_tags[0]); i++)
    {
        if (bsd_simple_tags[i].code != code)
            continue;
        if (bsd_simple_tags[i].access != SBT_RW)
            return FALSE;

        *(ULONG *)((UBYTE *)base + bsd_simple_tags[i].offset) = value;

        /* Keep the mirrored copies in step. */
        if (code == SBTC_ERRNO)
            bsd_set_errno(base, (LONG)value);
        else if (code == SBTC_HERRNO)
            bsd_set_herrno(base, (LONG)value);

        return TRUE;
    }

    /* A SET of a fixed tunable to the value it already holds. See BsdConstTag:
       the caller changed nothing, so there is nothing to refuse. */
    for (i = 0; i < sizeof(bsd_const_tags) / sizeof(bsd_const_tags[0]); i++)
    {
        if (bsd_const_tags[i].code != code)
            continue;

        return (bsd_const_tags[i].settable == SBT_RW &&
                bsd_const_tags[i].value == value) ? TRUE : FALSE;
    }

    switch (code)
    {
        case SBTC_DTABLESIZE:
            return (bsd_table_resize(base, (LONG)value) == 0);

        /* errno mirroring. The caller picks the width, which is why there are
           three tags rather than one. */
        case SBTC_ERRNOBYTEPTR:
            base->sb_ErrnoPtr  = (APTR)value;
            base->sb_ErrnoSize = (value != 0) ? 1 : 0;
            bsd_set_errno(base, base->sb_Errno);
            return TRUE;

        case SBTC_ERRNOWORDPTR:
            base->sb_ErrnoPtr  = (APTR)value;
            base->sb_ErrnoSize = (value != 0) ? 2 : 0;
            bsd_set_errno(base, base->sb_Errno);
            return TRUE;

        case SBTC_ERRNOLONGPTR:
            base->sb_ErrnoPtr  = (APTR)value;
            base->sb_ErrnoSize = (value != 0) ? 4 : 0;
            bsd_set_errno(base, base->sb_Errno);
            return TRUE;

        case SBTC_HERRNOLONGPTR:
            base->sb_HErrnoPtr = (LONG *)value;
            bsd_set_herrno(base, base->sb_HErrno);
            return TRUE;

        default:
            return FALSE;
    }
}

/*
 * NextTagItem() for the Roadshow tag-list vectors, declared in
 * bsdsocket_internal.h. bsd_SocketBaseTagList() keeps its own inline walk
 * because it has to report the 1-based index of the offending tag, which a
 * shared walker cannot.
 */
struct TagItem *bsd_next_tag(struct TagItem **cursor)
{
    struct TagItem *item = *cursor;

    while (item != NULL)
    {
        switch (item->ti_Tag)
        {
            case TAG_DONE:
                *cursor = NULL;
                return NULL;

            case TAG_IGNORE:
                item++;
                continue;

            case TAG_MORE:
                item = (struct TagItem *)item->ti_Data;
                continue;

            case TAG_SKIP:
                item += 1 + (LONG)item->ti_Data;
                continue;

            default:
                *cursor = item + 1;
                return item;
        }
    }

    *cursor = NULL;
    return NULL;
}

LONG bsd_SocketBaseTagList(register struct TagItem *tags __asm("a0"),
                           register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct TagItem *item  = tags;
    LONG            index = 0;

    if (tags == NULL)
        return 0;

    while (item != NULL)
    {
        Tag  tag = item->ti_Tag;
        UWORD code;
        BOOL by_ref, is_set, ok;

        /* Hand-rolled NextTagItem(): utility.library is not open here. */
        if (tag == TAG_DONE)
            break;

        if (tag == TAG_IGNORE)
        {
            item++;
            index++;
            continue;
        }

        if (tag == TAG_MORE)
        {
            item = (struct TagItem *)item->ti_Data;
            continue;
        }

        if (tag == TAG_SKIP)
        {
            item += 1 + (LONG)item->ti_Data;
            index += 1 + (LONG)item->ti_Data;
            continue;
        }

        index++;

        code   = (UWORD)SBTM_CODE(tag);
        by_ref = ((tag & SBTF_REF) != 0);
        is_set = ((tag & SBTF_SET) != 0);

        /* SBTF_REF makes ti_Data a pointer to the value.  A null pointer is
           an invalid tag, not a zero value or a successful no-op. */
        if (by_ref && item->ti_Data == 0)
            return index;

        ok = is_set ? bsd_tag_set(SocketBase, item, code, by_ref)
                    : bsd_tag_get(SocketBase, item, code, by_ref);

        if (!ok)
            return index;       /* 1-based index of the offending tag */

        item++;
    }

    return 0;
}
