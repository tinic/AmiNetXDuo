/* Standalone emulator smoke; no networking or ThreadX kernel selected.
 * SPDX-License-Identifier: MIT */
#include "exec_wait.h"
#include <stdio.h>

int main(void)
{
    AnxExecWait e;
    uint32_t token;
    unsigned passed = 0;
    if (!anx_exec_wait_open(&e)) {
        puts("research_exec_wait=FAIL resource setup");
        return 20;
    }
    token = anx_wait_begin(&e.wait, 20000, 0, 0, 0, 0);
    if (token && !anx_exec_wait_close(&e) &&
        anx_exec_wait_run(&e, token) == ANX_WAIT_TIMEOUT)
        passed++;
    token = anx_wait_begin(&e.wait, ANX_WAIT_FOREVER, 0, 0, 0, 0);
    if (token && anx_wait_complete(&e.wait, token, ANX_WAIT_READY) &&
        anx_exec_wait_run(&e, token) == ANX_WAIT_READY)
        passed++;
    token = anx_wait_begin(&e.wait, 20000, 0, 0, 0, 0);
    if (token && anx_wait_complete(&e.wait, token, ANX_WAIT_CANCELLED) &&
        anx_exec_wait_run(&e, token) == ANX_WAIT_CANCELLED)
        passed++;
    if (anx_exec_wait_close(&e))
        passed++;
    printf("research_exec_wait=%s checks=%u/4\n", passed == 4 ? "PASS" : "FAIL", passed);
    return passed == 4 ? 0 : 20;
}
