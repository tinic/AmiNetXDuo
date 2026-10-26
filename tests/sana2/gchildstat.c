/* Read-only counters for the #353 bounded child-list diagnostic. */
#include <exec/types.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <stdio.h>

int main(void)
{
    struct MsgPort *port;
    ULONG snapshot[6];
    ULONG clock[5];
    BOOL found;
    BOOL clock_found;

    Forbid();
    port = FindPort((CONST_STRPTR)"AmiNetXDuo gchild");
    found = (port != NULL);
    if (found)
        CopyMem(port->mp_SigTask, snapshot, sizeof(snapshot));
    port = FindPort((CONST_STRPTR)"AmiNetXDuo eclock");
    clock_found = (port != NULL);
    if (clock_found)
        CopyMem(port->mp_SigTask, clock, sizeof(clock));
    Permit();
    if (!found)
    {
        printf("gchildstat=none\n");
        return 5;
    }
    printf("gchildstat checks=%lu yes=%lu capped=%lu port_marker_skips=%lu "
           "recent_skips=%lu max_visits=%lu\n", (unsigned long)snapshot[0],
           (unsigned long)snapshot[1], (unsigned long)snapshot[2],
           (unsigned long)snapshot[3], (unsigned long)snapshot[4],
           (unsigned long)snapshot[5]);
    if (clock_found)
        printf("gclockstat hz=%lu budget=%lu refill=%lu failures=%lu disabled=%lu\n",
               (unsigned long)clock[0], (unsigned long)clock[1],
               (unsigned long)clock[2], (unsigned long)clock[3],
               (unsigned long)clock[4]);
    else
        printf("gclockstat=none\n");
    return 0;
}
