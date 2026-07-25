/*
 * bsdsocket.library -- errno, h_errno and SocketBaseTagList().
 *
 * errno is per-opener and is optionally mirrored into a caller-supplied
 * variable whose *width the caller chooses* (1, 2 or 4 bytes), which is why
 * this cannot be built on NetX Duo's own errno handling (docs/RESEARCH.md
 * S6.4).
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>
#include <stddef.h>

/* ------------------------------------------------------------------ errno -- */

VOID bsd_set_errno(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;

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

    if (base->sb_HErrnoPtr != NULL)
        *base->sb_HErrnoPtr = code;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    bsd_set_errno(base, code);
    return -1;
}

/*
 * NetX Duo status -> BSD errno. Only the codes our call sites can actually
 * produce are listed; anything else lands on EIO, which is the honest answer
 * for "the stack refused and we have no better word for it".
 */
typedef struct
{
    UWORD   status;
    UWORD   code;
} BsdStatusMap;

static const BsdStatusMap bsd_status_map[] =
{
    { NX_SUCCESS,           0                    },
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
    { NX_NO_INTERFACE_ADDRESS, AMI_EADDRNOTAVAIL }
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

/* ------------------------------------------------------------ error texts -- */

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

/* ---------------------------------------------------------------- vectors -- */

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
    /*
     * Reached through any vector we have not implemented. Returning -1 with
     * ENOSYS is survivable; a NULL vector is not.
     */
    return bsd_fail(SocketBase, AMI_ENOSYS);
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
    { SBTC_FDCALLBACK,   SBT_RW, (UWORD)offsetof(struct AmiSocketBase, sb_FDCallback)   }
};

/* Read-only capability answers, so Roadshow-aware callers can probe us. */
typedef struct
{
    UWORD   code;
    ULONG   value;
} BsdConstTag;

static const BsdConstTag bsd_const_tags[] =
{
    { SBTC_NUM_PACKET_FILTER_CHANNELS,  0     },
    { SBTC_HAVE_ROUTING_API,            FALSE },
    { SBTC_HAVE_INTERFACE_API,          FALSE },
    { SBTC_HAVE_MONITORING_API,         FALSE },
    { SBTC_CAN_SHARE_LIBRARY_BASES,     FALSE },
    { SBTC_HAVE_STATUS_API,             FALSE },
    { SBTC_HAVE_DNS_API,                FALSE },
    { SBTC_IPF_API_VERSION,             0     },
    { SBTC_HAVE_LOCAL_DATABASE_API,     FALSE },
    { SBTC_HAVE_ADDRESS_CONVERSION_API, TRUE  },
    { SBTC_HAVE_KERNEL_MEMORY_API,      FALSE },
    { SBTC_HAVE_SERVER_API,             FALSE },
    { SBTC_HAVE_ROADSHOWDATA_API,       FALSE },
    { SBTC_HAVE_GETHOSTADDR_R_API,      TRUE  }
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
            bsd_tag_store(item, by_ref, (ULONG)base->sb_TableSize);
            return TRUE;

        /*
         * The *STRPTR tags are in/out: the caller passes an error number and
         * gets a string pointer back through the same slot.
         */
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
            bsd_tag_store(item, by_ref, (ULONG)"AmiNetXDuo");
            return TRUE;

        case SBTC_SYSTEM_STATUS:
            bsd_tag_store(item, by_ref,
                          (netstack_get() != NULL) ? SBSYSSTAT_Interfaces : 0);
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

        /* Keep the mirrored copies honest. */
        if (code == SBTC_ERRNO)
            bsd_set_errno(base, (LONG)value);
        else if (code == SBTC_HERRNO)
            bsd_set_herrno(base, (LONG)value);

        return TRUE;
    }

    switch (code)
    {
        case SBTC_DTABLESIZE:
            return (bsd_table_resize(base, (LONG)value) == 0);

        /*
         * errno mirroring. The caller picks the width, which is the whole
         * point of having three tags instead of one.
         */
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

        ok = is_set ? bsd_tag_set(SocketBase, item, code, by_ref)
                    : bsd_tag_get(SocketBase, item, code, by_ref);

        if (!ok)
            return index;       /* 1-based index of the offending tag */

        item++;
    }

    return 0;
}
