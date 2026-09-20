/* Mounted-volume policy for httpd's machine-wide root.
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "httpvol.h"
#include "httpstr.h"

/* Copy one live volume name out from under the DosList lock.  No lock leaves
   this call: filesystem handlers may need the same list while a client reads. */
BOOL http_vol_at(UWORD wanted, char *out, ULONG outlen)
{
    struct DosList *dl;
    UWORD           seen = 0;
    BOOL            found = FALSE;

    if (outlen == 0UL)
        return FALSE;
    out[0] = '\0';

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    if (dl == NULL)
        return FALSE;

    while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) != NULL)
    {
        const UBYTE *bstr;
        ULONG        len;
        ULONG        i;

        /* A remembered but unmounted volume has no handler.  Listing it can
           only provoke an insert-volume requester when the client enters it. */
        if (dl->dol_Task == NULL || dl->dol_Name == (BSTR)0)
            continue;

        if (seen++ != wanted)
            continue;

        bstr = (const UBYTE *)BADDR(dl->dol_Name);
        len  = (ULONG)bstr[0];
        if (len == 0UL || len + 1UL > outlen)
            break;

        for (i = 0; i < len; i++)
            out[i] = (char)bstr[i + 1UL];
        out[len] = '\0';
        found = TRUE;
        break;
    }

    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return found;
}

/* Keep the 112-byte name on this branch's frame instead of making every path
   through http_vol_resolve() carry it on a 4096-byte Shell stack. */
static __attribute__((noinline)) BOOL http_vol_mounted(const char *path)
{
    struct DosList *dl;
    char            name[HTTP_NAME_MAX];
    ULONG           n = 0;
    BOOL            found = FALSE;

    while (path[n] != '\0' && path[n] != ':')
    {
        if (n + 1UL >= sizeof(name))
            return FALSE;
        name[n] = path[n];
        n++;
    }
    if (n == 0UL || path[n] != ':')
        return FALSE;
    name[n] = '\0';

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    if (dl == NULL)
        return FALSE;

    while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) != NULL)
    {
        const UBYTE *bstr;

        if (dl->dol_Task == NULL || dl->dol_Name == (BSTR)0)
            continue;

        bstr = (const UBYTE *)BADDR(dl->dol_Name);
        if ((ULONG)bstr[0] == n &&
            hs_nicmp((const char *)&bstr[1], name, n) == 0)
        {
            found = TRUE;
            break;
        }
    }

    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return found;
}

HttpPathResult http_vol_resolve(BOOL volumes, const char *root,
                                const char *target, HttpPath *out)
{
    HttpPathResult why;

    if (!volumes)
        return http_path_resolve(root, target, out);

    why = http_path_resolve_volumes(target, out);
    if (why == HTTP_PATH_OK && out->segments > 0 &&
        !http_vol_mounted(out->path))
        return HTTP_PATH_NOT_VOLUME;

    return why;
}
