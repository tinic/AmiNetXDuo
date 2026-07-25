/*
 * AmiNetXDuo -- usergroup.library: passwords, terminal input, utmp, lastlog.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_vectors.h"

#include "aminetxduo/compat.h"

#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* ------------------------------------------------------------- crypt ----- */

/*
 * We do not ship a DES implementation, and there is nothing here for one to
 * verify against: the built-in database has no passwords, and a passwd file
 * on an AmigaOS volume is readable by anyone anyway.
 *
 * So crypt() fails, but it fails the safe way -- it returns a valid, stable,
 * never-NULL string that cannot match any stored hash. Callers that do the
 * usual strcmp(crypt(typed, salt), pw->pw_passwd) get "no match" instead of a
 * NULL dereference, and callers that check ug_GetErr() see ENOSYS.
 */
UBYTE *ugl_crypt(UG_A6, register UBYTE *key __asm("a0"),
                        register UBYTE *set __asm("a1"))
{
    (void)key;
    (void)set;

    base->ug_PassBuf[0] = '*';
    base->ug_PassBuf[1] = '\0';

    ug_set_err(base, UG_ENOSYS);

    return (UBYTE *)base->ug_PassBuf;
}

/*
 * The salt of an existing entry is the first two characters of its hash;
 * "**" stands in when there is no hash to take it from, which keeps the
 * result usable as a crypt() argument either way.
 */
UBYTE *ugl_GetSalt(UG_A6, register struct ug_passwd *user __asm("a0"),
                          register UBYTE *buf __asm("a1"),
                          register ULONG size __asm("d0"))
{
    char *out = (char *)buf;
    const char *hash;

    if (out == NULL || size == 0)
    {
        ug_set_err(base, UG_EFAULT);
        return NULL;
    }

    hash = (user != NULL && user->pw_passwd != NULL) ? user->pw_passwd : "";

    if (size < 3)
    {
        out[0] = '\0';
        ug_set_err(base, UG_ERANGE);
        return buf;
    }

    if (hash[0] != '\0' && hash[1] != '\0')
    {
        out[0] = hash[0];
        out[1] = hash[1];
    }
    else
    {
        out[0] = '*';
        out[1] = '*';
    }
    out[2] = '\0';

    ug_set_err(base, 0);

    return buf;
}

/* ------------------------------------------------------------ getpass --- */

/*
 * Read a line from the console with echo suppressed. Never returns NULL: with
 * no console (a non-Process caller, or no dos.library) the answer is the empty
 * string and ug_GetErr() explains why.
 */
STRPTR ugl_getpass(UG_A6, register STRPTR prompt __asm("a1"))
{
    struct DosLibrary *dos = ug_dos(base);
    struct Process *self;
    BPTR  fh;
    LONG  length = 0;
    BOOL  raw = FALSE;

    base->ug_PassBuf[0] = '\0';

    if (dos == NULL)
    {
        ug_set_err(base, UG_ENOSYS);
        return (STRPTR)base->ug_PassBuf;
    }

    self = (struct Process *)FindTask(NULL);
    if (self->pr_Task.tc_Node.ln_Type != NT_PROCESS)
    {
        ug_set_err(base, UG_ENOTTY);
        return (STRPTR)base->ug_PassBuf;
    }

    fh = Open((STRPTR)"CONSOLE:", MODE_OLDFILE);
    if (fh == 0)
    {
        ug_set_err(base, UG_ENOTTY);
        return (STRPTR)base->ug_PassBuf;
    }

    if (prompt != NULL)
        Write(fh, prompt, (LONG)ug_strlen((const char *)prompt));

    /* Raw mode: one character at a time, and the console stops echoing. */
    raw = (BOOL)(SetMode(fh, 1) != DOSFALSE);

    for (;;)
    {
        char c;

        if (Read(fh, &c, 1) != 1)
            break;
        if (c == '\n' || c == '\r')
            break;

        if (c == '\b' || c == 0x7F)
        {
            if (length > 0)
                length--;
            continue;
        }

        if (length < UG_PASSWORD_LEN)
            base->ug_PassBuf[length++] = c;
    }

    base->ug_PassBuf[length] = '\0';

    if (raw)
        SetMode(fh, 0);

    Write(fh, (APTR)"\n", 1);
    Close(fh);

    ug_set_err(base, 0);

    return (STRPTR)base->ug_PassBuf;
}

/* -------------------------------------------------------------- utmp ----- */

/*
 * There is no utmp or lastlog on AmigaOS and nothing to synthesise them from.
 * An empty database is a correct answer rather than a failure: getutent()
 * returning NULL is exactly what "no more entries" looks like, so callers walk
 * zero records and carry on.
 */
VOID ugl_setutent(UG_A6)
{
    (void)base;
}

struct ug_utmp *ugl_getutent(UG_A6)
{
    ug_set_err(base, 0);

    return NULL;
}

VOID ugl_endutent(UG_A6)
{
    (void)base;
}

struct ug_lastlog *ugl_getlastlog(UG_A6, register LONG uid __asm("d0"))
{
    (void)uid;

    ug_set_err(base, 0);

    return NULL;
}

/* Accepted and discarded: login tools treat a failure here as fatal. */
LONG ugl_setlastlog(UG_A6, register LONG uid __asm("d0"),
                           register STRPTR name __asm("a0"),
                           register STRPTR host __asm("a1"))
{
    (void)uid;
    (void)name;
    (void)host;

    ug_set_err(base, 0);

    return 0;
}
