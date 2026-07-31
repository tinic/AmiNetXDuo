/*
 * IfNames -- list the interfaces by RFC 3493 index, and check the round trip.
 *
 * This is the Developer drawer's demonstration and its test at once.  It is
 * compiled against the STAGED DRAWER ALONE: the build stages Developer/ into
 * the build tree and puts only that and the NDK on the include path, so a
 * header the drawer forgot to ship is a compile error here rather than a
 * surprise for the first person who downloads the archive.
 *
 * The build target is tests/tools/CMakeLists.txt's test_ifnames.
 *
 * tests/tools/ifprobe.c reaches the same four vectors through hand-written
 * `jsr a6@(-882:W)`, because when it was written nothing declared them.  That
 * is what this file exists to make unnecessary; the two agreeing is also a
 * check that the drawer's LVOs are the ones the library really uses.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* The drawer.  proto/ pulls in clib/ for the prototypes and inline/ for the
   glue; aminetxduo/ifindex.h comes with it and carries the types. */
#include <proto/aminetxduo.h>

/* What the inline macros jump through.  The name is SocketBase because the
   addendum shares bsdsocket.library's base rather than opening a second
   library, so a program already using the NDK's inlines has it already. */
struct Library *SocketBase;

int main(void)
{
    struct if_nameindex *ni;
    char                 name[IF_NAMESIZE];
    ULONG                n;
    int                  rc = RETURN_OK;

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL) {
        Printf((CONST_STRPTR)"IfNames: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    /* The three-liner.  Nothing else can tell a caller whether the vectors
       are there, and calling them on an older library jumps into
       MakeLibrary()'s (APTR)-1 terminator. */
    if (SocketBase->lib_Revision < AMI_IFINDEX_MIN_REVISION) {
        Printf((CONST_STRPTR)"IfNames: bsdsocket.library revision %ld, "
                             "if_nametoindex needs %ld\n",
               (LONG)SocketBase->lib_Revision,
               (LONG)AMI_IFINDEX_MIN_REVISION);
        CloseLibrary(SocketBase);
        return RETURN_FAIL;
    }

    ni = if_nameindex();
    if (ni == NULL) {
        Printf((CONST_STRPTR)"IfNames: if_nameindex failed\n");
        CloseLibrary(SocketBase);
        return RETURN_FAIL;
    }

    for (n = 0; ni[n].if_index != 0 && ni[n].if_name != NULL; n++) {
        Printf((CONST_STRPTR)"%2ld  %s\n",
               (LONG)ni[n].if_index, (LONG)ni[n].if_name);

        if (if_nametoindex(ni[n].if_name) != ni[n].if_index) {
            Printf((CONST_STRPTR)"IfNames: %s did not map back to %ld\n",
                   (LONG)ni[n].if_name, (LONG)ni[n].if_index);
            rc = RETURN_ERROR;
        }
        if (if_indextoname(ni[n].if_index, name) == NULL) {
            Printf((CONST_STRPTR)"IfNames: index %ld had no name\n",
                   (LONG)ni[n].if_index);
            rc = RETURN_ERROR;
        }
    }

    if (n == 0)
        Printf((CONST_STRPTR)"IfNames: no interfaces\n");

    /* Index 0 is never an interface; RFC 3493 numbers them from 1. */
    if (if_nametoindex("nosuchif0") != 0) {
        Printf((CONST_STRPTR)"IfNames: an absent name got an index\n");
        rc = RETURN_ERROR;
    }
    if (if_indextoname(0, name) != NULL) {
        Printf((CONST_STRPTR)"IfNames: index 0 got a name\n");
        rc = RETURN_ERROR;
    }

    if_freenameindex(ni);
    if_freenameindex(NULL);     /* documented to do nothing */

    CloseLibrary(SocketBase);
    return rc;
}
