/*
 * AmiNetXDuo -- shared plumbing for the command-line tools.
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
 * The argarray cast is (APTR), not (CONST_APTR), and that is deliberate. The
 * two NDKs disagree about the parameter: NDK 3.2 declares it CONST_APTR
 * (`const void *`), while NDK 3.9's <inline/dos.h> writes `const APTR`
 * (`void *const` -- a const POINTER, not a pointer to const). Handing the
 * latter a CONST_APTR is "initialization discards const qualifier", which
 * -Werror turns into a build failure. A plain APTR converts cleanly to either,
 * and the stream is a local va_list that nothing is promising not to touch.
 */
VOID tool_printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
}

/*
 * Where diagnostics go.
 *
 * NOT ErrorOutput(): that is dos.library LVO -1134, which only exists from
 * V50 on. On Kickstart 3.1 (V40, the floor -- docs/RESEARCH.md 9) the LVO
 * table stops well short of it, so calling it jumps into whatever follows the
 * library and hangs the machine. Verified under FS-UAE, 2026-07-25.
 *
 * pr_CES is the same field ErrorOutput() would have returned and has been in
 * struct Process since 2.0; it is ZERO unless the Shell was given a "*>"
 * redirection, in which case stdout is the right answer anyway.
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

VOID tool_fault(LONG code)
{
    PrintFault(code, (CONST_STRPTR)tool_name);
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

        /* Delay(0) is documented as undefined; skip it. */
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
        case AMI_NET_ERR_NODEV:     return "the SANA-II device would not open";
        case AMI_NET_ERR_CONFIG:    return "the configuration is not usable";
        case AMI_NET_ERR_KERNEL:    return "the network kernel would not start";
        case AMI_NET_ERR_STATE:     return "the network stack is not running";
        case AMI_NET_ERR_NONAME:    return "there is no such name";
        case AMI_NET_ERR_NOSERVER:  return "no name server is configured";
        case AMI_NET_ERR_TIMEOUT:   return "the name server did not answer";
        default:                    return "unknown error";
    }
}

AmiNetStack *tool_require_stack(VOID)
{
    AmiNetStack *stack = netstack_get();

    if (stack == NULL)
    {
        /*
         * "not running" would be a lie when another program has the stack up
         * inside bsdsocket.library and we simply cannot see in. Which of the
         * two it is changes what the user should do about it, so it changes
         * the message; tool_explain_no_stack() prints the rest.
         */
        if (tool_stack_library_running())
            tool_error("the network is up, but this command cannot read it");
        else
            tool_error("the network has not been started");

        tool_explain_no_stack();
    }

    return stack;
}

LONG tool_find_interface(const char *name)
{
    const AmiConfig *cfg = netstack_config();
    UWORD            i;

    if (cfg == NULL)
    {
        if (tool_stack_library_running())
            tool_error("the network is up, but this command cannot read it");
        else
            tool_error("the network has not been started");

        tool_explain_no_stack();
        return -1;
    }

    for (i = 0; i < cfg->interface_count; i++)
    {
        const char *a = cfg->interfaces[i].name;
        const char *b = name;

        while (*a != '\0' && *b != '\0')
        {
            char ca = *a++;
            char cb = *b++;

            if (ca >= 'A' && ca <= 'Z')
                ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z')
                cb = (char)(cb + 32);
            if (ca != cb)
                break;
        }

        if (*a == '\0' && *b == '\0')
            return (LONG)i;
    }

    tool_error("there is no interface called \"%s\"", (LONG)name);

    if (cfg->interface_count == 0)
    {
        tool_explain_no_interfaces();
    }
    else
    {
        tool_advise_blank();
        tool_advise("The interfaces this machine has are:");
        for (i = 0; i < cfg->interface_count; i++)
            tool_printf("      %s\n", (LONG)cfg->interfaces[i].name);
        tool_advise("The name is the name of the file in DEVS:NetInterfaces.");
    }

    return -1;
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
     * A Workbench launch arrives with argc == 0 and a WBStartup message
     * instead of a command line. None of these commands has a GUI, and
     * ReadArgs() would read the Shell's -- nonexistent -- arguments.
     */
    if (argc == 0)
    {
        tool_error("this is a Shell command; run it from a Shell");
        return TRUE;
    }

    return FALSE;
}
