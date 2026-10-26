/* Read-only counters for the #353 bounded child-list diagnostic. */
#include <exec/types.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <stdio.h>

int main(void)
{
    struct MsgPort *port;
    ULONG snapshot[6];
    BOOL found;

    Forbid();
    port = FindPort((CONST_STRPTR)"AmiNetXDuo gchild");
    found = (port != NULL);
    if (found)
        CopyMem(port->mp_SigTask, snapshot, sizeof(snapshot));
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
    return 0;
}
