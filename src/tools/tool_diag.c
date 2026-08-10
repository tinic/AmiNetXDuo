/*
 * AmiNetXDuo tools, console diagnostics for the user at the keyboard.
 *
 * The stack's own diagnostics go to the serial port through ami_log() and are
 * aimed at whoever is debugging the stack. This file is the other half: what
 * gets printed on the console when something the user can fix is wrong. Kept
 * in one file so every command says it the same way:
 *
 *   1. What is wrong, without assuming the reader knows how a TCP/IP stack is
 *      put together.
 *   2. Where: a path, a line number, a device name.
 *   3. What to do next, as a command they can type.
 *
 * Nothing here needs a running stack, the machine that needs explaining is
 * the one where the stack did not come up, so every check below works off the
 * file system, the Exec device list and a throw-away OpenDevice().
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>

/*
 * The NDK ships no <devices/sana2.h>; src/sana2/sana2_device.h restates the
 * published protocol. Including it pulls in only the structure and the two
 * open tags, not the shim.
 */
#include "sana2_device.h"

#include "aminetxduo/compat.h"

extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ paths */

#define DIAG_DIR_INTERFACES   "DEVS:NetInterfaces"
#define DIAG_DIR_NETWORKS     "DEVS:Networks"
#define DIAG_DIR_INTERNET     "DEVS:Internet"

/* --------------------------------------------------------- file system ---- */

BOOL tool_exists(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);

    if (lock == (BPTR)0)
        return FALSE;

    UnLock(lock);
    return TRUE;
}

/*
 * A directory listing, into the caller's fixed table. The FileInfoBlock is
 * allocated rather than declared on the stack: 260 bytes, longword aligned,
 * against a Shell command's 4 KB stack.
 */
ULONG tool_list_dir(const char *path, char names[][TOOL_NAME_LEN], ULONG max,
                    const char *suffix)
{
    struct FileInfoBlock *fib;
    BPTR                  lock;
    ULONG                 count = 0;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return 0;

    fib = (struct FileInfoBlock *)ami_alloc((ULONG)sizeof(struct FileInfoBlock));
    if (fib == NULL)
    {
        UnLock(lock);
        return 0;
    }

    if (Examine(lock, fib))
    {
        while (count < max && ExNext(lock, fib))
        {
            const char *entry = (const char *)fib->fib_FileName;
            ULONG       len   = 0;

            if (fib->fib_DirEntryType > 0)          /* a drawer */
                continue;

            while (entry[len] != '\0')
                len++;

            /* Workbench icons sit beside the real files; skip them. */
            if (len >= 5 && tool_stricmp(entry + len - 5, ".info") == 0)
                continue;

            if (suffix != NULL)
            {
                ULONG slen = 0;

                while (suffix[slen] != '\0')
                    slen++;

                if (len <= slen || tool_stricmp(entry + len - slen, suffix) != 0)
                    continue;
            }

            tool_copy_string(names[count], TOOL_NAME_LEN, entry);
            count++;
        }
    }

    ami_free(fib);
    UnLock(lock);

    return count;
}

/* -------------------------------------------------------------- strings --- */

int tool_stricmp(const char *a, const char *b)
{
    for (;;)
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);

        if (ca != cb)
            return (int)(UBYTE)ca - (int)(UBYTE)cb;
        if (ca == '\0')
            return 0;
    }
}

int tool_stricmp_n(const char *a, const char *b, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);

        if (ca != cb)
            return (int)(UBYTE)ca - (int)(UBYTE)cb;
        if (ca == '\0')
            return 0;
    }

    return 0;
}

VOID tool_copy_string(char *dst, ULONG dstlen, const char *src)
{
    ULONG i;

    if (dst == NULL || dstlen == 0)
        return;

    for (i = 0; i + 1 < dstlen && src != NULL && src[i] != '\0'; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}

VOID tool_join_path(char *dst, ULONG dstlen, const char *dir, const char *name)
{
    ULONG pos = 0;

    if (dst == NULL || dstlen == 0)
        return;

    tool_copy_string(dst, dstlen, dir);
    while (dst[pos] != '\0')
        pos++;

    if (pos > 0 && dst[pos - 1] != '/' && dst[pos - 1] != ':' && pos + 1 < dstlen)
    {
        dst[pos++] = '/';
        dst[pos]   = '\0';
    }

    tool_copy_string(dst + pos, dstlen - pos, name);
}

/* ------------------------------------------------------- device discovery, */

/*
 * Where a SANA-II driver could be. DEVS:Networks is where Roadshow, AmiTCP and
 * every installer since 1994 put them, and is what we name when one is missing.
 * The rest are checked so a hand-installed driver can be reported as "in the
 * wrong place" rather than "not found".
 */
const char *const diag_device_dirs[] =
{
    DIAG_DIR_NETWORKS,
    "DEVS:",
    "SYS:Storage/Networks",
    "SYS:Expansion",
    NULL
};


/* Is the driver already open somewhere on this machine? */
BOOL diag_is_resident(const char *device)
{
    struct Node *node;
    BOOL         found = FALSE;

    Forbid();
    for (node = SysBase->DeviceList.lh_Head;
         node->ln_Succ != NULL;
         node = node->ln_Succ)
    {
        if (node->ln_Name != NULL &&
            tool_stricmp((const char *)node->ln_Name, device) == 0)
        {
            found = TRUE;
            break;
        }
    }
    Permit();

    return found;
}


const char *tool_device_where(const char *device)
{
    int dir;

    if (device == NULL || *device == '\0')
        return NULL;

    for (dir = 0; diag_device_dirs[dir] != NULL; dir++)
    {
        char path[TOOL_NAME_LEN * 2];

        tool_join_path(path, sizeof(path), diag_device_dirs[dir], device);
        if (tool_exists(path))
            return diag_device_dirs[dir];
    }

    if (diag_is_resident(device))
        return "already in memory";

    return NULL;
}

/* ----------------------------------------------------------- device probe, */

/*
 * SANA-II drivers are told at OpenDevice() time how to move packet data, and
 * several refuse to open without the tags. The probe opens and immediately
 * closes, so these are never called, but they must exist and be real functions
 * of the shape the shim uses (a0 = to, a1 = from, d0 = length).
 */
static BOOL diag_copy(register APTR to __asm("a0"),
                      register APTR from __asm("a1"),
                      register ULONG len __asm("d0"))
{
    (VOID)to;
    (VOID)from;
    (VOID)len;
    return FALSE;
}

LONG tool_device_probe(const char *device, ULONG unit)
{
    struct IOSana2Req *req;
    struct MsgPort    *port;
    struct TagItem     tags[3];
    LONG               status;

    if (device == NULL || *device == '\0')
        return TOOL_PROBE_NO_NAME;

    port = CreateMsgPort();
    if (port == NULL)
        return TOOL_PROBE_NO_MEMORY;

    req = (struct IOSana2Req *)ami_alloc((ULONG)sizeof(struct IOSana2Req));
    if (req == NULL)
    {
        DeleteMsgPort(port);
        return TOOL_PROBE_NO_MEMORY;
    }

    tags[0].ti_Tag  = S2_CopyToBuff;
    tags[0].ti_Data = (ULONG)diag_copy;
    tags[1].ti_Tag  = S2_CopyFromBuff;
    tags[1].ti_Data = (ULONG)diag_copy;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req->ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(struct IOSana2Req);
    req->ios2_Req.io_Message.mn_ReplyPort    = port;
    req->ios2_BufferManagement               = tags;

    status = ami_sana2_open_device(device, unit, (struct IORequest *)req);

    if (status == 0)
    {
        /*
         * Opening proves the driver and the unit; it does not prove the card.
         * src/sana2/sana2_device.c calls S2_DEVICEQUERY straight after its own
         * open and reports a refusal as AMI_NET_ERR_DEVBAD, so ask the same
         * question here, otherwise a card that opens and then answers
         * nothing is reported as "opens perfectly well", which is true and
         * useless.
         */
        struct Sana2DeviceQuery query;

        query.SizeAvailable = (ULONG)sizeof(query);
        query.SizeSupplied  = 0UL;

        req->ios2_Req.io_Command = S2_DEVICEQUERY;
        req->ios2_StatData       = &query;

        if (DoIO((struct IORequest *)req) != 0 || query.SizeSupplied == 0UL)
            status = TOOL_PROBE_REFUSED;

        CloseDevice((struct IORequest *)req);
    }

    ami_free(req);
    DeleteMsgPort(port);

    return status;
}

/* ---------------------------------------------------------------- output, */

/*
 * Wrap `text` to the width of a Shell window, every line indented by `indent`
 * spaces. Only the config layer's messages need this: they are built at run
 * time from a keyword the user typed, so their length is not known in advance.
 * Everything else here is written as fixed lines.
 */
#define TOOL_WRAP_WIDTH     77

VOID tool_wrap(ULONG indent, const char *text)
{
    char  line[TOOL_WRAP_WIDTH + 2];
    ULONG pos = 0;
    ULONG i;

    if (text == NULL)
        return;

    if (indent > 16)
        indent = 16;

    for (i = 0; i < indent; i++)
        line[pos++] = ' ';

    for (;;)
    {
        ULONG start;
        ULONG len;

        while (*text == ' ')
            text++;
        if (*text == '\0')
            break;

        /* Measure the next word. */
        for (len = 0; text[len] != '\0' && text[len] != ' '; len++)
            ;

        if (pos > indent && pos + 1 + len > (ULONG)TOOL_WRAP_WIDTH)
        {
            line[pos] = '\0';
            tool_printf("%s\n", (LONG)line);
            pos = 0;
            for (i = 0; i < indent; i++)
                line[pos++] = ' ';
        }

        if (pos > indent)
            line[pos++] = ' ';

        start = pos;
        for (i = 0; i < len && start + i + 1 < sizeof(line); i++)
            line[pos++] = text[i];

        text += len;
    }

    if (pos > indent)
    {
        line[pos] = '\0';
        tool_printf("%s\n", (LONG)line);
    }
}

/* ------------------------------------------------------- config reporting, */

static UWORD diag_problem_total;

static VOID diag_report(const AmiCfgProblem *problem, APTR user)
{
    (VOID)user;

    if (diag_problem_total++ == 0)
    {
        /* A header on the first problem only, so the list is introduced. */
        tool_printf("\nProblems in the configuration:\n");
    }

    if (problem->line > 0)
        tool_printf("  %s, line %lu:\n", (LONG)problem->file, problem->line);
    else
        tool_printf("  %s:\n", (LONG)problem->file);

    tool_wrap(6, problem->text);

    if (problem->hint != NULL)
        tool_wrap(6, problem->hint);
}

VOID tool_config_watch(VOID)
{
    diag_problem_total = 0;
    ami_config_set_reporter(diag_report, NULL);
}

VOID tool_config_unwatch(VOID)
{
    ami_config_set_reporter(NULL, NULL);
}

/* ------------------------------------------------------------- explainers, */

VOID tool_explain_interface_file(const char *name)
{
    char path[TOOL_NAME_LEN * 2];

    tool_join_path(path, sizeof(path), DIAG_DIR_INTERFACES, name);
    tool_printf("%s: %s: no such interface file\n", (LONG)tool_name, (LONG)path);
}


VOID tool_explain_dhcp(const char *name)
{
    tool_printf("%s: %s: no address from DHCP\n", (LONG)tool_name, (LONG)name);
}

VOID tool_explain_resolve(const char *name, LONG err)
{
    const char *why;

    switch (err)
    {
        case AMI_NET_ERR_NONAME:   why = "no such name";              break;
        case AMI_NET_ERR_NOSERVER: why = "no name server configured"; break;
        case AMI_NET_ERR_TIMEOUT:  why = "name server did not answer"; break;
        case AMI_NET_ERR_STATE:    why = "network not started";       break;
        default:                   why = "lookup failed";             break;
    }

    tool_printf("%s: cannot resolve \"%s\": %s\n",
                (LONG)tool_name, (LONG)name, (LONG)why);
}

VOID tool_explain_no_stack(VOID)
{
    if (tool_stack_library_running())
    {
        tool_printf("%s: the running stack does not report its state\n",
                    (LONG)tool_name);
        return;
    }

    tool_printf("%s: network not started\n", (LONG)tool_name);
}

/* ------------------------------------------------------------ stack state, */

BOOL tool_stack_library_running(VOID)
{
    struct Library *lib;
    BOOL            running = FALSE;

    /*
     * Looking, not opening: OpenLibrary("bsdsocket.library") would bring the
     * stack up, which a status command must not do.
     *
     * Either sign is enough. The AMITCP public message port is the conventional
     * Amiga barrier, src/netstack adds it when the stack comes up and removes
     * it on the way down, and `WaitForPort AMITCP` in S:User-Startup waits on
     * the same thing. The library's open count also counts as running, and
     * catches a stack whose port could not be added because another one already
     * owns the name.
     */
    Forbid();

    if (FindPort((CONST_STRPTR)"AMITCP") != NULL)
    {
        running = TRUE;
    }
    else
    {
        lib = (struct Library *)FindName(&SysBase->LibList,
                                         (CONST_STRPTR)"bsdsocket.library");
        if (lib != NULL && lib->lib_OpenCnt > 0)
            running = TRUE;
    }

    Permit();

    return running;
}

/*
 * What the LOADED library says it is, copied out without opening it.
 *
 * The version that matters to someone asking "what am I running" is the
 * library's, not the command's: C: and LIBS: are updated separately, and a
 * machine with new commands over an old library is exactly the case worth
 * reporting. Reading lib_IdString answers for the copy actually in memory.
 *
 * Looking, not opening, for the reason tool_stack_library_running() gives,
 * a status command must not start the network as a side effect of being asked
 * a question. Copied rather than returned by pointer, because the library can
 * expunge the moment Forbid() ends and the string goes with it.
 *
 * FALSE when no library is loaded, which is not an error: nothing has opened
 * it yet, and the caller says so in its own words.
 */
BOOL tool_stack_version(char *buf, ULONG len)
{
    struct Library *lib;
    BOOL            got = FALSE;

    if (buf == NULL || len == 0UL)
        return FALSE;

    buf[0] = '\0';

    Forbid();

    lib = (struct Library *)FindName(&SysBase->LibList,
                                     (CONST_STRPTR)"bsdsocket.library");
    if (lib != NULL && lib->lib_IdString != NULL)
    {
        const char *id = (const char *)lib->lib_IdString;
        ULONG       i;

        /* lib_IdString ends "\r\n" by convention; neither belongs in a line
           this command is composing itself. */
        for (i = 0; i + 1UL < len && id[i] != '\0' &&
                    id[i] != '\r' && id[i] != '\n'; i++)
            buf[i] = id[i];

        buf[i] = '\0';
        got    = (BOOL)(i > 0UL);
    }

    Permit();

    return got;
}

BOOL tool_stack_installed(VOID)
{
    return (BOOL)(tool_exists("LIBS:bsdsocket.library") ||
                  tool_stack_library_running());
}

/*
 * Is the bsdsocket.library on this machine ours? Every Amiga TCP/IP stack
 * answers to that name, and two cannot share one machine, so a box with
 * Roadshow or AmiTCP installed (or an emulator providing its own) hands out
 * somebody else's library and the AmiNetXDuo configuration has no effect.
 */
BOOL tool_stack_is_ours(struct Library *base)
{
    const char *id;
    ULONG       i;

    if (base == NULL)
        return FALSE;

    id = (const char *)base->lib_IdString;
    if (id == NULL)
        return FALSE;

    /* The cap is tested first: `id` belongs to a foreign library, and the old
       order read id[200] before deciding to stop there. */
    for (i = 0; i < 200UL && id[i] != '\0'; i++)
    {
        if (id[i] == 'A' && tool_stricmp_n(&id[i], "AmiNetXDuo", 10) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * bsdsocket.library LVOs, from the NDK's <inline/bsdsocket.h>. Called by hand
 * so a command that only asks a question does not link the whole socket
 * surface.
 */
#define LVO_gethostbyname   (-210)
#define LVO_gethostbyaddr   (-216)
#define LVO_gethostname     (-282)
#define LVO_gethostid       (-288)
#define LVO_GetDefaultDomainName (-702)

/*
 * struct hostent, exactly as the NDK's <netdb.h> declares it (h_name,
 * h_aliases, LONG h_addrtype, LONG h_length, h_addr_list). Restated here
 * because tools.h has already pulled in the NetX Duo headers, and mixing the
 * two socket worlds in one file caused earlier ABI mistakes. The layout is the
 * published Roadshow one, not a guess.
 */
typedef struct ToolHostEnt
{
    char  *h_name;
    char **h_aliases;
    LONG   h_addrtype;
    LONG   h_length;
    char **h_addr_list;
} ToolHostEnt;

static ToolHostEnt *tool_call_gethostbyname(struct Library *base,
                                            const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register ToolHostEnt    *res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

static ToolHostEnt *tool_call_gethostbyaddr(struct Library *base,
                                            const UBYTE *addr, LONG len,
                                            LONG type)
{
    register struct Library *a6  __asm("a6") = base;
    register const UBYTE    *a0  __asm("a0") = addr;
    register LONG            d0  __asm("d0") = len;
    register LONG            d1  __asm("d1") = type;
    register ToolHostEnt    *res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-216:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static ULONG tool_call_gethostid(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           res __asm("d0");

    __asm __volatile ("jsr a6@(-288:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG tool_call_gethostname(struct Library *base, char *name, ULONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register char           *a0  __asm("a0") = name;
    register ULONG           d0  __asm("d0") = len;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-282:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

static BOOL tool_call_default_domain(struct Library *base, char *buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register char           *a0  __asm("a0") = buf;
    register LONG            d0  __asm("d0") = len;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-702:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "d1", "a1", "cc", "memory");
    return (BOOL)(res != 0);
}

struct Library *tool_stack_start(VOID)
{
    /*
     * How a command starts the network.
     *
     * The stack singleton cannot live in a command: ThreadX runs its Tasks on
     * stacks inside the hunk that created them, so a stack built here would be
     * executing freed memory the moment the program exits, and AddNetInterface
     * needs the interface to stay up afterwards. The stack therefore lives
     * inside bsdsocket.library, which brings it up on its first OpenLibrary()
     * (docs/RESEARCH.md 3.3, "self-starting").
     *
     * Opening it starts the network; not closing it is how it stays up. The
     * leaked reference is intentional, the same one AddNetInterface's comment
     * describes.
     */
    return OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
}

/*
 * Name lookup through the running stack's own vectors. A command cannot reach
 * netstack_resolve() inside bsdsocket.library, but gethostbyname() is a
 * published entry point into the same resolver, including name servers a DHCP
 * lease supplied, which the configuration files know nothing about.
 */
BOOL tool_stack_lookup(const char *name, ULONG *addr_out)
{
    struct Library *base;
    ToolHostEnt    *he;
    BOOL            ok = FALSE;

    if (name == NULL || addr_out == NULL || !tool_stack_library_running())
        return FALSE;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
        return FALSE;

    he = tool_call_gethostbyname(base, name);

    /* The hostent belongs to our opener base, so read it before closing. */
    if (he != NULL && he->h_addr_list != NULL && he->h_addr_list[0] != NULL &&
        he->h_length == 4)
    {
        const UBYTE *b = (const UBYTE *)he->h_addr_list[0];

        *addr_out = ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
                    ((ULONG)b[2] <<  8) |  (ULONG)b[3];
        ok = TRUE;
    }

    CloseLibrary(base);

    return ok;
}

BOOL tool_stack_lookup_addr(ULONG addr, char *name_out, ULONG name_len)
{
    struct Library *base;
    ToolHostEnt    *he;
    UBYTE           quad[4];
    BOOL            ok = FALSE;

    if (name_out == NULL || name_len == 0 || !tool_stack_library_running())
        return FALSE;

    name_out[0] = '\0';

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
        return FALSE;

    quad[0] = (UBYTE)((addr >> 24) & 0xff);
    quad[1] = (UBYTE)((addr >> 16) & 0xff);
    quad[2] = (UBYTE)((addr >>  8) & 0xff);
    quad[3] = (UBYTE)(addr & 0xff);

    he = tool_call_gethostbyaddr(base, quad, 4L, 2L /* AF_INET */);

    if (he != NULL && he->h_name != NULL && he->h_name[0] != '\0')
    {
        tool_copy_string(name_out, name_len, he->h_name);
        ok = TRUE;
    }

    CloseLibrary(base);

    return ok;
}

/*
 * The name servers the running stack is really using. A DHCP lease supplies
 * them, so the files on disk say nothing or something stale.
 * ObtainDomainNameServerList() is the published call that answers, and
 * src/netstack records the lease's servers in the configuration it reports, so
 * the two agree.
 *
 * struct DomainNameServerNode is mirrored from src/bsdsocket/roadshow.c, the
 * code that builds the list, so producer and consumer are both ours.
 */
typedef struct ToolDnsNode
{
    struct MinNode  dnsn_MinNode;
    LONG            dnsn_Size;
    char           *dnsn_Address;       /* dotted quad, as text */
    LONG            dnsn_UseCount;
} ToolDnsNode;

static struct List *tool_call_obtain_dns(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register struct List    *res __asm("d0");

    __asm __volatile ("jsr a6@(-534:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static VOID tool_call_release_dns(struct Library *base, struct List *list)
{
    register struct Library *a6 __asm("a6") = base;
    register struct List    *a0 __asm("a0") = list;
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-528:W)"
                      : "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d0", "d1", "a1", "cc", "memory");
}

ULONG tool_stack_name_servers(char out[][16], ULONG max)
{
    struct Library *base;
    struct List    *list;
    ULONG           count = 0;

    if (out == NULL || max == 0 || !tool_stack_library_running())
        return 0;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
        return 0;

    list = tool_call_obtain_dns(base);
    if (list != NULL)
    {
        struct MinNode *node = (struct MinNode *)list->lh_Head;

        while (node != NULL && node->mln_Succ != NULL && count < max)
        {
            const ToolDnsNode *dns = (const ToolDnsNode *)node;

            if (dns->dnsn_Address != NULL)
                tool_copy_string(out[count++], 16, dns->dnsn_Address);

            node = node->mln_Succ;
        }

        tool_call_release_dns(base, list);
    }

    CloseLibrary(base);

    return count;
}

VOID tool_explain_foreign_stack(struct Library *base)
{
    tool_printf("%s: bsdsocket.library is \"%s\", not AmiNetXDuo\n",
                (LONG)tool_name,
                (LONG)((base != NULL && base->lib_IdString != NULL)
                           ? (const char *)base->lib_IdString
                           : "unidentified"));
}

BOOL tool_stack_query(ULONG *addr_out, char *host, ULONG hostlen)
{
    struct Library *base;

    if (addr_out != NULL)
        *addr_out = 0;
    if (host != NULL && hostlen > 0)
        host[0] = '\0';

    /*
     * Only ask a stack that is already running: opening the library otherwise
     * would start it, and no status command may do that.
     */
    if (!tool_stack_library_running())
        return FALSE;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
        return FALSE;

    if (addr_out != NULL)
        *addr_out = tool_call_gethostid(base);
    if (host != NULL && hostlen > 0)
    {
        /* A name too long for the buffer comes back without a terminator
           (bsdsocket.doc gethostname); the buffer's owner puts one back. */
        (VOID)tool_call_gethostname(base, host, hostlen);
        host[hostlen - 1] = '\0';
    }

    CloseLibrary(base);

    return TRUE;
}

BOOL tool_stack_domain(char *domain, ULONG domainlen)
{
    struct Library *base;
    BOOL            got;

    if (domain == NULL || domainlen == 0)
        return FALSE;

    domain[0] = '\0';

    if (!tool_stack_library_running())
        return FALSE;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
        return FALSE;

    /*
     * Only our own library. GetDefaultDomainName() is a Roadshow extension, so
     * on an AmiTCP-era bsdsocket.library -0x2be can be past the end of the
     * vector table, a guru rather than an answer, the same hazard
     * aminetxduo/netstatus.h describes for its own two slots.
     */
    if (!tool_stack_is_ours(base))
    {
        CloseLibrary(base);
        return FALSE;
    }

    got = tool_call_default_domain(base, domain, (LONG)domainlen);
    domain[domainlen - 1] = '\0';

    CloseLibrary(base);

    return (BOOL)(got && domain[0] != '\0');
}

/* ----------------------------------------------------------------- usage, */

VOID tool_usage(const char *tmpl, const char *summary)
{
    tool_printf("Usage: %s %s\n", (LONG)tool_name, (LONG)tmpl);
    tool_printf("  %s\n", (LONG)summary);
}

/* ------------------------------------------------- the running stack -----
 *
 * NetStackQuery()/NetStackControl(), from the caller's side. These are the only
 * way a Shell command can see the stack that is actually running: its own
 * linked copy of NetX Duo is a different, empty one, and
 * src/tools/netstack_weak.c's netstack_ip() answers NULL. See
 * include/aminetxduo/netstatus.h and docs/RESEARCH.md 21.
 *
 * Same idiom as tool_call_gethostbyaddr() above and nettrace.c's bpf_* calls:
 * an inline jsr through the library base with the ABI's registers named. d2
 * carries the size and is call-saved; the compiler saves it around the call
 * because it is a register variable here, as it does for the published vectors
 * that use d2 (recvfrom, sendto).
 */

static LONG tool_call_netstatus_query(struct Library *base, ULONG what,
                                      APTR buffer, ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = what;
    register APTR            a0  __asm("a0") = buffer;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-870:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG tool_call_netstatus_control(struct Library *base, ULONG op,
                                        APTR arg, ULONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = arg;
    register ULONG           d2  __asm("d2") = size;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

/*
 * The two IPv6 text conversions, through src/config/config_text6.c.
 *
 * Not through the library's inet_ntop() / inet_pton(): those answer
 * EAFNOSUPPORT for AF_INET6 on an IPv4-only library, and one set of commands
 * serves a library built either way.  A command has to tell "::1" from a typo
 * whether the machine can route to it or not, see the head of
 * config_text6.c.  Neither needs the library open.
 */
VOID tool_format_ip6(const ULONG addr[4], char *buf, ULONG buflen)
{
    if (buflen == 0)
        return;

    buf[0] = '\0';

    if (addr != NULL)
        ami_config_format_ip6(addr, buf, buflen);
}

BOOL tool_parse_ip6(const char *text, ULONG out[4])
{
    if (text == NULL || out == NULL)
        return FALSE;

    return ami_config_parse_ip6(text, out, NULL);
}

/* Errno(), LVO -0x0a2: what the two above leave behind on failure. */
static LONG tool_call_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

_Static_assert(AMI_NETSTATUS_QUERY_LVO   == -870, "NetStackQuery LVO");
_Static_assert(AMI_NETSTATUS_CONTROL_LVO == -876, "NetStackControl LVO");

VOID tool_explain_no_netstatus(struct Library *base)
{
    if (base != NULL && !tool_stack_is_ours(base))
    {
        tool_explain_foreign_stack(base);
        return;
    }

    /*
     * The library is not too old here: tool_netstatus_open() checks
     * lib_Revision and reports that itself before any call is made. Reaching
     * this point means a new enough library answered no.
     */
    tool_printf("%s: the stack would not report on itself\n", (LONG)tool_name);
}

struct Library *tool_netstatus_open(BOOL quiet)
{
    struct Library *base;

    /*
     * Looking before opening: OpenLibrary() on a stack that is not running
     * would start it, and a command that reports on the network must not bring
     * it up to have something to report.
     */
    if (!tool_stack_library_running())
    {
        if (!quiet)
        {
            /* tool_explain_no_stack() prints this same sentence itself when
               the library is not running, and something better when it is. */
            tool_explain_no_stack();
        }
        return NULL;
    }

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        if (!quiet)
            tool_error("bsdsocket.library would not open");
        return NULL;
    }

    /*
     * Required, not a nicety. These two vectors sit past everything any
     * published bsdsocket ABI names, so on somebody else's library that slot is
     * whatever their table ends with, possibly the (APTR)-1 terminator,
     * possibly nothing. Jumping through it takes the machine down. The magic
     * argument protects against a future vendor defining the same offset; only
     * this check protects against a present one that has not.
     */
    if (!tool_stack_is_ours(base))
    {
        if (!quiet)
        {
            tool_error("the network is up, but it is not this stack");
            tool_explain_foreign_stack(base);
        }
        CloseLibrary(base);
        return NULL;
    }

    /*
     * And ours has to be new enough. lib_IdString says whose library it is;
     * lib_Revision says which one. In the published v0.2.0 library, offset
     * -0x366 is past the end of the vector table, on MakeLibrary()'s (APTR)-1
     * terminator, and jumping there takes the machine down.
     */
    if (base->lib_Revision < (UWORD)AMI_NETSTATUS_MIN_REVISION)
    {
        if (!quiet)
        {
            tool_printf("%s: bsdsocket.library is revision %ld, need %ld\n",
                        (LONG)tool_name,
                        (LONG)base->lib_Revision,
                        (LONG)AMI_NETSTATUS_MIN_REVISION);
        }
        CloseLibrary(base);
        return NULL;
    }

    return base;
}

VOID tool_netstatus_close(struct Library *base)
{
    if (base != NULL)
        CloseLibrary(base);
}

LONG tool_netstatus_query(struct Library *base, ULONG what,
                          APTR buffer, ULONG size, ULONG entry_size)
{
    NetStatusHeader *hdr = (NetStatusHeader *)buffer;

    if (base == NULL || hdr == NULL || size < sizeof(NetStatusHeader))
        return -1;

    hdr->nsh_Magic   = AMI_NETSTATUS_MAGIC;
    hdr->nsh_Version = (UWORD)AMI_NETSTATUS_VERSION;

    if (tool_call_netstatus_query(base, what, buffer, size) < 0)
        return -1;

    /*
     * The library reports the size of the struct it was built with. If that
     * differs from this command's, every field after the first is at the wrong
     * offset and the numbers come out as plausible nonsense. The version check
     * inside the library catches a changed ABI; this catches one header
     * compiled two different ways, i.e. a half-installed pair.
     */
    if (hdr->nsh_Type != (UWORD)what)
        return -1;
    if (hdr->nsh_Count > 0 && hdr->nsh_EntrySize != (UWORD)entry_size)
        return -1;

    return (LONG)hdr->nsh_Count;
}

LONG tool_netstatus_control(struct Library *base, ULONG op,
                            NetStatusControl *ctl, LONG *errno_out)
{
    LONG rc;

    if (errno_out != NULL)
        *errno_out = 0;

    if (base == NULL || ctl == NULL)
        return -1;

    ctl->nsc_Magic   = AMI_NETSTATUS_MAGIC;
    ctl->nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;

    rc = tool_call_netstatus_control(base, op, ctl, sizeof(NetStatusControl));
    if (rc < 0 && errno_out != NULL)
        *errno_out = tool_call_errno(base);

    return rc;
}

BOOL tool_netstatus_system(NetStatusSystem *out)
{
    struct Library *base;
    struct
    {
        NetStatusHeader hdr;
        NetStatusSystem sys;
    } buf;
    BOOL ok = FALSE;

    if (out == NULL)
        return FALSE;

    base = tool_netstatus_open(TRUE);
    if (base == NULL)
        return FALSE;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &buf, sizeof(buf),
                             sizeof(NetStatusSystem)) > 0)
    {
        *out = buf.sys;
        ok = TRUE;
    }

    tool_netstatus_close(base);

    return ok;
}
