/* CLI crash supervisor. Each child gets its requested Shell stack, not ours.
 * SPDX-License-Identifier: MIT */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase;
static struct Task *parent;
static volatile LONG child_rc;
static volatile ULONG child_stack;
static ULONG gone;
static BPTR journal;
static char line[8192], arguments[8192];
static struct IntuiMessage close_message;
static struct MsgPort *close_port;

static VOID child_exit(register LONG rc __asm("d0"),
                       register APTR data __asm("d1"))
{
    struct Task *me = FindTask(NULL);
    (VOID)data;
    child_rc = rc;
    child_stack = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    Signal(parent, gone);
}

static ULONG number(const char *s)
{
    ULONG n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10UL + (ULONG)(*s++ - '0');
    return n;
}

static char *field(char *s)
{
    while (*s != '\0' && *s != '\t') s++;
    if (*s != '\t') return NULL;
    *s++ = '\0';
    return s;
}

static BOOL finished(VOID) { return (SetSignal(0UL, 0UL) & gone) != 0; }

/* Deliver the same close event as the window's close gadget. Only a window
 * whose port belongs to this child qualifies. The message remains owned by
 * this supervisor until ReplyMsg; never free/reuse it while queued. */
static BOOL label_is(const struct Gadget *g, const char *want)
{
    const char *s;
    if ((g->Flags & GFLG_LABELMASK) != GFLG_LABELITEXT || g->GadgetText == NULL) return FALSE;
    s = (const char *)g->GadgetText->IText;
    if (!s) return FALSE;
    while (*s && *want) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *want++) return FALSE;
    }
    return !*s && !*want;
}

static BOOL close_window(struct Process *child)
{
    struct Screen *s;
    struct Window *w;
    struct Gadget *g;
    ULONG lock, seen = 0, owned = 0, flags = 0;
    BOOL sent = FALSE;
    if (IntuitionBase == NULL) return FALSE;
    lock = LockIBase(0);
    Forbid();
    if (finished()) { Permit(); UnlockIBase(lock); return FALSE; }
    for (s = IntuitionBase->FirstScreen; s != NULL && !sent; s = s->NextScreen)
        for (w = s->FirstWindow; w != NULL && !sent; w = w->NextWindow)
        {
            seen++;
            if (w->UserPort != NULL && w->UserPort->mp_SigTask == (struct Task *)child)
            {
                owned++; flags = w->IDCMPFlags;
                close_message.ExecMessage.mn_ReplyPort = close_port;
                close_message.ExecMessage.mn_Length = sizeof(close_message);
                g = NULL;
                if (w->IDCMPFlags & IDCMP_CLOSEWINDOW)
                    close_message.Class = IDCMP_CLOSEWINDOW;
                else if (w->IDCMPFlags & IDCMP_VANILLAKEY)
                    close_message.Class = IDCMP_VANILLAKEY; /* Escape: cancel a modal requester. */
                else {
                    if (!(w->IDCMPFlags & IDCMP_GADGETUP)) continue;
                    for (g = w->FirstGadget; g != NULL; g = g->NextGadget)
                        if (label_is(g, "cancel") || label_is(g, "abort") ||
                            label_is(g, "abort install") || label_is(g, "quit")) break;
                    if (g == NULL) continue;
                    close_message.Class = IDCMP_GADGETUP;
                }
                close_message.IDCMPWindow = w;
                close_message.IAddress = (APTR)g;
                close_message.Code = g ? g->GadgetID :
                    (close_message.Class == IDCMP_VANILLAKEY ? 27 : 0);
                PutMsg(w->UserPort, (struct Message *)&close_message);
                sent = TRUE;
            }
        }
    Permit();
    UnlockIBase(lock);
    if (!sent) {
        LONG vals[3];
        vals[0] = seen; vals[1] = owned; vals[2] = flags;
        VFPrintf(Output(), (CONST_STRPTR)"cli_gui windows=%ld owned=%ld flags=%lx\n", (APTR)vals);
        Flush(Output());
    }
    return sent;
}

static VOID record(const char *id, const char *event, LONG rc, ULONG stack, ULONG broke)
{
    LONG vals[5];
    vals[0] = (LONG)id; vals[1] = (LONG)event; vals[2] = rc;
    vals[3] = (LONG)stack; vals[4] = (LONG)broke;
    VFPrintf(journal, (CONST_STRPTR)"%s\t%s\t%ld\t%lu\t%lu\n", (APTR)vals);
    Flush(journal);
}

/* A child that never reaches its exit hook still owns our seglist, buffers,
 * handles and callback. Preserve them and park: the host watchdog reaps the
 * entire isolated guest. Returning or UnLoadSeg here would create a UAF. */
static VOID park(const char *id, const char *reason)
{
    record(id, reason, 0, 0, 0);
    for (;;) Delay(50);
}

static LONG run_case(char *id, ULONG stack, char mode, ULONG seconds, const char *path, const char *args)
{
    BPTR seg, input, output;
    struct Process *child;
    ULONG i, waited, broke = 0;
    BOOL sent_close = FALSE, close_pending = FALSE;
    LONG rc;
    for (i = 0; i < sizeof(arguments) - 2 && args[i] != '\0'; i++) arguments[i] = args[i];
    if (args[i] != '\0') { record(id, "INFRA", 0, 0, 0); return 20; }
    arguments[i++] = '\n'; arguments[i] = '\0';
    input = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    output = Open((CONST_STRPTR)"DH0:cli-output.txt", MODE_NEWFILE);
    seg = LoadSeg((CONST_STRPTR)path);
    if (input == 0 || output == 0 || seg == 0)
    {
        record(id, "INFRA", IoErr(), 0, 0);
        if (input) Close(input);
        if (output) Close(output);
        if (seg) UnLoadSeg(seg);
        return 20;
    }
    child_rc = -999; child_stack = 0;
    SetSignal(0, gone);
    record(id, "START", 0, stack, 0);
    child = CreateNewProcTags(NP_Seglist, (Tag)seg, NP_FreeSeglist, (Tag)FALSE,
        NP_Cli, (Tag)TRUE, NP_Name, (Tag)path, NP_Arguments, (Tag)arguments,
        NP_StackSize, (Tag)stack, NP_Input, (Tag)input, NP_Output, (Tag)output,
        NP_Error, (Tag)output, NP_CloseInput, (Tag)FALSE, NP_CloseOutput, (Tag)FALSE,
        NP_CloseError, (Tag)FALSE, NP_ExitCode, (Tag)child_exit, TAG_DONE);
    if (child == NULL)
    {
        record(id, "INFRA", IoErr(), 0, 0);
        UnLoadSeg(seg); Close(input); Close(output);
        return 20;
    }
    /* Interrupt/GUI cases are expected to stay resident until stopped. Plain
     * exit cases must finish unaided; Ctrl-C never converts their hang to PASS. */
    for (waited = 0; waited < seconds * 50UL && !finished(); waited += 2)
        Delay(2);
    if (!finished())
    {
        if (mode == 'I') close_pending = sent_close = close_window(child);
        Forbid();
        if (!finished())
        {
            if (!sent_close) Signal((struct Task *)child, SIGBREAKF_CTRL_C);
            broke = 1;
        }
        if (sent_close) broke = 1;
        Permit();
        for (waited = 0; waited < 150UL && !finished(); waited += 2)
        {
            Delay(2);
            if (close_pending && GetMsg(close_port) != NULL) close_pending = FALSE;
            /* Cancel can open a second confirmation requester. Reuse our
             * message only after its reply, never while queued in a child. */
            if (sent_close && !close_pending && waited % 10UL == 0 && !finished())
                close_pending = close_window(child);
        }
    }
    if (!finished()) park(id, "TIMEOUT");
    if (close_pending)
    {
        /* The child must reply even if it exits immediately after reading. */
        if (GetMsg(close_port) == NULL) park(id, "UNREPLIED_CLOSE");
    }
    rc = child_rc;
    UnLoadSeg(seg); Close(input); Close(output);
    record(id, mode != 'I' && broke ? "LATE" : "END", rc, child_stack, broke);
    return 0;
}

static BOOL fixtures(VOID)
{
    static const char *names[] = {"RAM:cli-input", "RAM:cli-case", "RAM:cli-script", "RAM:cli-wireless.prefs"};
    static const char *contents[] = {"CLI fixture\n", "CLI fixture\n", "(exit \"CLI fixture\" (quiet))\n", "network={\n ssid=\"cli-test\"\n key_mgmt=NONE\n}\n"};
    ULONG i;
    for (i = 0; i < 4; i++) {
        BPTR f = Open((CONST_STRPTR)names[i], MODE_NEWFILE);
        if (!f) return FALSE;
        if (FPuts(f, (CONST_STRPTR)contents[i]) < 0) { Close(f); return FALSE; }
        Close(f);
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    BPTR cases;
    BYTE bit;
    LONG rc = 0;
    (VOID)argc; (VOID)argv;
    parent = FindTask(NULL);
    bit = AllocSignal(-1);
    if (bit < 0) return 20;
    gone = 1UL << bit;
    close_port = CreateMsgPort();
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    journal = Open((CONST_STRPTR)"DH0:cli-results.tsv", MODE_NEWFILE);
    cases = Open((CONST_STRPTR)"DH0:cli-cases.tsv", MODE_OLDFILE);
    if (journal == 0 || cases == 0 || close_port == NULL) rc = 20;
    else
    {
        while (FGets(cases, (STRPTR)line, sizeof(line)) != NULL)
        {
            char *size, *mode, *seconds, *path, *args;
            ULONG n;
            for (n = 0; line[n] != '\0'; n++) ;
            if (n == 0 || line[n-1] != '\n') { rc = 20; break; }
            size = field(line);
            if (size == NULL || (mode = field(size)) == NULL ||
                (seconds = field(mode)) == NULL || (path = field(seconds)) == NULL || (args = field(path)) == NULL)
            { rc = 20; break; }
            for (n = 0; args[n] != '\0'; n++) ;
            while (n > 0 && (args[n-1] == '\n' || args[n-1] == '\r')) args[--n] = '\0';
            if ((number(size) != 4096 && number(size) != 8192) ||
                (mode[0] != 'I' && mode[0] != 'E') ||
                number(seconds) < 1 || number(seconds) > 120) { rc = 20; break; }
            if (!fixtures()) { record(line, "INFRA", IoErr(), 0, 0); rc = 20; break; }
            if (run_case(line, number(size), mode[0], number(seconds), path, args) != 0) rc = 20;
        }
        record("SUITE", "DONE", rc, 0, 0);
    }
    if (cases) Close(cases);
    if (journal) Close(journal);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (close_port) DeleteMsgPort(close_port);
    FreeSignal(bit);
    return (int)rc;
}
