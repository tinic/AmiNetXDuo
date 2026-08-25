/*
 * AmiNetXDuo, shared plumbing for the command-line tools.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

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
 * was to consult a debug log, which no shipped build can produce, because
 * AMI_ERROR and friends compile to nothing without AMINETXDUO_LOG
 * (src/common/compat.h, docs/BACKLOG.md).  It sent users to look for a file
 * that does not exist.
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

/*
 * Every command that refuses an IPv6 address because the library it opened has
 * none ends the same way, so the sentence exists once. Two things a user
 * cannot get at from the refusal itself: that this is which library is
 * installed rather than something to switch on, and where the addresses the
 * machine does have are listed.
 */
VOID tool_no_ipv6_note(VOID)
{
    tool_printf("  IPv6 is a build option, not anything that can be "
                "switched on from\n"
                "  here. ShowNetStatus INTERFACES lists the addresses this "
                "machine has.\n");
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
        case AMI_NET_ERR_NONAME:    return "there is no such name";
        case AMI_NET_ERR_NOSERVER:  return "no name server is configured";
        case AMI_NET_ERR_TIMEOUT:   return "the name server did not answer";
        case AMI_NET_ERR_BUSY:      return "the interface is still in use";
        case AMI_NET_ERR_ABORTED:   return "the caller asked to be let go";
        case AMI_NET_ERR_NOSLOT:    return "every interface slot is taken";
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
        tool_error("this is a Shell command. Run it from a Shell");
        return TRUE;
    }

    return FALSE;
}
