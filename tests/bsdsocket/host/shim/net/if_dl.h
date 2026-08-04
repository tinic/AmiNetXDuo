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
#endif
