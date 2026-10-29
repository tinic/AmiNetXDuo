/*
 * AmiNetXDuo, shared plumbing for the command-line tools.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "toolquiet.h"
#include <dos/dostags.h>

/* ----------------------------------------------------------------- output */

/*
 * VPrintf() takes RawDoFmt's "data stream": a flat array of 32-bit arguments.
 * On m68k every C vararg is already pushed 32-bit-aligned in that exact
 * order, so a va_list is that stream. src/common/compat.c relies on the same
 * property for RawDoFmt().
 *
 * The argarray cast is (APTR), not (CONST_APTR). The two NDKs disagree about
 * the parameter: NDK 3.2 declares it CONST_APTR (`const void *`), NDK 3.9's
 * <inline/dos.h> writes `const APTR` (`void *const`, a const pointer, not a
 * pointer to const). Handing the latter a CONST_APTR is "initialization
 * discards const qualifier", which -Werror turns into a build failure. A plain
 * APTR converts cleanly to either, and the stream is a local va_list.
 */
VOID tool_printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
}

/*
 * The same, flushed.
 *
 * dos.library buffers Output(), so a command that runs for a minute prints its
 * running commentary in one burst at the end, and a machine that has to be
 * killed prints none of it at all.  Progress that arrives after the event is
 * not progress, and a stalled run then looks exactly like a working one.
 *
 * The cost is one Flush() per line, which is why this is a second function and
 * not a change to tool_printf(): a command printing a table wants the
 * buffering.
 */
VOID tool_say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);     /* (APTR): see tool_printf */
    va_end(args);

    Flush(Output());
}

/*
 * Where diagnostics go.
 *
 * Not ErrorOutput(): that is dos.library LVO -1134, which only exists from V50
 * on. On Kickstart 3.1 (V40, the floor, docs/RESEARCH.md 9) the LVO table
 * stops well short of it, so calling it jumps into whatever follows the library
 * and hangs the machine. Checked under FS-UAE, 2026-07-25.
 *
 * pr_CES is the field ErrorOutput() would have returned and has been in struct
 * Process since 2.0. It is zero unless the Shell was given a "*>" redirection,
 * in which case stdout is the right answer anyway.
 */
static BPTR tool_error_stream(VOID)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    BPTR            fh = (BPTR)0;

    if (pr != NULL && pr->pr_Task.tc_Node.ln_Type == NT_PROCESS)
        fh = pr->pr_CES;

    return (fh != (BPTR)0) ? fh : Output();
}

VOID tool_error(const char *fmt, ...)
{
    va_list args;
    BPTR    err = tool_error_stream();

    FPuts(err, (CONST_STRPTR)tool_name);
    FPuts(err, (CONST_STRPTR)": ");

    va_start(args, fmt);
    VFPrintf(err, (CONST_STRPTR)fmt, (APTR)args);   /* (APTR): see tool_printf */
    va_end(args);

    FPutC(err, '\n');
}

/*
 * ONE line, under the refusal it belongs to, and never more than one.
 *
 * WHAT THIS REPLACED was the house style of answering a failure with four or
 * five lines of prose, and the prose had two faults that no amount of writing
 * fixes.  It never named the call that refused, so the sentence could not be
 * looked up in the source or reported to anybody; and its usual last suggestion
 * was to consult a debug log, which a stock machine cannot read: the only log
 * the stack has is the serial port, and reaching it takes a null modem or
 * Sashimi.  It sent users to look for a file that does not exist.
 *
 * The shape now is: tool_error() with the operation and its code, then at most
 * one of these naming something that EXISTS -- a command to run, a line in a
 * file to change.  Same stream as tool_error(), so a "*>" redirection keeps the
 * pair together, and indented so the hint reads as subordinate to the refusal.
 */
VOID tool_hint(const char *fmt, ...)
{
    va_list args;
    BPTR    err = tool_error_stream();

    FPuts(err, (CONST_STRPTR)"  ");

    va_start(args, fmt);
    VFPrintf(err, (CONST_STRPTR)fmt, (APTR)args);   /* (APTR): see tool_printf */
    va_end(args);

    FPutC(err, '\n');
}

VOID tool_fault(LONG code)
{
    PrintFault(code, (CONST_STRPTR)tool_name);
}

/* ------------------------------------------------------------------ QUIET */

/*
 * NIL: rather than a flag in tool_error(): the explainers, PrintFault() and
 * every report print through Output() or pr_CES, and one redirection silences
 * all of them without each caller having to know.  The return code mapping
 * holds even if NIL: did not open.
 */
static BOOL quiet_on;
static BPTR quiet_nil;
static BPTR quiet_out;
static BPTR quiet_ces;

VOID tool_quiet_begin(VOID)
{
    struct Process *pr = (struct Process *)FindTask(NULL);

    if (quiet_on)
        return;
    quiet_on = TRUE;

    if (pr == NULL || pr->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return;

    quiet_nil = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (quiet_nil == (BPTR)0)
        return;

    quiet_out  = SelectOutput(quiet_nil);
    quiet_ces  = pr->pr_CES;
    pr->pr_CES = quiet_nil;
}

LONG tool_quiet_end(LONG rc)
{
    if (!quiet_on)
        return rc;
    quiet_on = FALSE;

    if (quiet_nil != (BPTR)0)
    {
        struct Process *pr = (struct Process *)FindTask(NULL);

        (VOID)SelectOutput(quiet_out);
        pr->pr_CES = quiet_ces;
        Close(quiet_nil);
        quiet_nil = (BPTR)0;
    }

    return (LONG)tool_quiet_rc(1, (long)rc);
}

/* ------------------------------------------------------------------ break */

VOID tool_break_arm(VOID)
{
    /* Drop anything inherited from the previous command. */
    SetSignal(0L, SIGBREAKF_CTRL_C);
}

BOOL tool_break(VOID)
{
    if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)
    {
        SetSignal(0L, SIGBREAKF_CTRL_C);
        return TRUE;
    }
    return FALSE;
}

BOOL tool_delay_ticks(ULONG ticks)
{
    while (ticks > 0)
    {
        ULONG slice = (ticks > 10UL) ? 10UL : ticks;

        if (tool_break())
            return TRUE;

        /* Delay(0) is documented as undefined, so it is skipped. */
        if (slice > 0)
            Delay(slice);
        ticks -= slice;
    }

    return tool_break();
}

/* ------------------------------------------------------------- formatting */

VOID tool_format_mac(const UBYTE *mac, char *buf, ULONG buflen)
{
    static const char hex[] = "0123456789abcdef";
    ULONG             i;
    ULONG             o = 0;

    if (buf == NULL || buflen == 0)
        return;

    for (i = 0; i < 6UL; i++)
    {
        if (o + 3 >= buflen)
            break;
        if (i > 0)
            buf[o++] = ':';
        buf[o++] = hex[(mac[i] >> 4) & 0x0f];
        buf[o++] = hex[mac[i] & 0x0f];
    }

    buf[o] = '\0';
}

ULONG tool_broadcast(ULONG addr, ULONG mask)
{
    return (addr & mask) | (~mask);
}

UWORD tool_prefix_len(ULONG mask)
{
    UWORD bits = 0;

    while (mask & 0x80000000UL)
    {
        bits++;
        mask <<= 1;
    }

    return bits;
}

/* ------------------------------------------------------------------ stack */

const char *tool_net_error(LONG err)
{
    switch (err)
    {
        case AMI_NET_OK:            return "no error";
        case AMI_NET_ERR_NOMEM:     return "out of memory";
        case AMI_NET_ERR_NODEV:     return "the SANA-II device did not open";
        case AMI_NET_ERR_DEVBAD:    return "the SANA-II device opened but did not answer";
        case AMI_NET_ERR_CONFIG:    return "the configuration is not usable";
        case AMI_NET_ERR_KERNEL:    return "the network kernel did not start";
        case AMI_NET_ERR_STATE:     return "the network stack is not running";
        case AMI_NET_ERR_NONAME:    return "no such name";
        case AMI_NET_ERR_NOSERVER:  return "no name server is configured";
        case AMI_NET_ERR_TIMEOUT:   return "the name server did not answer";
        case AMI_NET_ERR_BUSY:      return "interface in use";
        case AMI_NET_ERR_ABORTED:   return "aborted";
        case AMI_NET_ERR_NOSLOT:    return "every interface slot is taken";
        case AMI_NET_ERR_RETAINED:  return "SANA-II device still holds requests";
        default:                    return "unknown error";
    }
}

/* --------------------------------------------------------- the code names,
 *
 * WHY THESE EXIST.  A refusal that says only what a command thinks went wrong
 * cannot be looked up, and the sentence is a translation somebody has to
 * translate back.  These give the identifier that is in the source, so the
 * first line of a refusal locates the stage in the tree without anything else
 * being asked of the user.  The number goes beside it, because a code this
 * build does not know still has to be reportable.
 *
 * In tool_util.c, and so in every command, on purpose: the whole point is that
 * every refusal can name itself, and a table only some commands carry gives
 * only some commands the ability.  It is about 300 bytes.
 *
 * Never NULL.  An unknown code gets a token that reads as one, so the caller's
 * format string never has a hole in it and the number carries the meaning.
 */

const char *tool_code_net(LONG err)
{
    switch (err)
    {
        case AMI_NET_OK:            return "AMI_NET_OK";
        case AMI_NET_ERR_NOMEM:     return "AMI_NET_ERR_NOMEM";
        case AMI_NET_ERR_NODEV:     return "AMI_NET_ERR_NODEV";
        case AMI_NET_ERR_CONFIG:    return "AMI_NET_ERR_CONFIG";
        case AMI_NET_ERR_KERNEL:    return "AMI_NET_ERR_KERNEL";
        case AMI_NET_ERR_STATE:     return "AMI_NET_ERR_STATE";
        case AMI_NET_ERR_NONAME:    return "AMI_NET_ERR_NONAME";
        case AMI_NET_ERR_NOSERVER:  return "AMI_NET_ERR_NOSERVER";
        case AMI_NET_ERR_TIMEOUT:   return "AMI_NET_ERR_TIMEOUT";
        case AMI_NET_ERR_BUSY:      return "AMI_NET_ERR_BUSY";
        case AMI_NET_ERR_DEVBAD:    return "AMI_NET_ERR_DEVBAD";
        case AMI_NET_ERR_ABORTED:   return "AMI_NET_ERR_ABORTED";
        case AMI_NET_ERR_NOSLOT:    return "AMI_NET_ERR_NOSLOT";
        case AMI_NET_ERR_RETAINED:  return "AMI_NET_ERR_RETAINED";
        default:                    return "AMI_NET_ERR_?";
    }
}

/*
 * 4.4BSD's numbering, which is what every Amiga bsdsocket.library reports and
 * what src/bsdsocket/netstatus.c hands back from NetStackControl(). Only the
 * ones that vector can return are here; anything else prints as its number.
 */
const char *tool_code_errno(LONG err)
{
    switch (err)
    {
        case 0:     return "no error";
        case 1:     return "EPERM";
        case 2:     return "ENOENT";
        case 5:     return "EIO";
        case 6:     return "ENXIO";
        case 17:    return "EEXIST";
        case 22:    return "EINVAL";
        case 16:    return "EBUSY";
        case 28:    return "ENOSPC";
        case 36:    return "EINPROGRESS";
        case 55:    return "ENOBUFS";
        case 78:    return "ENOSYS";
        default:    return "errno";
    }
}

/*
 * SANA-II, devices/sana2.h.  ios2_Error is the first and ios2_WireError the
 * second, and they are reported as a pair because neither is meaningful alone:
 * S2ERR_OUTOFSERVICE with S2WERR_UNIT_OFFLINE is a card that was taken down,
 * and the same S2ERR_ with S2WERR_RCVREL_HDW_ERR is one that failed.
 */
const char *tool_code_sana2(LONG err)
{
    switch (err)
    {
        case 0:     return "S2ERR_NO_ERROR";
        case 1:     return "S2ERR_NO_RESOURCES";
        case 3:     return "S2ERR_BAD_ARGUMENT";
        case 4:     return "S2ERR_BAD_STATE";
        case 5:     return "S2ERR_BAD_ADDRESS";
        case 6:     return "S2ERR_MTU_EXCEEDED";
        case 8:     return "S2ERR_NOT_SUPPORTED";
        case 9:     return "S2ERR_SOFTWARE";
        case 10:    return "S2ERR_OUTOFSERVICE";
        case 11:    return "S2ERR_TX_FAILURE";
        default:    return "S2ERR_?";
    }
}

const char *tool_code_wire(LONG err)
{
    switch (err)
    {
        case 0:     return "S2WERR_GENERIC_ERROR";
        case 1:     return "S2WERR_NOT_CONFIGURED";
        case 2:     return "S2WERR_UNIT_ONLINE";
        case 3:     return "S2WERR_UNIT_OFFLINE";
        case 4:     return "S2WERR_ALREADY_TRACKED";
        case 5:     return "S2WERR_NOT_TRACKED";
        case 6:     return "S2WERR_BUFF_ERROR";
        case 7:     return "S2WERR_SRC_ADDRESS";
        case 8:     return "S2WERR_DST_ADDRESS";
        case 9:     return "S2WERR_BAD_BROADCAST";
        case 10:    return "S2WERR_BAD_MULTICAST";
        case 11:    return "S2WERR_MULTICAST_FULL";
        case 12:    return "S2WERR_BAD_EVENT";
        case 13:    return "S2WERR_BAD_STATDATA";
        case 15:    return "S2WERR_IS_CONFIGURED";
        case 16:    return "S2WERR_NULL_POINTER";
        case 17:    return "S2WERR_TOO_MANY_RETRIES";
        case 18:    return "S2WERR_RCVRBLE_HDW_ERR";
        default:    return "S2WERR_?";
    }
}

AmiNetStack *tool_require_stack(VOID)
{
    AmiNetStack *stack = netstack_get();

    if (stack == NULL)
    {
        /*
         * "not running" would be wrong when another program has the stack up
         * inside bsdsocket.library and we cannot see in. The two cases need
         * different action from the user, and tool_explain_no_stack() is where
         * that split lives -- saying it here too printed it twice.
         */
        tool_explain_no_stack();
    }

    return stack;
}

/*
 * The library beside this command first, then the library by name.
 *
 * A self-contained installation keeps bsdsocket.library and tls.library in
 * the Libs drawer next to the C drawer this command was loaded from, and
 * that Libs is the LAST member of LIBS:, behind the system's.  On a machine
 * that also has Roadshow or AmiTCP, "bsdsocket.library" is theirs; on one
 * that has an older AmiNetXDuo in LIBS:, "tls.library" is the older one.
 * PROGDIR:/Libs is the drawer the command came from, one level up: for a
 * system install that is SYS:C -> SYS:Libs, the same file LIBS: names.
 *
 * OpenLibrary() with a path first looks through the libraries already in
 * memory by the bare name, so once any bsdsocket.library is loaded every
 * caller gets that one -- which is what makes the S:Network-Startup line
 * the thing that selects the stack: it runs the drawer's AddNetInterface,
 * that opens the drawer's library, and everything after it finds that one.
 *
 * Requesters are off for the path: a command made resident has no PROGDIR:,
 * and a drawer somebody copied the command out of has no Libs beside it.
 *
 * The path is static, not a frame: 56 bytes here put GetNetStatus and arp
 * over their stack budgets (tools/check-stack-frames.sh), and a command is
 * one process.
 */
struct Library *tool_open_library(const char *name, ULONG version)
{
    static char     path[TOOL_NAME_LEN + 16];
    struct Process *me   = (struct Process *)FindTask(NULL);
    struct Library *base = NULL;

    if (me != NULL && me->pr_Task.tc_Node.ln_Type == NT_PROCESS)
    {
        APTR saved = me->pr_WindowPtr;

        tool_join_path(path, sizeof(path), "PROGDIR:/Libs", name);
        me->pr_WindowPtr = (APTR)-1L;
        base = OpenLibrary((CONST_STRPTR)path, version);
        me->pr_WindowPtr = saved;
    }

    if (base == NULL)
        base = OpenLibrary((CONST_STRPTR)name, version);

    return base;
}

/* SystemTagList's Shell has its own PROGDIR:. Resolve the companion before
   launching it, and quote both path and argument (AmigaDOS escapes with *).
   Static buffers keep this off a Shell command's small stack. */
static BOOL companion_quote(char *out, ULONG cap, ULONG *used,
                             const char *text)
{
    if (*used + 2 >= cap) return FALSE;
    out[(*used)++] = '"';
    while (*text != '\0')
    {
        char c = *text++;
        if ((UBYTE)c < 32 || (UBYTE)c == 127 || *used + 3 >= cap)
            return FALSE;
        if (c == '*' || c == '"' || c == '$' || c == '`')
            out[(*used)++] = '*';
        out[(*used)++] = c;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return TRUE;
}

LONG tool_run_companion(const char *command, const char *argument,
                        const char *options, BOOL silent)
{
    static char path[512], line[1600];
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved = me->pr_WindowPtr;
    BPTR home = GetProgramDir(), lock;
    BPTR input, output = (BPTR)0;
    ULONG i, used = 0;
    LONG rc;

    /* No volume requesters when resident or installed without siblings. */
    me->pr_WindowPtr = (APTR)-1L;
    for (i = 0; i < 3; i++)
    {
        if (i < 2)
        {
            if (home == (BPTR)0 ||
                !NameFromLock(home, (STRPTR)path, sizeof(path))) continue;
            if (i == 1 && !AddPart((STRPTR)path, (STRPTR)"C", sizeof(path)))
                continue;
        }
        else tool_copy_string(path, sizeof(path), "C:");
        if (!AddPart((STRPTR)path, (STRPTR)command, sizeof(path))) continue;
        lock = Lock((CONST_STRPTR)path, ACCESS_READ);
        if (lock != (BPTR)0) { UnLock(lock); break; }
    }
    me->pr_WindowPtr = saved;
    if (i == 3 || !companion_quote(line, sizeof(line), &used, path))
        return -1;
    line[used++] = ' ';
    if (!companion_quote(line, sizeof(line), &used, argument)) return -1;
    if (options != NULL && options[0] != '\0')
    {
        ULONG n = 0;
        while (options[n] != '\0') n++;
        if (used + n + 2 >= sizeof(line)) return -1;
        line[used++] = ' ';
        tool_copy_string(line + used, sizeof(line) - used, options);
    }

    input = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    if (silent) output = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (input == (BPTR)0 || (silent && output == (BPTR)0))
    {
        if (input != (BPTR)0) Close(input);
        if (output != (BPTR)0) Close(output);
        return -1;
    }
    rc = SystemTags((CONST_STRPTR)line,
                    SYS_Input, (ULONG)input,
                    silent ? SYS_Output : TAG_IGNORE, (ULONG)output,
                    NP_StackSize, 8192UL,
                    TAG_DONE);
    /* DOS owns these handles after a successful launch. */
    if (rc == -1)
    {
        Close(input);
        if (output != (BPTR)0) Close(output);
    }
    return rc;
}

const char *tool_basename(const char *path)
{
    const char *last = path;
    const char *p;

    for (p = path; *p != '\0'; p++)
    {
        if (*p == '/' || *p == ':')
            last = p + 1;
    }

    return last;
}

BOOL tool_from_workbench(int argc)
{
    /*
     * A Workbench launch arrives with argc == 0 and a WBStartup message instead
     * of a command line. None of these commands has a GUI, and ReadArgs() would
     * read the Shell's nonexistent arguments.
     */
    if (argc == 0)
    {
        tool_error("requires a Shell");
        return TRUE;
    }

    return FALSE;
}
