#include "reader_victim_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct HostExecBase exec_base;
struct HostExecBase *SysBase = &exec_base;
static struct AmiSocketBase master, children[90], *root;
static struct Task tasks[30];
static struct Node tail;
static struct MinNode child_tail;
static ULONG now;
static unsigned depth, checks, failures, clear_root;

VOID Disable(VOID) { if (depth++) abort(); if (clear_root) root = NULL; }
VOID Enable(VOID) { if (depth != 1) abort(); depth--; }
ULONG tx_time_get(VOID) { if (!depth) abort(); return now; }
static void check(BOOL ok, const char *what) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", what); }
}
static void ready(unsigned n) {
    for (unsigned i = 0; i < n; i++)
        tasks[i].tc_Node.ln_Succ = i + 1 < n ? &tasks[i+1].tc_Node : &tail;
    SysBase->TaskReady.lh_Head = n ? &tasks[0].tc_Node : &tail;
}
static void openers(unsigned n) {
    for (unsigned i = 0; i < n; i++)
        children[i].sb_Node.mln_Succ = i + 1 < n ? &children[i+1].sb_Node : &child_tail;
    master.sb_Children.mlh_Head = n ? &children[0].sb_Node : &child_tail;
}
static void reset(void) {
    memset(&master, 0, sizeof(master));
    memset(children, 0, sizeof(children));
    memset(tasks, 0, sizeof(tasks));
    memset(bsd_reader_victim_stats, 0, sizeof(ULONG) * 6);
    root = &master; now = 100; clear_root = depth = 0;
    ready(0); openers(0);
}
static BOOL victim(void) {
    BOOL result = bsd_reader_victim_ready(&root);
    check(depth == 0, "critical section balanced");
    check(bsd_reader_victim_stats[5] <= 64, "shared visit ceiling");
    return result;
}
int main(void) {
    reset(); root = NULL; check(!victim(), "absent master is unknown");
    reset(); check(!victim(), "empty ready list");
    reset(); ready(1); tasks[0].tc_UserData = &tasks[0];
    check(!victim(), "internal priority-zero task excluded");
    reset(); ready(1); openers(1); children[0].sb_LastNetTask = &tasks[0];
    children[0].sb_LastNetCall = now;
    check(!victim(), "recent caller excluded");
    children[0].sb_Task=&tasks[1];
    check(!victim(), "actual caller protected despite different opener");
    ready(2); check(victim(), "caller does not hide later decoder");
    reset(); ready(3); tasks[0].tc_UserData = &tasks[0]; openers(1);
    children[0].sb_LastNetTask = &tasks[1]; children[0].sb_LastNetCall = now;
    check(victim(), "internal caller decoder ordering");
    reset(); ready(10); for (unsigned i=0;i<9;i++) tasks[i].tc_Node.ln_Pri=1;
    check(!victim() && bsd_reader_victim_stats[2]==1, "above-zero cap is unknown");
    reset(); ready(1); openers(1); children[0].sb_LastNetTask = &tasks[0];
    children[0].sb_LastNetCall = now - 10;
    check(!victim(), "window boundary remains recent");
    now++; check(victim(), "expired caller eligible");
    reset(); ready(1); openers(1); children[0].sb_LastNetTask = &tasks[0];
    now=3; children[0].sb_LastNetCall=UINT32_MAX-1;
    check(!victim(), "clock wrap keeps recent caller");
    now=20; check(victim(), "clock wrap expires old caller");
    reset(); ready(1); tasks[0].tc_Node.ln_Pri=-1;
    check(!victim(), "priority-minus-one is not helped");
    reset(); ready(1); openers(90);
    check(!victim() && bsd_reader_victim_stats[2]==1 && bsd_reader_victim_stats[5]==64,
          "child-list work shares ceiling; capped miss is unknown");
    reset(); ready(1); openers(2); children[0].sb_LastNetTask=children[1].sb_LastNetTask=&tasks[0];
    children[0].sb_LastNetCall=0; children[1].sb_LastNetCall=now;
    check(!victim(), "any recent base protects multi-open caller");
    reset(); ready(1); openers(1);
    struct Task *gone=calloc(1,sizeof(*gone)); if(!gone)return 2;
    children[0].sb_LastNetTask=gone; children[0].sb_LastNetCall=now; free(gone);
    check(victim(), "stored dead task address never dereferenced");
    reset(); ready(1); clear_root=1;
    check(!victim(), "root loaded after Disable, not before");
    reset(); ready(17); for(unsigned i=0;i<17;i++)tasks[i].tc_UserData=&tasks[i];
    check(!victim() && bsd_reader_victim_stats[2]==1, "zero-priority cap is unknown");
    struct Task *plain=calloc(1,sizeof(*plain)); if(!plain)return 2;
    plain->tc_UserData=plain;
    check(tx_amiga_task_may_be_internal(plain), "self-userdata uses no control block past plain Task");
    plain->tc_UserData=NULL;
    check(!tx_amiga_task_may_be_internal(plain), "ordinary external Task");
    check(!tx_amiga_task_may_be_internal(NULL), "null Task marker");
    free(plain);
    printf("reader-victim checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
