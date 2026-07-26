/*
 * AmiNetXDuo tools -- the layer that turns a failure into an instruction.
 *
 * The stack's own diagnostics go to the serial port through ami_log(): they
 * are precise, they are aimed at whoever is debugging the stack, and nobody
 * setting up an Amiga for the first time will ever see them. Everything in
 * this file is the other half -- what the person at the keyboard is told, on
 * the console, when something they can fix is wrong.
 *
 * Three rules, which are why this is one file rather than sprinkled through
 * the commands:
 *
 *   1. Say what is wrong in words that do not assume the reader knows how a
 *      TCP/IP stack is put together.
 *   2. Say where: a path, a line number, a device name.
 *   3. Say what to do next, as a command they can type.
 *
 * Nothing here needs a running stack. That is deliberate: the machine that
 * needs explaining is exactly the machine where the stack did not come up, so
 * every check below works off the file system, the Exec device list and a
 * throw-away OpenDevice().
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>

/*
 * The NDK ships no <devices/sana2.h>; src/sana2/sana2_device.h restates the
 * published protocol and is what the whole project uses. Including it does
 * not pull in the shim -- only the structure and the two open tags.
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
 * allocated rather than declared on the stack: it is 260 bytes and must be
 * longword aligned, and a Shell command starts with a 4 KB stack.
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

            /* Workbench icons sit beside the real files; they are not one. */
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

/* ------------------------------------------------------- device discovery -- */

/*
 * Where a SANA-II driver could be. DEVS:Networks is where Roadshow, AmiTCP and
 * every installer since 1994 put them, and it is the answer we give when one
 * is missing; the rest are checked because a machine set up by hand often has
 * the driver somewhere else and telling the user "it is in the wrong place" is
 * far better than "not found".
 */
static const char *const diag_device_dirs[] =
{
    DIAG_DIR_NETWORKS,
    "DEVS:",
    "SYS:Storage/Networks",
    "SYS:Expansion",
    NULL
};

/*
 * SANA-II drivers we know by name. Only used to look harder -- a driver not on
 * this list is found perfectly well by the DEVS:Networks scan. It exists so
 * that a machine whose driver is already in memory (loaded from a Zorro ROM,
 * or by an earlier stack) is reported as having a card even though nothing is
 * on disk.
 */
static const char *const diag_known_devices[] =
{
    "a2065.device", "ariadne.device", "ariadne2.device", "amiganet.device",
    "cnet.device", "hydra.device", "x-surf.device", "xsurf100.device",
    "e3b_ax88796.device", "prism2.device", "eth3com.device", "rtl8029.device",
    "emac.device", "uaenet.device", "slip.device", "ppp.device",
    NULL
};

static ToolDevice diag_found[TOOL_MAX_DEVICES];
static UWORD      diag_found_count;
static BOOL       diag_scanned;

static BOOL diag_already_found(const char *name)
{
    UWORD i;

    for (i = 0; i < diag_found_count; i++)
    {
        if (tool_stricmp(diag_found[i].name, name) == 0)
            return TRUE;
    }

    return FALSE;
}

static VOID diag_add(const char *name, const char *where)
{
    if (diag_found_count >= (UWORD)TOOL_MAX_DEVICES || diag_already_found(name))
        return;

    tool_copy_string(diag_found[diag_found_count].name, TOOL_NAME_LEN, name);
    tool_copy_string(diag_found[diag_found_count].where, TOOL_NAME_LEN, where);
    diag_found_count++;
}

/* Is the driver already open somewhere on this machine? */
static BOOL diag_is_resident(const char *device)
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

ULONG tool_scan_devices(VOID)
{
    char  names[TOOL_MAX_DEVICES][TOOL_NAME_LEN];
    ULONG n;
    ULONG i;
    int   d;

    if (diag_scanned)
        return diag_found_count;

    diag_scanned     = TRUE;
    diag_found_count = 0;

    /* Everything in DEVS:Networks is, by convention, a SANA-II driver. */
    n = tool_list_dir(DIAG_DIR_NETWORKS, names, (ULONG)TOOL_MAX_DEVICES,
                      ".device");
    for (i = 0; i < n; i++)
        diag_add(names[i], DIAG_DIR_NETWORKS);

    /* Then the names we know, wherever they happen to be. */
    for (d = 0; diag_known_devices[d] != NULL; d++)
    {
        const char *name = diag_known_devices[d];
        int         dir;

        if (diag_already_found(name))
            continue;

        if (diag_is_resident(name))
        {
            diag_add(name, "already in memory");
            continue;
        }

        for (dir = 0; diag_device_dirs[dir] != NULL; dir++)
        {
            char path[TOOL_NAME_LEN * 2];

            tool_join_path(path, sizeof(path), diag_device_dirs[dir], name);
            if (tool_exists(path))
            {
                diag_add(name, diag_device_dirs[dir]);
                break;
            }
        }
    }

    return diag_found_count;
}

const ToolDevice *tool_scan_device(ULONG index)
{
    if (index >= (ULONG)diag_found_count)
        return NULL;

    return &diag_found[index];
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

/* ----------------------------------------------------------- device probe -- */

/*
 * SANA-II drivers are told at OpenDevice() time how to move packet data, and
 * several refuse to open at all without the tags. These two are never called
 * -- the probe opens and immediately closes -- but they have to be there and
 * they have to be real, so they are the same shape the shim uses (a0 = to,
 * a1 = from, d0 = length).
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

    status = (LONG)(BYTE)OpenDevice((CONST_STRPTR)device, unit,
                                    (struct IORequest *)req, 0);
    if (status == 0)
        CloseDevice((struct IORequest *)req);

    ami_free(req);
    DeleteMsgPort(port);

    return status;
}

/* ---------------------------------------------------------------- output -- */

/*
 * Advice is printed as a block: a blank line, then lines indented two spaces
 * under the one-line complaint tool_error() has already produced. The
 * indentation is what makes a wall of text scan as "this belongs to the error
 * above" on an 80-column screen.
 */
VOID tool_advise(const char *text)
{
    tool_printf("  %s\n", (LONG)text);
}

/*
 * Wrap `text` to the width of a Shell window, every line indented by `indent`
 * spaces. Only the config layer's messages go through this -- everything else
 * here is written as fixed lines, which reads better -- but those are built
 * at run time from a keyword the user typed, so their length is not known
 * until it is printed.
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

VOID tool_advise_blank(VOID)
{
    tool_printf("\n");
}

/* ------------------------------------------------------- config reporting -- */

static UWORD diag_problem_total;

static VOID diag_report(const AmiCfgProblem *problem, APTR user)
{
    (VOID)user;

    if (diag_problem_total++ == 0)
    {
        /*
         * A header on the first one only. Without it the list arrives before
         * the command has said anything at all, and reads as though it came
         * from nowhere.
         */
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

/* ------------------------------------------------------------- explainers -- */

VOID tool_explain_interface_file(const char *name)
{
    char  path[TOOL_NAME_LEN * 2];
    char  names[TOOL_MAX_DEVICES][TOOL_NAME_LEN];
    ULONG n;
    ULONG i;

    tool_join_path(path, sizeof(path), DIAG_DIR_INTERFACES, name);

    tool_advise_blank();
    tool_printf("  There is no file called %s.\n", (LONG)path);
    tool_advise("That file is how the stack learns which network card to use");
    tool_advise("and how it should get an address.");

    if (!tool_exists(DIAG_DIR_INTERFACES))
    {
        tool_advise_blank();
        tool_advise("The DEVS:NetInterfaces drawer does not exist at all, so no");
        tool_advise("network has ever been set up on this machine.");
    }
    else
    {
        n = tool_list_dir(DIAG_DIR_INTERFACES, names,
                          (ULONG)TOOL_MAX_DEVICES, NULL);
        if (n > 0)
        {
            tool_advise_blank();
            tool_advise("The interfaces that do exist are:");
            for (i = 0; i < n; i++)
                tool_printf("      %s\n", (LONG)names[i]);
            tool_advise("Check the spelling -- the name is the file name.");
        }
        else
        {
            tool_advise_blank();
            tool_advise("The DEVS:NetInterfaces drawer is empty.");
        }
    }

    tool_advise_blank();
    tool_advise("Run  NetSetup  to answer a few questions and have the file");
    tool_advise("written for you.");
}

VOID tool_explain_device(const char *device, ULONG unit)
{
    const char *where = tool_device_where(device);
    LONG        probe;

    tool_advise_blank();

    if (where == NULL)
    {
        tool_printf("  There is no %s on this machine.\n", (LONG)device);
        tool_advise_blank();
        tool_advise("That is the driver for your network card, and it has to be");
        tool_advise("installed before anything can use the card. Drivers belong");
        tool_advise("in DEVS:Networks/ -- they come with the card, or with the");
        tool_advise("operating system for cards Commodore made.");

        if (tool_scan_devices() > 0)
        {
            ULONG i;

            tool_advise_blank();
            tool_advise("These network drivers ARE installed:");
            for (i = 0; i < tool_scan_devices(); i++)
            {
                const ToolDevice *dev = tool_scan_device(i);

                tool_printf("      %-24s (%s)\n", (LONG)dev->name,
                            (LONG)dev->where);
            }
            tool_advise("If one of those is your card, run NetSetup and pick it.");
        }

        return;
    }

    /*
     * The driver is there, so ask it directly rather than guessing. This is
     * the difference between "would not open" and "your card is on unit 0,
     * not unit 1" -- which is the actual mistake most of the time.
     */
    probe = tool_device_probe(device, unit);

    if (probe == 0)
    {
        tool_printf("  %s unit %lu opens perfectly well on its own, so the\n",
                    (LONG)device, unit);
        tool_advise("card and its driver are fine.");
        tool_advise_blank();
        tool_advise("Something else has it open -- another network stack, or an");
        tool_advise("earlier copy of this one that is still running. A reboot");
        tool_advise("clears that. If this machine has no other stack installed,");
        tool_advise("the serial debug log records what actually failed.");
        return;
    }

    tool_printf("  %s is installed (%s) but unit %lu would not open.\n",
                (LONG)device, (LONG)where, unit);

    if (unit != 0 && tool_device_probe(device, 0) == 0)
    {
        tool_advise_blank();
        tool_printf("  Unit 0 opens. Almost every card is unit 0: change the UNIT\n");
        tool_printf("  line in DEVS:NetInterfaces to 0, or run NetSetup again.\n");
        return;
    }

    tool_advise_blank();
    tool_advise("The driver is installed but the card is not answering. Usually");
    tool_advise("that means the card is not in the machine, is not seated");
    tool_advise("properly, or needs a different unit number.");
}

VOID tool_explain_no_interfaces(VOID)
{
    ULONG n;

    tool_advise_blank();
    tool_advise("No network interfaces are configured.");
    tool_advise_blank();
    tool_advise("The stack reads one file per network card from");
    tool_advise("DEVS:NetInterfaces. There is nothing usable there yet.");

    n = tool_scan_devices();

    if (n > 0)
    {
        ULONG i;

        tool_advise_blank();
        tool_advise("The network card drivers on this machine are:");
        for (i = 0; i < n; i++)
        {
            const ToolDevice *dev = tool_scan_device(i);

            tool_printf("      %-24s (%s)\n", (LONG)dev->name, (LONG)dev->where);
        }
        tool_advise_blank();
        tool_advise("Run  NetSetup  and pick it from the list. Nothing has been");
        tool_advise("changed for you -- NetSetup asks first and writes after.");
    }
    else
    {
        tool_advise_blank();
        tool_advise("No network card driver could be found either. The driver for");
        tool_advise("your card belongs in DEVS:Networks/ -- for example");
        tool_advise("DEVS:Networks/ariadne.device for an Ariadne, or a2065.device");
        tool_advise("for an A2065. Copy it there first, then run  NetSetup.");
    }
}

VOID tool_explain_dhcp(const char *name)
{
    tool_advise_blank();
    tool_printf("  %s is up, but nothing on the network handed out an address.\n",
                (LONG)name);
    tool_advise_blank();
    tool_advise("That is what DHCP means: the Amiga asks, and a server -- on a");
    tool_advise("home network, the broadband router -- answers. Nothing did.");
    tool_advise_blank();
    tool_advise("Check, in this order:");
    tool_advise("   1. the network cable, at both ends;");
    tool_advise("   2. that the router or switch at the other end is powered on;");
    tool_advise("   3. that something on this network hands out addresses.");
    tool_advise_blank();
    tool_advise("If nothing does, run  NetSetup  and choose a fixed address");
    tool_advise("instead of DHCP.  ShowNetStatus  shows where it got to.");
}

VOID tool_explain_resolve(const char *name, LONG err)
{
    tool_advise_blank();

    switch (err)
    {
        case AMI_NET_ERR_NONAME:
            tool_printf("  The name servers were asked and none of them knows\n");
            tool_printf("  \"%s\".\n", (LONG)name);
            tool_advise_blank();
            tool_advise("Check the spelling. If it is right, the name may");
            tool_advise("simply not exist -- nothing is wrong with this");
            tool_advise("machine's network.");
            break;

        case AMI_NET_ERR_NOSERVER:
            tool_advise("This machine has no name server, so names cannot be");
            tool_advise("looked up at all. Numeric addresses still work.");
            tool_advise_blank();
            tool_advise("Run  NetSetup  and give it one, or put");
            tool_advise("   NAMESERVER <address>");
            tool_advise("in DEVS:Internet/name_resolution. On a home network");
            tool_advise("the router is usually the name server too.");
            break;

        case AMI_NET_ERR_TIMEOUT:
            tool_advise("The name server did not answer in time.");
            tool_advise_blank();
            tool_advise("Either it is not reachable from here or the address");
            tool_advise("configured for it is wrong.  ShowNetStatus  lists the");
            tool_advise("one in use, and  ping <that address>  says whether it");
            tool_advise("can be reached at all.");
            break;

        case AMI_NET_ERR_STATE:
            tool_advise("The network is not running, so nothing can be looked");
            tool_advise("up. Start it with  AddNetInterface eth0.");
            break;

        default:
            tool_advise("ShowNetStatus reports the state of the network.");
            break;
    }
}

VOID tool_explain_no_stack(VOID)
{
    tool_advise_blank();

    if (tool_stack_library_running())
    {
        ULONG addr = 0;
        char  text[16];

        tool_advise("The network itself is up -- another program has");
        tool_advise("bsdsocket.library open -- but this command cannot read the");
        tool_advise("details out of it.");

        if (tool_stack_query(&addr, NULL, 0) && addr != 0)
        {
            ami_config_format_ip(addr, text, sizeof(text));
            tool_advise_blank();
            tool_printf("  This machine's address is %s.\n", (LONG)text);
        }

        tool_advise_blank();
        tool_advise("The stack lives inside bsdsocket.library and there is no");
        tool_advise("call yet that lets a separate command look inside it, so");
        tool_advise("counters and per-interface detail are not available.");
        tool_advise("Nothing is wrong with your network. AddNetInterface,");
        tool_advise("Online and Offline work regardless.");
        return;
    }

    tool_advise("Nothing has started the network yet.");
    tool_advise_blank();
    tool_advise("Start it with:   AddNetInterface eth0");
    tool_advise("substituting the name of your interface file in");
    tool_advise("DEVS:NetInterfaces. If you have not set one up,  NetSetup");
    tool_advise("will do it for you.");
}

/* ------------------------------------------------------------ stack state -- */

BOOL tool_stack_library_running(VOID)
{
    struct Library *lib;
    BOOL            running = FALSE;

    /*
     * Looking, not opening: OpenLibrary("bsdsocket.library") would BRING THE
     * STACK UP, which is precisely what a status command must not do.
     *
     * Two signs, either of which is enough. The AMITCP public message port is
     * the conventional Amiga barrier -- src/netstack adds it when the stack
     * comes up and removes it on the way down, and `WaitForPort AMITCP` in
     * S:User-Startup waits on the same thing. The library's open count catches
     * a stack whose port could not be added (another one already owns the
     * name), which is a state worth reading as "running" too.
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

BOOL tool_stack_installed(VOID)
{
    return (BOOL)(tool_exists("LIBS:bsdsocket.library") ||
                  tool_stack_library_running());
}

/*
 * Is the bsdsocket.library on this machine ours?
 *
 * It matters because "bsdsocket.library" is the name every Amiga TCP/IP stack
 * answers to. A machine with Roadshow or AmiTCP already installed -- or one
 * running under an emulator that provides its own -- will hand out somebody
 * else's library, and the user needs to be told that rather than left
 * wondering why their AmiNetXDuo configuration has no effect. Two stacks
 * cannot share one machine.
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

    for (i = 0; id[i] != '\0' && i < 200UL; i++)
    {
        if (id[i] == 'A' && tool_stricmp_n(&id[i], "AmiNetXDuo", 10) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * bsdsocket.library LVOs, from the NDK's <inline/bsdsocket.h>. Called by hand
 * rather than through the NDK inlines because a command that merely asks a
 * question must not link the whole socket surface.
 */
#define LVO_gethostbyname   (-210)
#define LVO_gethostbyaddr   (-216)
#define LVO_gethostname     (-282)
#define LVO_gethostid       (-288)

/*
 * struct hostent, exactly as the NDK's <netdb.h> declares it (h_name,
 * h_aliases, LONG h_addrtype, LONG h_length, h_addr_list). Restated here
 * because tools.h has already pulled in the NetX Duo headers and mixing the
 * two socket worlds in one file is how this project's earlier ABI mistakes
 * happened; the layout is the published Roadshow one and is not guessed.
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

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res)
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

    __asm __volatile ("jsr a6@(-216:W)"
                      : "=r" (res)
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

    __asm __volatile ("jsr a6@(-282:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

struct Library *tool_stack_start(VOID)
{
    /*
     * This is how a command starts the network.
     *
     * The stack singleton cannot live in a command: ThreadX runs its Tasks on
     * stacks inside the hunk that created them, so a stack built here would be
     * executing freed memory the moment this program exits -- and the whole
     * point of AddNetInterface is that the interface stays up afterwards. The
     * stack therefore lives inside bsdsocket.library, which brings it up on
     * its first OpenLibrary() (docs/RESEARCH.md 3.3, "self-starting").
     *
     * Opening it is how you start the network; NOT closing it is how it stays
     * up. The reference this leaks is deliberate and is exactly the reference
     * AddNetInterface's own comment describes.
     */
    return OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
}

/*
 * Name lookup through the running stack's own vectors.
 *
 * This is what makes `host` work on a normal machine. The stack is inside
 * bsdsocket.library and a command cannot reach netstack_resolve() there, but
 * gethostbyname() is a published entry point and answers from the same
 * resolver -- including the name servers a DHCP lease supplied, which is
 * exactly the case the configuration files know nothing about.
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
 * The name servers the running stack is really using.
 *
 * This matters because a DHCP lease supplies them, and the files on disk
 * therefore say nothing (or something stale). ObtainDomainNameServerList() is
 * the published call that answers, and src/netstack now records the lease's
 * servers in the configuration it reports, so the two agree.
 *
 * struct DomainNameServerNode is mirrored from src/bsdsocket/roadshow.c,
 * which is the code that builds the list -- producer and consumer are both
 * ours, so this is not a guess at somebody else's ABI.
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

    __asm __volatile ("jsr a6@(-528:W)"
                      : /* no output */
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
    tool_advise_blank();
    tool_printf("  bsdsocket.library on this machine is \"%s\",\n",
                (LONG)((base != NULL && base->lib_IdString != NULL)
                           ? (const char *)base->lib_IdString
                           : "unidentified"));
    tool_advise("which is not AmiNetXDuo's.");
    tool_advise_blank();
    tool_advise("Every Amiga TCP/IP stack answers to that name and only one");
    tool_advise("can be installed at a time, so this machine is running the");
    tool_advise("other one -- your DEVS:NetInterfaces settings belong to it,");
    tool_advise("not to us. Remove the other stack, or use its own commands.");
}

BOOL tool_stack_query(ULONG *addr_out, char *host, ULONG hostlen)
{
    struct Library *base;

    if (addr_out != NULL)
        *addr_out = 0;
    if (host != NULL && hostlen > 0)
        host[0] = '\0';

    /*
     * Only ask a stack that is already running: opening the library when it
     * is not would start it, and no status command may do that.
     */
    if (!tool_stack_library_running())
        return FALSE;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
        return FALSE;

    if (addr_out != NULL)
        *addr_out = tool_call_gethostid(base);
    if (host != NULL && hostlen > 0)
        (VOID)tool_call_gethostname(base, host, hostlen);

    CloseLibrary(base);

    return TRUE;
}

/* ----------------------------------------------------------------- usage -- */

VOID tool_usage(const char *tmpl, const char *summary)
{
    tool_printf("Usage: %s %s\n", (LONG)tool_name, (LONG)tmpl);
    tool_printf("  %s\n", (LONG)summary);
}

/* ------------------------------------------------- the RUNNING stack -----
 *
 * NetStackQuery()/NetStackControl(), from the caller's side. These are the
 * only way a Shell command can see the stack that is actually running: its
 * own linked copy of NetX Duo is a different, empty one, and
 * src/tools/netstack_weak.c's netstack_ip() answers NULL. See
 * include/aminetxduo/netstatus.h and docs/RESEARCH.md 21.
 *
 * Same idiom as tool_call_gethostbyaddr() above and as nettrace.c's bpf_*
 * calls: an inline jsr through the library base, with the ABI's registers
 * named. d2 carries the size, which is a call-saved register -- the compiler
 * saves it around the call because it is a register variable here, exactly as
 * it does for the published vectors that use d2 (recvfrom, sendto).
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

    __asm __volatile ("jsr a6@(-870:W)"
                      : "=r" (res)
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

    __asm __volatile ("jsr a6@(-876:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

/* Errno(), LVO -0x0a2. What the two above leave behind on failure. */
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
     * Not "your library is too old" -- tool_netstatus_open() checks
     * lib_Revision and says that in its own words before any call is made. By
     * the time this is reached the library IS new enough and answered no, so
     * the interesting states are the ones below.
     */
    tool_advise_blank();
    tool_advise("The network is up and the library is the right one, but it");
    tool_advise("would not report on itself.");
    tool_advise_blank();
    tool_advise("That happens when the stack is starting or stopping and there");
    tool_advise("is no IP instance for a moment. Try again; if it persists the");
    tool_advise("serial debug log has what the stack said.");
}

struct Library *tool_netstatus_open(BOOL quiet)
{
    struct Library *base;

    /*
     * Looking before opening. OpenLibrary() on a stack that is not running
     * would START it, and a command that reports on the network must not
     * bring it up in order to have something to report.
     */
    if (!tool_stack_library_running())
    {
        if (!quiet)
        {
            tool_error("the network has not been started");
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
     * MANDATORY, not a nicety. These two vectors sit past everything any
     * published bsdsocket ABI names, so on somebody else's library that slot
     * is whatever their table happens to end with -- possibly the (APTR)-1
     * terminator, possibly nothing at all. Jumping through it would take the
     * machine down. The magic argument protects against a FUTURE vendor
     * defining the same offset; only this check protects against a present
     * one that has not defined it.
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
     * lib_Revision says which one. In the v0.2.0 library -- which is published
     * -- offset -0x366 is past the end of the vector table, on MakeLibrary()'s
     * (APTR)-1 terminator, and jumping there takes the machine down. A command
     * that gurus against last release's library is a worse answer than the
     * message this whole interface exists to stop printing.
     */
    if (base->lib_Revision < (UWORD)AMI_NETSTATUS_MIN_REVISION)
    {
        if (!quiet)
        {
            tool_error("the network is up, but the library running it is "
                       "older than this command");
            tool_advise_blank();
            tool_printf("  LIBS:bsdsocket.library is revision %ld; this "
                        "command needs %ld.\n",
                        (LONG)base->lib_Revision,
                        (LONG)AMI_NETSTATUS_MIN_REVISION);
            tool_advise_blank();
            tool_advise("The library and the commands come out of one build.");
            tool_advise("Install them together, and reboot so the new library");
            tool_advise("is the one in memory.");
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
     * The library reports the size of the struct IT was built with. If that
     * is not the size this command was built with, every field after the
     * first is at the wrong offset and the numbers would be plausible
     * nonsense -- which is worse than an error. The version check inside the
     * library catches a changed ABI; this catches one header compiled two
     * different ways, which is what a half-installed pair looks like.
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
