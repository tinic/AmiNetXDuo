#include <stdint.h>
#include <stddef.h>
typedef void VOID;
typedef void *APTR;
typedef int BOOL;
typedef unsigned UINT;
typedef uint32_t ULONG;
typedef uint8_t UBYTE;
#define TRUE 1
#define FALSE 0
#define TX_TRUE 1U
#define TX_FALSE 0U
#define TX_TIMER_TICKS_PER_SECOND 50UL
struct Node { struct Node *ln_Succ; int8_t ln_Pri; };
struct MinNode { struct MinNode *mln_Succ; };
struct Task { struct Node tc_Node; APTR tc_UserData; };
struct AmiSocketBase {
    ULONG pad;
    struct MinNode sb_Node;
    struct { struct MinNode *mlh_Head; } sb_Children;
    struct Task *sb_Task;
    ULONG sb_LastNetCall;
    struct Task *sb_LastNetTask;
};
struct HostExecBase { struct { struct Node *lh_Head; } TaskReady; };
extern struct HostExecBase *SysBase;
extern ULONG bsd_reader_victim_stats[6];
VOID Disable(VOID);
VOID Enable(VOID);
ULONG tx_time_get(VOID);
UINT tx_amiga_task_may_be_internal(VOID *task);
BOOL bsd_reader_victim_ready(struct AmiSocketBase **master_slot);
