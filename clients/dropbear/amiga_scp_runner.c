/* Private process host for the Amiga SCP transport.
 *
 * The code must live outside scp's SegList: after it signals completion,
 * dos.library still has a short Process-deactivation tail to execute.  Keeping
 * that tail in this helper lets the invoking Shell unload scp immediately
 * without invalidating instructions the helper can still return through.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/tasks.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "amiga_stdio.h"

#define SCP_SSH_STACK (256UL * 1024UL)

int main(void)
{
    struct Task *task = FindTask(NULL);
    struct amiga_stdio_override *stdio;
    ULONG tagged;
    LONG rc = RETURN_FAIL;

    if (task == NULL)
        return (int)rc;
    tagged = (ULONG)task->tc_UserData;
    if ((tagged & 3UL) != AMIGA_STDIO_USERDATA_TAG)
        return (int)rc;
    stdio = (struct amiga_stdio_override *)(tagged & ~3UL);
    if (stdio->magic != AMIGA_STDIO_MAGIC)
        return (int)rc;

    rc = RunCommand(stdio->command_segment, SCP_SSH_STACK,
                    stdio->arguments, stdio->arguments_length);

    amiga_mempipe_close_reader(stdio->input);
    amiga_mempipe_close_writer(stdio->output);
    *stdio->exit_rc = rc;
    *stdio->exit_done = 1;
    Signal(stdio->exit_task, stdio->exit_signal);
    return (int)rc;
}
