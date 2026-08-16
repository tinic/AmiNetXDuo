/* <net/if_dl.h> for the bsdsocket host tests.  The link-layer sockaddr a
   routing message carries.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_NET_IF_DL_H
#define AMINETXDUO_BSD_TEST_NET_IF_DL_H
#include <exec/types.h>
struct sockaddr_dl {
    UBYTE sdl_len;
    UBYTE sdl_family;
    UWORD sdl_index;
    UBYTE sdl_type;
    UBYTE sdl_nlen;
    UBYTE sdl_alen;
    UBYTE sdl_slen;
    char  sdl_data[12];
};
#define LLADDR(s) ((char *)((s)->sdl_data + (s)->sdl_nlen))
/* sdl_family's value.  The NDK puts it in <sys/socket.h>; the host's has
   AF_PACKET at a different number and no AF_LINK at all, so it belongs with
   the structure that is the only thing here using it. */
#ifndef AF_LINK
#define AF_LINK   18
#endif
#endif
