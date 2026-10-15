/* NetSwitch's banner, rendered once for the screen it opens on. */
#ifndef AMINETXDUO_NETSWITCH_BANNER_H
#define AMINETXDUO_NETSWITCH_BANNER_H

#include <exec/types.h>
#include <graphics/gfx.h>
#include <intuition/screens.h>

#define NS_BANNER_PENS 16

typedef struct NsBanner
{
    struct BitMap *bm;              /* NULL: no banner */
    WORD           w, h;
    UBYTE          npens;           /* pens obtained, released by free */
    UBYTE          pens[NS_BANNER_PENS];
} NsBanner;

/* Load `file` (a 1..8-plane ILBM) and render it for `s`: RGB on a cybergraphics
   screen deeper than 8 bits, else dithered to the screen's pens.  FALSE leaves
   *b empty, which is also what an unsupported screen gets. */
BOOL ns_banner_load(struct Screen *s, const char *file, NsBanner *b);
VOID ns_banner_free(struct Screen *s, NsBanner *b);

#endif
