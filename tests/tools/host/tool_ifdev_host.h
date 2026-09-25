#ifndef AMINETXDUO_TOOL_IFDEV_HOST_H
#define AMINETXDUO_TOOL_IFDEV_HOST_H

/* tools.h without the Amiga half: tool_ifdev.c needs the netstatus records,
   one bound and three calls. */
#include <exec/types.h>
#include "aminetxduo/netstatus.h"

#define NX_MAX_PHYSICAL_INTERFACES 4

struct Library;

LONG tool_netstatus_query(struct Library *base, ULONG what,
                          APTR buffer, ULONG size, ULONG entry_size);
VOID tool_copy_string(char *dst, ULONG dstlen, const char *src);
int  tool_stricmp(const char *a, const char *b);
int  tool_stricmp_n(const char *a, const char *b, ULONG n);

VOID tool_netstatus_devices(struct Library *base);
BOOL tool_if_device(char *dst, ULONG dstlen, const NetStatusInterface *e);
BOOL tool_device_matches(const char *file, const char *live, BOOL whole);

#endif
