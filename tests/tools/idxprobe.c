/*
 * IdxProbe, bisects the RFC 3493 if_* vectors, one flushed line per call.
 *
 * IfProbe's transcript stops mid-word inside its last phase, which is where
 * DOS's buffer happened to fill and not necessarily where the machine stopped.
 * This calls the same four vectors in the same order and flushes after every
 * one, so the last line printed is the last call that RETURNED.  CloseLibrary
 * is separated out because IfProbe is the only opener at that point in the run
 * and closing it tears the whole stack down.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

struct probe_if_nameindex { ULONG if_index; char *if_name; };

static ULONG p_if_nametoindex(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register ULONG           res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-882:W)"     /* if_nametoindex -0x372 */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (a0)
                      : "d2", "d3", "a1", "cc", "memory");
    return res;
}

static char *p_if_indextoname(struct Library *base, ULONG index, char *out)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = index;
    register char           *a0  __asm("a0") = out;
    register char           *res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-888:W)"     /* if_indextoname -0x378 */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (a0)
                      : "d2", "d3", "a1", "cc", "memory");
    return res;
}

static struct probe_if_nameindex *p_if_nameindex(struct Library *base)
{
    register struct Library            *a6  __asm("a6") = base;
    register struct probe_if_nameindex *res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-894:W)"     /* if_nameindex -0x37e */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6)
                      : "d2", "d3", "a0", "a1", "cc", "memory");
    return res;
}

static VOID p_if_freenameindex(struct Library *base,
                               struct probe_if_nameindex *ptr)
{
    register struct Library            *a6 __asm("a6") = base;
    register struct probe_if_nameindex *a0 __asm("a0") = ptr;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-900:W)"     /* if_freenameindex -0x384 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d2", "d3", "a1", "cc", "memory");
}

/* Every line is flushed: an unflushed transcript names the wrong statement. */
static VOID say(const char *fmt, LONG a, LONG b)
{
    Printf((CONST_STRPTR)fmt, a, b);
    Flush(Output());
}

int main(void)
{
    struct Library            *base;
    struct probe_if_nameindex *ni;
    char                       name[16];
    ULONG                      n;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        say("idx: no bsdsocket.library\n", 0, 0);
        return RETURN_FAIL;
    }

    say("idx: opened, revision %ld\n", (LONG)base->lib_Revision, 0);

    if (base->lib_Revision < 3)
    {
        say("idx: revision too old, nothing to probe\n", 0, 0);
        CloseLibrary(base);
        return RETURN_OK;
    }

    ni = p_if_nameindex(base);
    say("idx: if_nameindex returned %s\n",
        (LONG)((ni != NULL) ? "a list" : "NULL"), 0);

    if (ni != NULL)
    {
        for (n = 0; ni[n].if_name != NULL; n++)
        {
            ULONG back;
            char *got;

            say("idx: entry %ld index %ld\n", (LONG)n, (LONG)ni[n].if_index);

            back = p_if_nametoindex(base, ni[n].if_name);
            say("idx:   nametoindex('%s') = %ld\n",
                (LONG)ni[n].if_name, (LONG)back);

            name[0] = '\0';
            got = p_if_indextoname(base, ni[n].if_index, name);
            say("idx:   indextoname(%ld) = %s\n", (LONG)ni[n].if_index,
                (LONG)((got != NULL) ? name : "NULL"));
        }

        say("idx: walk finished, %ld entries\n", (LONG)n, 0);
    }

    /* The four edge cases IfProbe reaches after its last flushed line. */
    say("idx: -> nametoindex('nosuchif0')\n", 0, 0);
    say("idx: <- nametoindex('nosuchif0') = %ld\n",
        (LONG)p_if_nametoindex(base, "nosuchif0"), 0);

    name[0] = '\0';
    say("idx: -> indextoname(0)\n", 0, 0);
    say("idx: <- indextoname(0) = %s\n",
        (LONG)((p_if_indextoname(base, 0, name) != NULL) ? "non-NULL" : "NULL"), 0);

    name[0] = '\0';
    say("idx: -> indextoname(9999)\n", 0, 0);
    say("idx: <- indextoname(9999) = %s\n",
        (LONG)((p_if_indextoname(base, 9999UL, name) != NULL) ? "non-NULL" : "NULL"), 0);

    say("idx: -> freenameindex(list)\n", 0, 0);
    p_if_freenameindex(base, ni);
    say("idx: <- freenameindex(list)\n", 0, 0);

    say("idx: -> freenameindex(NULL)\n", 0, 0);
    p_if_freenameindex(base, NULL);
    say("idx: <- freenameindex(NULL)\n", 0, 0);

    /* The discriminator: everything above is the if_* trio, this is the whole
       stack coming down, because nothing else holds the library open. */
    say("idx: -> CloseLibrary\n", 0, 0);
    CloseLibrary(base);
    say("idx: <- CloseLibrary\n", 0, 0);

    return RETURN_OK;
}
