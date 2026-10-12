/* Prepare an isolated Copperline guest, then run the raw SANA-II test.
 * SPDX-License-Identifier: MIT
 */
#include <dos/dostags.h>
#include <proto/dos.h>

int main(void)
{
    BPTR lock = Lock((CONST_STRPTR)"DH0:devs", SHARED_LOCK);
    if (lock == 0)
        return 20;
    if (!AssignLock((CONST_STRPTR)"DEVS", lock))
    {
        UnLock(lock);
        return 20;
    }
    /* This raw-device test needs DEVS only and starts no TCP/IP stack. */
    return (int)SystemTags((STRPTR)"DH0:netmulticast_test >DH0:stdout.txt",
                          SYS_Asynch, FALSE, TAG_DONE);
}
