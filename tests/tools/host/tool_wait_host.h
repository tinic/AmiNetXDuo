#ifndef AMINETXDUO_TOOL_WAIT_HOST_H
#define AMINETXDUO_TOOL_WAIT_HOST_H

#include <stdint.h>

typedef int      BOOL;
typedef void     VOID;
typedef int32_t  LONG;
typedef uint16_t UWORD;
typedef uint32_t ULONG;

#define FALSE 0
#define TRUE  1

#define TICKS_PER_SECOND       50
#define NETSTATUS_DHCP_OFF      0
#define NETSTATUS_DHCP_WORKING  1
#define NETSTATUS_DHCP_BOUND    2

struct Library;

typedef struct ToolWait
{
    ULONG limit;
    ULONG elapsed;
    BOOL  broken;
} ToolWait;

BOOL tool_delay_ticks(ULONG ticks);
LONG tool_netstatus_dhcp_state(struct Library *base, UWORD index,
                               ULONG *addr_out);

VOID tool_wait_init(ToolWait *wait, ULONG seconds);
BOOL tool_wait_second(ToolWait *wait);
BOOL tool_wait_dhcp_bound(struct Library *base, UWORD index, ToolWait *wait,
                          ULONG *addr_out);

#endif
