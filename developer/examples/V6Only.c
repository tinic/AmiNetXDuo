/*
 * V6Only -- the AF_INET6 names the NDK does not define, used.
 *
 * Makes an IPv6 socket, reads and sets IPV6_V6ONLY, prints the address it is
 * bound to, and classifies it.  None of IPPROTO_IPV6, IPV6_V6ONLY,
 * INET6_ADDRSTRLEN, IN6ADDR_LOOPBACK_INIT, IN6_IS_ADDR_* or
 * struct sockaddr_storage is in the NDK; all of them come from
 * <aminetxduo/in6.h>, which <proto/aminetxduo.h> brings in.
 *
 * Compiled against the staged Developer drawer alone, as the drawer's own
 * check that it ships everything it names.  The build target is
 * tests/tools/CMakeLists.txt's test_v6only.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>

/* The NDK's <sys/socket.h> uses size_t and ssize_t without declaring them. */
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <proto/aminetxduo.h>

struct Library *SocketBase;

int main(void)
{
    static const struct in6_addr loopback = IN6ADDR_LOOPBACK_INIT;

    struct sockaddr_storage  ss;
    struct sockaddr_in6      sin6;
    char                     text[INET6_ADDRSTRLEN];
    socklen_t                len;
    LONG                     sock, on;
    int                      rc = RETURN_OK;

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL) {
        Printf((CONST_STRPTR)"V6Only: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    /*
     * Whether this build has IPv6 is not a revision question -- nothing was
     * added to the vector table for it -- so the socket call is the test.
     */
    sock = socket(PF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        Printf((CONST_STRPTR)"V6Only: no AF_INET6 in this build\n");
        CloseLibrary(SocketBase);
        return RETURN_WARN;
    }

    on  = 1;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0) {
        Printf((CONST_STRPTR)"V6Only: IPV6_V6ONLY refused\n");
        rc = RETURN_ERROR;
    }

    /*
     * sin6_family goes at offset 0 and there is no sin6_len to fill in: on
     * this NDK that byte is the family.  aminetxduo/in6.h has the warning.
     */
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = 0;
    sin6.sin6_addr   = loopback;

    if (bind(sock, (struct sockaddr *)&sin6, sizeof(sin6)) < 0) {
        Printf((CONST_STRPTR)"V6Only: bind to ::1 failed\n");
        rc = RETURN_ERROR;
    }

    /* sockaddr_storage is somewhere to receive either family. */
    len = (socklen_t)sizeof(ss);
    if (getsockname(sock, (struct sockaddr *)&ss, &len) < 0) {
        Printf((CONST_STRPTR)"V6Only: getsockname failed\n");
        rc = RETURN_ERROR;
    } else if (len == (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *bound = (const struct sockaddr_in6 *)&ss;

        if (inet_ntop(AF_INET6, (APTR)&bound->sin6_addr,
                      (STRPTR)text, sizeof(text)) != NULL)
            Printf((CONST_STRPTR)"bound to %s port %ld\n",
                   (LONG)text, (LONG)bound->sin6_port);

        Printf((CONST_STRPTR)"loopback %s, multicast %s, v4mapped %s\n",
               (LONG)(IN6_IS_ADDR_LOOPBACK(&bound->sin6_addr) ? "yes" : "no"),
               (LONG)(IN6_IS_ADDR_MULTICAST(&bound->sin6_addr) ? "yes" : "no"),
               (LONG)(IN6_IS_ADDR_V4MAPPED(&bound->sin6_addr) ? "yes" : "no"));

        if (!IN6_IS_ADDR_LOOPBACK(&bound->sin6_addr)) {
            Printf((CONST_STRPTR)"V6Only: ::1 did not read back as loopback\n");
            rc = RETURN_ERROR;
        }
    } else {
        Printf((CONST_STRPTR)"V6Only: getsockname returned %ld bytes, "
                             "not a sockaddr_in6\n", (LONG)len);
        rc = RETURN_ERROR;
    }

    CloseSocket(sock);
    CloseLibrary(SocketBase);
    return rc;
}
