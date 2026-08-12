/* Harness smoke test: proves amiberry-run.sh boots, logs to serial, can write a
   result file back to the host, and that ENV:/T: assigns exist.
   SPDX-License-Identifier: MIT */
#include <proto/exec.h>
#include <proto/dos.h>
#include "aminetxduo/compat.h"

static LONG checks, failures;

static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok) failures++;
    Printf((STRPTR)"  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

int main(void)
{
    BPTR fh;
    ULONG t0, t1;
    char buf[64];

    AMI_ERROR("aminetxduo smoke: serial output alive");

    t0 = ami_millis();
    Delay(25);                      /* 0.5s */
    t1 = ami_millis();
    check("ami_millis tracks Delay(25)", (t1 - t0) >= 300 && (t1 - t0) <= 800);

    /* ENV:, this is what the bare Startup-Sequence used to lack. */
    check("SetVar ENV:", SetVar((STRPTR)"AmiNetXDuoProbe", (STRPTR)"hello", -1,
                                GVF_GLOBAL_ONLY | LV_VAR) != 0);
    buf[0] = '\0';
    check("GetVar reads it back",
          GetVar((STRPTR)"AmiNetXDuoProbe", (STRPTR)buf, sizeof(buf),
                 GVF_GLOBAL_ONLY | LV_VAR) > 0 &&
          buf[0] == 'h' && buf[4] == 'o');

    fh = Open((STRPTR)"ENV:AmiNetXDuoProbe", MODE_OLDFILE);
    check("ENV: is a real assign", fh != 0);
    if (fh) Close(fh);

    /* T:, scratch space. */
    fh = Open((STRPTR)"T:probe.tmp", MODE_NEWFILE);
    check("T: is writable", fh != 0);
    if (fh) { FPuts(fh, (STRPTR)"t\n"); Close(fh); }

    fh = Open((STRPTR)"DH0:smoke.txt", MODE_NEWFILE);
    if (fh) { FPuts(fh, (STRPTR)(failures ? "smoke FAILED\n" : "smoke ok\n")); Close(fh); }

    Printf((STRPTR)"\n%ld checks, %ld failure(s)\n", checks, failures);
    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}
