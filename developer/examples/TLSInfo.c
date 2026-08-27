/*
 * TLSInfo, tls.library called the way every other AmigaOS library is called.
 *
 * Connects to a host, hands the descriptor to tls.library, prints what was
 * negotiated -- protocol version, cipher suite, whether the chain verified,
 * and the RFC 7301 application protocol if the server chose one -- and closes.
 *
 * There is no GCC extended assembly here and no library base in any argument
 * list.  <proto/tls.h> resolves TLSOpenTags() and the rest through the global
 * TLSBase below, from developer/sfd/tls_lib.sfd, which is the description
 * SAS/C and vbcc read too.  This file exists to prove that: it is compiled
 * against the staged Developer drawer and the NDK and NOTHING ELSE
 * (tests/tools/CMakeLists.txt, test_tlsinfo), so a prototype or a type that
 * never reaches Developer/ fails the build.
 *
 *   TLSInfo [HOST] [PORT]        default www.example.com 443
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>

/* The NDK's <sys/socket.h> uses size_t and ssize_t without declaring them. */
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <proto/tls.h>

struct Library *SocketBase;
struct Library *TLSBase;

static LONG connect_to(const char *host, UWORD port)
{
    struct sockaddr_in  sa;
    struct hostent     *he;
    LONG                sock;

    he = gethostbyname((UBYTE *)host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
    {
        printf("%s: no address\n", host);
        return -1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        printf("socket() failed\n");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = port;
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));

    if (connect(sock, (struct sockaddr *)&sa, (LONG)sizeof(sa)) != 0)
    {
        printf("connect(%s) failed\n", host);
        CloseSocket(sock);
        return -1;
    }

    return sock;
}

int main(int argc, char **argv)
{
    struct TLSConnection *tls;
    struct TLSInfo        info;
    const char           *host = (argc > 1) ? argv[1] : "www.example.com";
    UWORD                 port = (UWORD)((argc > 2) ? atoi(argv[2]) : 443);
    char                  alpn[TLS_ALPN_NAME_MAX + 1];
    LONG                  error = 0;
    LONG                  sock;
    LONG                  n;

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4UL);
    if (SocketBase == NULL)
    {
        printf("no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    /* Version 3 because TLSGetALPN() is a version 3 vector.  Asking for the
       version you use is the rule: Exec opens on lib_Version >= what you ask
       for, so asking for less and calling more jumps past the jump table. */
    TLSBase = OpenLibrary((STRPTR)TLS_LIB_NAME, 3UL);
    if (TLSBase == NULL)
    {
        printf("no %s version 3\n", TLS_LIB_NAME);
        CloseLibrary(SocketBase);
        return RETURN_FAIL;
    }

    sock = connect_to(host, port);
    if (sock < 0)
    {
        CloseLibrary(TLSBase);
        CloseLibrary(SocketBase);
        return RETURN_FAIL;
    }

    tls = TLSOpenTags((APTR)SocketBase, sock,
                      TLSA_HostName, (ULONG)host,
                      TLSA_Error,    (ULONG)&error,
                      TLSA_ALPN,     (ULONG)"h2,http/1.1",
                      TAG_END);
    if (tls == NULL)
    {
        printf("TLSOpen(%s): %s\n", host, TLSErrorString(error));
        CloseSocket(sock);
        CloseLibrary(TLSBase);
        CloseLibrary(SocketBase);
        return RETURN_FAIL;
    }

    memset(&info, 0, sizeof(info));
    info.ti_Size = (ULONG)sizeof(info);

    if (TLSInfo(tls, &info) == 0)
    {
        printf("host      %s:%lu\n", host, (unsigned long)port);
        printf("protocol  0x%04lx\n", (unsigned long)info.ti_Version);
        printf("suite     0x%04lx\n", (unsigned long)info.ti_CipherSuite);
        printf("verified  %s\n", info.ti_Verified ? "yes" : "no");
        printf("resumed   %s\n", info.ti_Resumed ? "yes" : "no");
        printf("handshake %lu ms\n", (unsigned long)info.ti_HandshakeMillis);
    }

    n = TLSGetALPN(tls, alpn, (LONG)sizeof(alpn));
    printf("alpn      %s\n", (n > 0) ? alpn : "(none negotiated)");

    TLSClose(tls);
    CloseSocket(sock);
    CloseLibrary(TLSBase);
    CloseLibrary(SocketBase);

    return RETURN_OK;
}
