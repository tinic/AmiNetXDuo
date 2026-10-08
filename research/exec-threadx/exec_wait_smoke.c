/* Standalone emulator smoke; no networking or ThreadX kernel selected.
 * SPDX-License-Identifier: MIT */
#include "exec_wait.h"
#include <proto/dos.h>

static void say(const char *text)
{
    const char *end = text;
    while (*end)
        end++;
    (void)Write(Output(), (APTR)text, (LONG)(end - text));
    (void)Flush(Output());
}

int main(void)
{
    AnxExecWait e;
    uint32_t token;
    unsigned passed = 0;
    say("research_exec_wait=START\n");
    if (!anx_exec_wait_open(&e)) {
        say("research_exec_wait=FAIL resource setup\n");
        return 20;
    }
    say("research_exec_wait=OPEN\n");
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
    say(passed == 4 ? "research_exec_wait=PASS checks=4/4\n"
                    : "research_exec_wait=FAIL incomplete checks\n");
    return passed == 4 ? 0 : 20;
}
