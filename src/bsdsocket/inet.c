/*
 * bsdsocket.library -- the inet_* address conversions.
 *
 * Self-contained: no libc, no NetX Duo. m68k is big-endian, so the network
 * byte order these functions traffic in is also the host byte order, and the
 * BSD_HTONL/BSD_NTOHL macros are documentation rather than code.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#ifdef AMINETXDUO_IPV6
/*
 * The IPv6 text conversions are src/config/config_text.c's -- the same
 * routines the DEVS:NetInterfaces parser uses for ADDRESS6. One parser means
 * the config file and inet_pton()/inet_ntop() cannot disagree about what an
 * address is.
 *
 * The dialect difference -- the config file takes a "/prefixlen" suffix and
 * inet_pton() must not -- is expressed by passing NULL for the prefix output.
 */
#include "aminetxduo/config.h"
#endif

#ifndef INADDR_NONE
#  define INADDR_NONE   0xffffffffUL
#endif

#define BSD_IN_CLASSA(i)    (((ULONG)(i) & 0x80000000UL) == 0)
#define BSD_IN_CLASSB(i)    (((ULONG)(i) & 0xc0000000UL) == 0x80000000UL)
#define BSD_IN_CLASSC(i)    (((ULONG)(i) & 0xe0000000UL) == 0xc0000000UL)

/*
 * The 4.3BSD parser: up to four dot-separated parts, each decimal, octal
 * (leading 0) or hexadecimal (leading 0x), with the classic "a", "a.b",
 * "a.b.c" short forms. Returns FALSE on anything malformed.
 */
static BOOL bsd_inet_parse(const char *cp, ULONG *result, LONG *nparts,
                           ULONG parts_out[4])
{
    ULONG parts[4];
    LONG  n = 0;
    LONG  i;

    if (cp == NULL)
        return FALSE;

    for (;;)
    {
        ULONG value = 0;
        ULONG base  = 10;
        LONG  digits = 0;

        if (*cp == '0')
        {
            cp++;
            digits++;
            if (*cp == 'x' || *cp == 'X')
            {
                cp++;
                base = 16;
                digits = 0;
            }
            else
            {
                base = 8;
            }
        }

        for (;;)
        {
            UBYTE c = (UBYTE)*cp;
            ULONG digit;

            if (c >= '0' && c <= '9')
                digit = (ULONG)(c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f')
                digit = (ULONG)(c - 'a') + 10;
            else if (base == 16 && c >= 'A' && c <= 'F')
                digit = (ULONG)(c - 'A') + 10;
            else
                break;

            if (digit >= base)
                break;

            value = value * base + digit;
            digits++;
            cp++;
        }

        if (digits == 0)
            return FALSE;

        if (n >= 4)
            return FALSE;

        parts[n++] = value;

        if (*cp != '.')
            break;

        cp++;
    }

    if (*cp != '\0')
        return FALSE;

    /* Trailing part absorbs the remaining bytes; the leading ones are bytes. */
    switch (n)
    {
        case 1:
            *result = parts[0];
            break;

        case 2:
            if (parts[0] > 0xff || parts[1] > 0xffffff)
                return FALSE;
            *result = (parts[0] << 24) | parts[1];
            break;

        case 3:
            if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffff)
                return FALSE;
            *result = (parts[0] << 24) | (parts[1] << 16) | parts[2];
            break;

        case 4:
            if (parts[0] > 0xff || parts[1] > 0xff ||
                parts[2] > 0xff || parts[3] > 0xff)
                return FALSE;
            *result = (parts[0] << 24) | (parts[1] << 16) |
                      (parts[2] << 8)  |  parts[3];
            break;

        default:
            return FALSE;
    }

    if (nparts != NULL)
        *nparts = n;

    if (parts_out != NULL)
    {
        for (i = 0; i < n; i++)
            parts_out[i] = parts[i];
    }

    return TRUE;
}

/* Unsigned decimal, no libc. Returns the number of characters written. */
static ULONG bsd_format_u8(char *dst, ULONG value)
{
    char  tmp[3];
    ULONG n = 0, i;

    do
    {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && n < sizeof(tmp));

    for (i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];

    return n;
}

static ULONG bsd_format_ip(char *dst, ULONG addr)
{
    ULONG len = 0;
    LONG  i;

    for (i = 3; i >= 0; i--)
    {
        len += bsd_format_u8(dst + len, (addr >> (i * 8)) & 0xff);
        if (i > 0)
            dst[len++] = '.';
    }

    dst[len] = '\0';

    return len;
}

/* ---------------------------------------------------------------- vectors -- */

in_addr_t bsd_inet_addr(register STRPTR cp __asm("a0"),
                        register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr;

    (VOID)SocketBase;

    if (!bsd_inet_parse((const char *)cp, &addr, NULL, NULL))
        return (in_addr_t)INADDR_NONE;

    return (in_addr_t)BSD_HTONL(addr);
}

LONG bsd_inet_aton(register STRPTR cp             __asm("a0"),
                   register struct in_addr *addr  __asm("a1"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG value;

    (VOID)SocketBase;

    if (!bsd_inet_parse((const char *)cp, &value, NULL, NULL))
        return 0;

    if (addr != NULL)
        addr->s_addr = (in_addr_t)BSD_HTONL(value);

    return 1;
}

in_addr_t bsd_inet_network(register STRPTR cp __asm("a0"),
                           register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr;
    ULONG parts[4];
    ULONG net = 0;
    LONG  n = 0;
    LONG  i;

    (VOID)SocketBase;

    if (!bsd_inet_parse((const char *)cp, &addr, &n, parts))
        return (in_addr_t)INADDR_NONE;

    /*
     * 4.3BSD: each part is one byte of the network number, packed left to
     * right and right-aligned in the result -- "192.168.1" is 0x00C0A801, not
     * an address with a zero host part. Same rule as
     * ami_cfg_parse_net_number(), which reads DEVS:Internet/networks.
     */
    for (i = 0; i < n; i++)
        net = (net << 8) | (parts[i] & 0xff);

    return (in_addr_t)net;
}

STRPTR bsd_Inet_NtoA(register in_addr_t ip __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    bsd_format_ip(SocketBase->sb_NtoABuf, BSD_NTOHL(ip));

    return (STRPTR)SocketBase->sb_NtoABuf;
}

in_addr_t bsd_Inet_LnaOf(register in_addr_t in __asm("d0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr = BSD_NTOHL(in);

    (VOID)SocketBase;

    if (BSD_IN_CLASSA(addr))
        return (in_addr_t)(addr & 0x00ffffffUL);

    if (BSD_IN_CLASSB(addr))
        return (in_addr_t)(addr & 0x0000ffffUL);

    return (in_addr_t)(addr & 0x000000ffUL);
}

in_addr_t bsd_Inet_NetOf(register in_addr_t in __asm("d0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr = BSD_NTOHL(in);

    (VOID)SocketBase;

    if (BSD_IN_CLASSA(addr))
        return (in_addr_t)((addr & 0xff000000UL) >> 24);

    if (BSD_IN_CLASSB(addr))
        return (in_addr_t)((addr & 0xffff0000UL) >> 16);

    return (in_addr_t)((addr & 0xffffff00UL) >> 8);
}

in_addr_t bsd_Inet_MakeAddr(register in_addr_t net  __asm("d0"),
                            register in_addr_t host __asm("d1"),
                            register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr;

    (VOID)SocketBase;

    if (net < 128)
        addr = (((ULONG)net) << 24) | (((ULONG)host) & 0x00ffffffUL);
    else if (net < 65536)
        addr = (((ULONG)net) << 16) | (((ULONG)host) & 0x0000ffffUL);
    else if (net < 0x01000000UL)
        addr = (((ULONG)net) <<  8) | (((ULONG)host) & 0x000000ffUL);
    else
        addr = ((ULONG)net) | ((ULONG)host);    /* net is already a full address */

    return (in_addr_t)BSD_HTONL(addr);
}

STRPTR bsd_inet_ntop(register LONG af      __asm("d0"),
                     register APTR src     __asm("a0"),
                     register STRPTR dst   __asm("a1"),
                     register LONG size    __asm("d1"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    char  scratch[BSD_NTOA_BUFLEN];
    ULONG len;

    if (src == NULL || dst == NULL)
    {
        bsd_set_errno(SocketBase, AMI_EFAULT);
        return NULL;
    }

#ifdef AMINETXDUO_IPV6
    if (af == AF_INET6)
    {
        char  text[AMI_INET6_ADDRSTRLEN];
        ULONG words[4];
        ULONG n;

        bsd_in6_to_words((const UBYTE *)src, words);
        ami_config_format_ip6(words, text, sizeof(text));

        for (n = 0; text[n] != '\0'; n++)
            ;

        if (size <= (LONG)n)
        {
            bsd_set_errno(SocketBase, AMI_ENOSPC);
            return NULL;
        }

        bsd_bcopy(text, dst, n + 1);

        return dst;
    }
#endif

    if (af != AF_INET)
    {
        bsd_set_errno(SocketBase, AMI_EAFNOSUPPORT);
        return NULL;
    }

    len = bsd_format_ip(scratch, BSD_NTOHL(((struct in_addr *)src)->s_addr));

    if (size <= (LONG)len)
    {
        bsd_set_errno(SocketBase, AMI_ENOSPC);
        return NULL;
    }

    bsd_bcopy(scratch, dst, len + 1);

    return dst;
}

LONG bsd_inet_pton(register LONG af    __asm("d0"),
                   register STRPTR src __asm("a0"),
                   register APTR dst   __asm("a1"),
                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG addr;
    LONG  parts = 0;

#ifdef AMINETXDUO_IPV6
    if (af == AF_INET6)
    {
        ULONG words[4];

        if (src == NULL || dst == NULL)
        {
            bsd_set_errno(SocketBase, AMI_EFAULT);
            return -1;
        }

        /* NULL prefix output == strict mode: "fe80::1/64" is not an address.
           Per POSIX, a malformed address returns 0; -1 is reserved for an
           unknown family. */
        if (!ami_config_parse_ip6((const char *)src, words, NULL))
            return 0;

        bsd_words_to_in6(words, (UBYTE *)dst);

        return 1;
    }
#endif

    if (af != AF_INET)
    {
        bsd_set_errno(SocketBase, AMI_EAFNOSUPPORT);
        return -1;
    }

    if (src == NULL || dst == NULL)
    {
        bsd_set_errno(SocketBase, AMI_EFAULT);
        return -1;
    }

    /* Unlike inet_addr(), inet_pton() accepts only strict dotted quads. */
    if (!bsd_inet_parse((const char *)src, &addr, &parts, NULL) || parts != 4)
        return 0;

    ((struct in_addr *)dst)->s_addr = (in_addr_t)BSD_HTONL(addr);

    return 1;
}
