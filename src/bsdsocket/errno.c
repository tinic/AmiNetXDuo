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

/*
 * SBTC_ERROR_HOOK. "Install or remove a hook which is called whenever the
 * global 'errno' or 'h_errno' variables are changed. The hook is always called
 * on the context of the caller." The register shape is the autodoc's:
 * error = hookfunc(hook, reserved, ehm) in A0, A2, A1, reserved NULL.
 *
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
    { SBTC_FDCALLBACK,   SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_FDCallback)   },
    { SBTC_ERROR_HOOK,   SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_ErrorHook)    },
    { SBTC_SIG_ADDRESS_CHANGE_MASK, SBT_RW,
      (UWORD)offsetof(struct AmiSocketBase, sb_SigAddressChangeMask) },

    /*
     * Recorded per opener and never acted on, which is the honest shape for
     * this one. The capability is genuinely FALSE here -- signal masks and
     * timer.device state belong to the task that opened us, so a shared base
     * would deliver another task's completions -- and a GET of a base that
     * never set it reads the FALSE that bsd_child_create() writes.
     */
    { SBTC_CAN_SHARE_LIBRARY_BASES, SBT_RW,
      (UWORD)offsetof(struct AmiSocketBase, sb_CanShareBases) }
};

/*
 * Fixed answers, so Roadshow-aware callers can probe us.
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
     */

    /*
     * The tunables. SocketBaseTagList() is documented to return the index of
     * the first tag it cannot service and stop there, so one unserviced code
     * in a caller's list discards every tag after it. A client that probes a
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
    { SBTC_HAVE_ROUTING_API,            SBT_RO, TRUE  },
    { SBTC_HAVE_INTERFACE_API,          SBT_RO, TRUE  },
    { SBTC_HAVE_MONITORING_API,         SBT_RO, TRUE  },
    { SBTC_HAVE_STATUS_API,             SBT_RO, TRUE  },
    { SBTC_HAVE_DNS_API,                SBT_RO, TRUE  },
    { SBTC_IPF_API_VERSION,             SBT_RO, 0     },
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
         */
        case SBTC_GET_BYTES_RECEIVED:
        case SBTC_GET_BYTES_SENT:
        {
            NX_IP *ip;
            ULONG  sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
            ULONG *quad;
            UINT   nx_status = NX_NOT_ENABLED;

            if (!by_ref || item->ti_Data == 0)
                return FALSE;

            /* SocketBaseTagList() has no vector-level ThreadX bracket. */
            if (bsd_nx_enter(base) != 0)
                return FALSE;

            ip = netstack_ip();
            if (ip != NULL)
                nx_status = nx_ip_info_get(
                    ip, &sent, &sent_bytes, &received, &received_bytes,
                    NULL, NULL, NULL, NULL, NULL, NULL);

            bsd_nx_leave(base);

            if (nx_status != NX_SUCCESS)
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
         */
        case SBTC_SYSTEM_STATUS:
        {
            ULONG status = 0UL;

            /* SBSYSSTAT_Resolver comes off cfg->resolver, which a lease or a
               router advertisement changes from a NetX task that cannot apply
               it.  Outside the bracket below: this takes one of its own. */
            netstack_dns_absorb_pending();

            /* The interface flags are live NetX fields and the gateway getter
               takes nx_ip_protection.  Snapshot both while this application
               task is adopted; formatting the tag itself needs no bracket. */
            if (bsd_nx_enter(base) == 0)
            {
                const AmiConfig *cfg = netstack_config();

                if (netstack_get() != NULL && cfg != NULL)
                {
                    UWORD i;

                    for (i = 0; i < cfg->interface_count &&
                                i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
                    {
                        if (!cfg->interfaces[i].configured)
                            continue;

                        status |= SBSYSSTAT_Interfaces;

                        if (netstack_interface_is_up(i))
                        {
                            status |= SBSYSSTAT_BCast_Interfaces;

                            status |= SBSYSSTAT_Routes;
                        }
                    }

                    if (cfg->resolver.nameserver_count > 0)
                        status |= SBSYSSTAT_Resolver;

                    {
                        NX_IP *ip = netstack_ip();
                        ULONG  gateway = 0UL;

                        if (ip != NULL &&
                            nx_ip_gateway_address_get(ip, &gateway)
                                == NX_SUCCESS &&
                            gateway != 0UL)
                        {
                            status |= (SBSYSSTAT_DefaultRoute |
                                       SBSYSSTAT_Routes);
                        }
                    }
                }

                bsd_nx_leave(base);
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
