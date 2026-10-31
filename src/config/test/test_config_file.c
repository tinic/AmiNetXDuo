/*
 * AmiNetXDuo, host test of config_file.c's DEVS: redirect.
 *
 * Which NetInterfaces drawer every tool and the stack read, for each layout
 * an installation can leave behind: a system install, a drawer install, a
 * drawer install whose interface files went to the system's DEVS:, and a
 * system install over the Devs a removed drawer install left in its
 * documentation drawer.  config_file.c is compiled as it ships, over the
 * volume model in test/fileshim/proto/dos.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../config_internal.h"
#include "aminetxduo/compat.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;

APTR ami_alloc(ULONG size)
{
    void *p = (size != 0) ? calloc(1, size) : NULL;

    if (p != NULL)
        stub_outstanding++;
    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;
    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID)
{
    return stub_outstanding;
}

__attribute__((format(printf, 2, 3)))
VOID ami_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

SHIM_DOS_DEFINE_STATE;

struct Task *shim_current_task;
int          shim_forbid_depth;
int          shim_semaphore_depth;
ULONG        shim_signals;

const char *shim_vol_obj[SHIM_VOL_MAX];
int         shim_vol_dir[SHIM_VOL_MAX];
int         shim_vol_count;
const char *shim_assign_anxd;
const char *shim_assign_devs[SHIM_DEVS_MAX];
int         shim_assign_devs_count;
int         shim_vol_locks;
int         shim_vol_unwindowed_locks;
struct DevProc shim_devproc;
int            shim_devproc_member;
int            shim_devproc_out;

/* ---------------------------------------------------------------- harness */

static int failures;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));         \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static struct Process me;

static void reset(void)
{
    shim_dos_reset();
    memset(shim_vol_obj, 0, sizeof(shim_vol_obj));
    memset(shim_vol_dir, 0, sizeof(shim_vol_dir));
    memset(shim_assign_devs, 0, sizeof(shim_assign_devs));
    shim_vol_count            = 0;
    shim_assign_anxd          = NULL;
    shim_assign_devs_count    = 0;
    shim_vol_locks            = 0;
    shim_vol_unwindowed_locks = 0;
    shim_devproc_out          = 0;

    memset(&me, 0, sizeof(me));
    me.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    me.pr_WindowPtr = (APTR)0x1234;
    shim_current_task = &me.pr_Task;

    /* Every layout has the system's DEVS:. */
    shim_vol_add("SYS:", 1);
    shim_vol_add("SYS:Devs", 1);
    shim_assign_devs[shim_assign_devs_count++] = "SYS:Devs";
}

/* The stock system install: genet and wifipi in SYS:Devs/NetInterfaces. */
static void system_files(void)
{
    shim_vol_add("SYS:Devs/NetInterfaces", 1);
    shim_vol_add("SYS:Devs/NetInterfaces/genet", 0);
    shim_vol_add("SYS:Devs/NetInterfaces/wifipi", 0);
}

/* SYS:AmiNetXDuo, with or without a Devs/NetInterfaces/prism2 in it. */
static void drawer(int with_netinterfaces)
{
    shim_vol_add("SYS:AmiNetXDuo", 1);
    shim_vol_add("SYS:AmiNetXDuo/Devs", 1);
    shim_vol_add("SYS:AmiNetXDuo/Devs/Networks", 1);
    if (with_netinterfaces)
    {
        shim_vol_add("SYS:AmiNetXDuo/Devs/NetInterfaces", 1);
        shim_vol_add("SYS:AmiNetXDuo/Devs/NetInterfaces/prism2", 0);
    }
    shim_assign_anxd = "SYS:AmiNetXDuo";
}

/* What a drawer install's S:User-Startup adds: Assign DEVS: ... ADD. */
static void drawer_in_devs(void)
{
    shim_assign_devs[shim_assign_devs_count++] = "SYS:AmiNetXDuo/Devs";
}

static const char *resolve(const char *path)
{
    static char buf[AMI_CFG_PATH_LEN];

    return ami_cfg_resolve(path, buf, sizeof(buf));
}

static char seen[256];

static VOID sink(AmiConfig *cfg, const char *name)
{
    (void)cfg;
    strncat(seen, name, sizeof(seen) - strlen(seen) - 2);
    strcat(seen, " ");
}

static const char *scan(void)
{
    static AmiConfig cfg;

    seen[0] = '\0';
    CHECK(ami_cfg_scan_interfaces(&cfg, sink), "the drawer is scanned");
    return seen;
}

static void balanced(const char *what)
{
    CHECK(shim_vol_locks == 0, what);
    CHECK(shim_devproc_out == 0, what);
    CHECK(stub_outstanding == 0, what);
    CHECK(me.pr_WindowPtr == (APTR)0x1234, what);
}

/* ------------------------------------------------------------------ tests */

static void test_system_layout(void)
{
    reset();
    system_files();

    CHECK(strcmp(resolve("DEVS:NetInterfaces"), "DEVS:NetInterfaces") == 0,
          "system: DEVS:NetInterfaces as given");
    CHECK(strcmp(scan(), "genet wifipi ") == 0, "system: genet and wifipi");
    balanced("system: locks, DevProcs, buffers and window pointer restored");
}

static void test_drawer_layout(void)
{
    reset();
    drawer(1);
    drawer_in_devs();

    CHECK(strcmp(resolve("DEVS:NetInterfaces"),
                 "AmiNetXDuo:Devs/NetInterfaces") == 0,
          "drawer: the drawer's NetInterfaces");
    CHECK(strcmp(resolve("devs:NetInterfaces/prism2"),
                 "AmiNetXDuo:Devs/NetInterfaces/prism2") == 0,
          "drawer: a file in it, any case of DEVS:");
    CHECK(strcmp(scan(), "prism2 ") == 0, "drawer: prism2");
    CHECK(shim_vol_unwindowed_locks > 0, "drawer: probed with no requester");
    balanced("drawer: locks, DevProcs, buffers and window pointer restored");
}

/* A drawer install over a system whose interface files are in SYS:Devs:
   the drawer has no NetInterfaces, so the system's is read. */
static void test_drawer_without_netinterfaces(void)
{
    reset();
    system_files();
    drawer(0);
    drawer_in_devs();

    CHECK(strcmp(resolve("DEVS:NetInterfaces"), "DEVS:NetInterfaces") == 0,
          "drawer, no NetInterfaces: the system's");
    CHECK(strcmp(resolve("DEVS:Networks"), "AmiNetXDuo:Devs/Networks") == 0,
          "drawer, no NetInterfaces: its Networks still wins");
    CHECK(strcmp(scan(), "genet wifipi ") == 0,
          "drawer, no NetInterfaces: genet and wifipi");
    balanced("drawer, no NetInterfaces: balanced");
}

/* The A1200, 1.0.0-rc2: a system install whose AmiNetXDuo: is the
   documentation drawer, holding the prism2 a removed drawer install kept. */
static void test_stale_drawer(void)
{
    reset();
    system_files();
    drawer(1);

    CHECK(strcmp(resolve("DEVS:NetInterfaces"), "DEVS:NetInterfaces") == 0,
          "stale drawer: the system's NetInterfaces");
    CHECK(strcmp(resolve("DEVS:NetInterfaces/genet"),
                 "DEVS:NetInterfaces/genet") == 0,
          "stale drawer: genet as given");
    CHECK(strcmp(resolve("DEVS:NetInterfaces/prism2"),
                 "DEVS:NetInterfaces/prism2") == 0,
          "stale drawer: its prism2 is not read");
    CHECK(strcmp(scan(), "genet wifipi ") == 0,
          "stale drawer: genet and wifipi, no prism2");
    balanced("stale drawer: balanced");
}

/* Only the drawer has interface files: it is read. */
static void test_drawer_only(void)
{
    reset();
    drawer(1);
    drawer_in_devs();

    CHECK(strcmp(resolve("DEVS:NetInterfaces/prism2"),
                 "AmiNetXDuo:Devs/NetInterfaces/prism2") == 0,
          "drawer only: prism2 in the drawer");
    CHECK(strcmp(scan(), "prism2 ") == 0, "drawer only: prism2");
    balanced("drawer only: balanced");
}

/* A task may not Lock(): nothing is probed, the name is used as given. */
static void test_task_caller(void)
{
    reset();
    drawer(1);
    drawer_in_devs();
    me.pr_Task.tc_Node.ln_Type = NT_TASK;

    CHECK(strcmp(resolve("DEVS:NetInterfaces"), "DEVS:NetInterfaces") == 0,
          "task: no redirect");
    CHECK(shim_vol_unwindowed_locks == 0 && shim_vol_locks == 0,
          "task: no Lock()");
}

static void test_not_devs(void)
{
    reset();
    drawer(1);
    drawer_in_devs();

    CHECK(strcmp(resolve("ENV:HOSTNAME"), "ENV:HOSTNAME") == 0,
          "ENV: is never redirected");
    CHECK(shim_vol_unwindowed_locks == 0, "ENV: probes nothing");
}

int main(void)
{
    test_system_layout();
    test_drawer_layout();
    test_drawer_without_netinterfaces();
    test_stale_drawer();
    test_drawer_only();
    test_task_caller();
    test_not_devs();

    if (failures != 0)
    {
        printf("config_file: %d failure(s)\n", failures);
        return 1;
    }
    printf("config_file: all passed\n");
    return 0;
}
