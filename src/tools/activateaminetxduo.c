/*
 * ActivateAmiNetXDuo, put a self-contained installation first in LIBS:, C:
 * and DEVS: without discarding anything already assigned there.
 *
 * A plain `Assign ... ADD` cannot select a TCP/IP stack: AssignAdd() appends,
 * so an existing LIBS:bsdsocket.library wins.  Replacing LIBS: with the
 * private drawer and then adding SYS:Libs back loses every other member of a
 * user's multi-assign (SYS:Classes on a stock Workbench, among others).  This
 * command duplicates the complete old lists, replaces each head with the
 * AmiNetXDuo drawer, and appends the old members in their original order.
 *
 * Run only at boot, before any bsdsocket.library has opened.  A resident
 * library cannot be switched by changing a DOS assign, so the command refuses
 * that state instead of claiming a switch it did not perform.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/execbase.h>
#include <exec/memory.h>

const char *const tool_name = "ActivateAmiNetXDuo";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ActivateAmiNetXDuo");

static VOID release_locks(BPTR *locks, ULONG first, ULONG count)
{
    ULONG i;

    if (locks == NULL)
        return;

    for (i = first; i < count; i++)
    {
        if (locks[i] != 0)
            UnLock(locks[i]);
    }
    FreeVec(locks);
}

static BOOL prepend_assign(const char *name, const char *path)
{
    struct DosList    *list;
    struct DosList    *entry;
    struct AssignList *member;
    BPTR              *saved = NULL;
    BPTR               front;
    ULONG              capacity = 0;
    ULONG              count = 0;
    ULONG              i;
    LONG               error;
    BOOL               failed = FALSE;

    front = Lock((STRPTR)path, SHARED_LOCK);
    if (front == 0)
    {
        tool_error("cannot lock %s", (LONG)path);
        tool_fault(IoErr());
        return FALSE;
    }

    list = LockDosList(LDF_ASSIGNS | LDF_READ);
    entry = FindDosEntry(list, (STRPTR)name, LDF_ASSIGNS);

    if (entry != NULL && entry->dol_Type != DLT_DIRECTORY)
    {
        UnLockDosList(LDF_ASSIGNS | LDF_READ);
        UnLock(front);
        tool_error("%s: is not a directory assign", (LONG)name);
        return FALSE;
    }

    if (entry != NULL)
    {
        capacity = 1;
        for (member = entry->dol_misc.dol_assign.dol_List;
             member != NULL; member = member->al_Next)
            capacity++;

        saved = (BPTR *)AllocVec(capacity * sizeof(*saved),
                                 MEMF_PUBLIC | MEMF_CLEAR);
        if (saved == NULL)
        {
            UnLockDosList(LDF_ASSIGNS | LDF_READ);
            UnLock(front);
            tool_error("out of memory preserving %s:", (LONG)name);
            return FALSE;
        }

        if (entry->dol_Lock != 0 &&
            SameLock(entry->dol_Lock, front) != LOCK_SAME)
        {
            saved[count] = DupLock(entry->dol_Lock);
            if (saved[count] == 0)
                failed = TRUE;
            else
                count++;
        }

        for (member = entry->dol_misc.dol_assign.dol_List;
             !failed && member != NULL; member = member->al_Next)
        {
            if (member->al_Lock == 0 ||
                SameLock(member->al_Lock, front) == LOCK_SAME)
                continue;

            saved[count] = DupLock(member->al_Lock);
            if (saved[count] == 0)
                failed = TRUE;
            else
                count++;
        }
    }

    UnLockDosList(LDF_ASSIGNS | LDF_READ);

    if (failed)
    {
        error = IoErr();
        UnLock(front);
        release_locks(saved, 0, count);
        tool_error("cannot preserve every member of %s:", (LONG)name);
        tool_fault(error);
        return FALSE;
    }

    if (AssignLock((STRPTR)name, front) == DOSFALSE)
    {
        error = IoErr();
        UnLock(front);
        release_locks(saved, 0, count);
        tool_error("cannot select %s for %s:", (LONG)path, (LONG)name);
        tool_fault(error);
        return FALSE;
    }

    /* AssignLock owns front now.  AssignAdd owns each saved lock on success. */
    for (i = 0; i < count; i++)
    {
        if (AssignAdd((STRPTR)name, saved[i]) == DOSFALSE)
        {
            error = IoErr();
            release_locks(saved, i, count);
            tool_error("cannot restore every old member of %s:", (LONG)name);
            tool_fault(error);
            return FALSE;
        }
        saved[i] = 0;
    }

    if (saved != NULL)
        FreeVec(saved);
    return TRUE;
}

int main(int argc, char **argv)
{
    struct Node *loaded;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    Forbid();
    loaded = FindName(&SysBase->LibList, (STRPTR)"bsdsocket.library");
    Permit();

    if (loaded != NULL)
    {
        tool_error("bsdsocket.library is already in memory");
        tool_hint("change the startup selection and reboot to switch stacks");
        return RETURN_FAIL;
    }

    if (!prepend_assign("LIBS", "AmiNetXDuo:Libs"))
        return RETURN_FAIL;
    if (!prepend_assign("C", "AmiNetXDuo:C"))
        return RETURN_FAIL;
    if (!prepend_assign("DEVS", "AmiNetXDuo:Devs"))
        return RETURN_FAIL;

    return RETURN_OK;
}
