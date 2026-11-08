/*
 * CreateAmiNetXDuoStatusReport's task list: ShowTasks' own snapshot
 * (third_party/shelltools, task_snapshot.cpp), printed as task.<n>.* rows.
 *
 * The report carries it because "run ShowTasks" is not a request a user can
 * follow on every machine: WarpOS ships a ShowTasks of its own, for the
 * PowerPC tasks, and the Shell finds that one first.
 *
 * SPDX-License-Identifier: MIT
 */

#include "task_snapshot.hpp"
#include "statusreport_text.h"

#include <exec/nodes.h>

namespace {
// 96 rows, as ShowTasks; static, the Shell gives a command 4 KB of stack.
shelltools::TaskSnapshot sr_snapshot;
} // namespace

extern "C" VOID sr_tasks(SrOut *o) {
    shelltools::capture_tasks(sr_snapshot);
    sr_ulong(o, "task.count", sr_snapshot.count);
    sr_yesno(o, "task.truncated", sr_snapshot.truncated ? TRUE : FALSE);
    for (unsigned i = 0; i < sr_snapshot.count; ++i) {
        const shelltools::TaskRow &row = sr_snapshot.rows[i];
        SrTask t;
        t.address = row.address;
        t.cli = row.cli;
        t.priority = row.priority;
        t.state = shelltools::task_state(row.state);
        t.process = row.type == NT_PROCESS ? TRUE : FALSE;
        t.flags = row.flags;
        t.sig_alloc = row.signal_alloc;
        t.sig_wait = row.signal_wait;
        t.sig_recvd = row.signal_received;
        t.stack_lower = row.stack_lower;
        t.stack_upper = row.stack_upper;
        t.stack_pointer = row.stack_pointer;
        t.process_stack = row.process_stack_size;
        t.process_flags = static_cast<ULONG>(row.process_flags);
        t.name = row.name.data();
        t.command = row.command.data();
        sr_task(o, i, &t);
    }
}
