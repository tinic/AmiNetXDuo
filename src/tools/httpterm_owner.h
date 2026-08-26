/*
 * Whether a DOS packet belongs to the console session httpterm.c is running.
 *
 * Separate from httpterm.c, and in plain pointers, because httpterm.c reaches
 * proto/dos.h and compiles nowhere but the target -- while getting this wrong
 * in the strict direction refuses an editor its raw input, and getting it
 * wrong in the loose direction hands an unrelated process someone's console.
 * The second is worse, so the decision is asserted on the host instead of
 * reasoned about: src/tools/test/test_httpterm.c drives it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPTERM_OWNER_H
#define AMINETXDUO_HTTPTERM_OWNER_H

typedef struct TermOwner
{
    int         to_Active;      /* a Shell has been started                  */
    const void *to_Port;        /* the handler port, ONE for every session   */
    const void *to_BreakPort;   /* ACTION_CHANGE_SIGNAL's, NULL until it comes */
    const void *to_ShellTask;   /* the task this session has adopted         */
} TermOwner;

typedef struct TermCallerId
{
    const void *tc_Port;        /* dp_Port                                   */
    const void *tc_Task;        /* dp_Port->mp_SigTask, NULL when there is none */
    const void *tc_Console;     /* its pr_ConsoleTask, NULL when not a Process */
    int         tc_Stale;       /* it is an abandoned session's live runner  */
} TermCallerId;

/*
 * Non-zero when the caller may drive this session.  *adopt, when it comes back
 * non-NULL, is the task the session must record as its own.
 */
static int term_owner_admits(const TermOwner *s, const TermCallerId *c,
                             const void **adopt)
{
    *adopt = 0;

    if (s == 0 || c == 0 || !s->to_Active || c->tc_Port == 0)
        return 0;

    /* A Shell whose session was let go of keeps this port as its console, so
       pr_ConsoleTask alone would still admit it. */
    if (c->tc_Stale)
        return 0;

    /* dos.library's own answer, and only this generation's: still first. */
    if (s->to_BreakPort != 0 && c->tc_Port == s->to_BreakPort)
        return 1;

    /* THE ROW THIS FILE EXISTS FOR.  A process the Shell spawned has its own
       pr_MsgPort and matches neither port here; pr_ConsoleTask is what
       dos.library resolves Open("*") through, so naming this port is what
       makes a caller a member of this session rather than a stranger. */
    if (c->tc_Console != 0)
        return (c->tc_Console == s->to_Port) ? 1 : 0;

    /* No pr_ConsoleTask to go on -- not a Process, or a Process with no
       console.  Fall back to the identity the session already had. */
    if (s->to_BreakPort != 0)
        return (c->tc_Port == s->to_BreakPort) ? 1 : 0;

    if (s->to_ShellTask == 0)
    {
        *adopt = c->tc_Task;
        return (c->tc_Task != 0) ? 1 : 0;
    }

    return (c->tc_Task == s->to_ShellTask) ? 1 : 0;
}

#endif /* AMINETXDUO_HTTPTERM_OWNER_H */
