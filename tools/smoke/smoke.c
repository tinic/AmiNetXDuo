/* Harness smoke test: proves fsuae-run.sh boots, logs to serial and can write
   a result file back to the host. SPDX-License-Identifier: MIT */
#include <proto/exec.h>
#include <proto/dos.h>
#include "aminetxduo/compat.h"

int main(void)
{
    BPTR fh;
    ULONG t0, t1;

    AMI_ERROR("aminetxduo smoke: serial output alive");
    AMI_INFO("exec version %ld", (LONG)SysBase->LibNode.lib_Version);

    t0 = ami_millis();
    Delay(25);                      /* 0.5s */
    t1 = ami_millis();
    AMI_INFO("ami_millis delta over Delay(25) = %ld ms", (LONG)(t1 - t0));

    fh = Open("DH0:smoke.txt", MODE_NEWFILE);
    if (fh)
    {
        FPuts(fh, "smoke ok\n");
        Close(fh);
    }

    return (t1 - t0) >= 300 && (t1 - t0) <= 800 ? 0 : 1;
}
