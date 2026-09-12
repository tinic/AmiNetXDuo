/*
 * AmiNetXDuo, what a NetX Duo or ThreadX status means to the caller.
 *
 * Every one of these calls returns a status, and 141 of them across src/ and
 * port/ were thrown away with a (VOID) cast.  (Codex counted 117; the number
 * is what `(VOID)' in front of an nx_/tx_ call actually measures.)  Most were
 * legitimate -- a delete on a teardown path has nowhere to report to -- but
 * they looked exactly like the ones that were not, and three of the ones that
 * were not mattered:
 *
 *   a static interface whose address could not be applied was recorded as
 *   resolved anyway, so the stack came up believing in an address no
 *   interface had taken
 *
 *   a DHCP client whose state-change callback failed to register would have
 *   taken a lease that nothing noticed, with every waiter still waiting
 *
 *   a route whose interface changed was deleted and re-added, and NetX Duo's
 *   add updates only the next hop of an entry it finds by (dest, mask) -- so
 *   a delete that failed reported success with the route still on the old
 *   interface
 *
 * So each call says which of seven things it is, at the call site.  The
 * first two are handling and need no macro; the rest are a discard with a
 * reason, and the reason is a CLAIM -- about the callee, the out-parameters,
 * or the statuses converging -- so a reader can go and check it:
 *
 *   Required   the failure changes what happens next.  An ordinary
 *              `if (status != NX_SUCCESS)' that reports, returns or aborts.
 *              No macro: the handling IS the point.
 *
 *   Optional   best effort.  Called, checked, and reported once if it fails,
 *              naming what was lost rather than the status alone.
 *
 *              WHERE IT IS REPORTED MATTERS.  AMINETXDUO_LOG is OFF in every
 *              shipped build, so AMI_ERROR() and AMI_WARN() are
 *              do { if (0) ... } while (0) there and a diagnostic line is not
 *              a report anyone can see.  A failure a user can act on takes an
 *              event as well -- ami_event(), which costs no image strings and
 *              which ShowNetStatus renders.  A failure only a developer can
 *              act on takes the diagnostic line alone, and says so.
 *
 *   Expected   only specific statuses are acceptable; an ordinary conditional
 *              enumerating them, so a NEW status is not silently accepted.
 *
 *   Cleanup    teardown or rollback, where there is nothing to report to and
 *              nothing to do about it.  AMI_NX_CLEANUP() below, which says
 *              that out loud instead of leaving a bare cast that could be
 *              either.
 *
 *   OnlySuccess the callee cannot return anything else from this call site.
 *              AMI_NX_ONLY_SUCCESS().  _tx_thread_suspend(),
 *              _tx_thread_resume() and _tx_thread_terminate() have one
 *              `return(TX_SUCCESS)' between them and no other exit, so a
 *              conditional on them is dead code on the scheduler's own path.
 *              It is a claim about the callee, so it is checkable: grep the
 *              vendored source for a second return.
 *
 *   ByOutput   the call is judged by its out-parameters, which the code below
 *              tests, and the status adds nothing to that test.
 *              AMI_NX_BY_OUTPUT().  _nx_ip_route_find() zeroes *next_hop
 *              before it can fail, so `next_hop == 0' is the same answer as
 *              the status and is the one the caller needs anyway.  The comment
 *              at the call site names WHICH output carries it, because a
 *              reader cannot otherwise tell this from a discard.
 *
 *   EitherWay  more than one status is possible and the caller does the same
 *              thing for every one of them.  AMI_NX_EITHER_WAY(), and the
 *              comment beside it says what the possibilities are and why they
 *              converge -- `tx_semaphore_get(..., TX_NO_WAIT)' draining a
 *              count is TX_SUCCESS or TX_NO_INSTANCE and the queue above it is
 *              the truth either way.  An `if' enumerating statuses whose arms
 *              are identical is dead code, and on this tree dead code is RAM.
 *
 * tools/check-nx-status.sh holds the line: a bare (VOID) on one of these calls
 * is an unreviewed discard and fails the build.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NXSTATUS_H
#define AMINETXDUO_NXSTATUS_H

/*
 * The result is deliberately not looked at: teardown, or a rollback that has
 * already failed once.  Reads as an intention rather than an omission.
 */
#define AMI_NX_CLEANUP(call)    ((VOID)(call))

/*
 * There is one outcome.  Not "the failure does not matter" -- there is no
 * failure to have.  Separate from Cleanup because Cleanup describes the
 * CALLER's position and this describes the CALLEE's, and a reader who cannot
 * tell them apart is back where the bare cast left them.
 */
#define AMI_NX_ONLY_SUCCESS(call)   ((VOID)(call))

/*
 * The answer is in the out-parameters and the code below reads them.  Say
 * which one in a comment beside the call: "judged by X" is checkable, "the
 * status is not needed" is not.
 */
#define AMI_NX_BY_OUTPUT(call)      ((VOID)(call))

/*
 * Every status this can return leads to the same next line.  Say which
 * statuses in a comment: the claim is that they converge, and a claim nobody
 * can check is the bare cast again under a longer name.
 */
#define AMI_NX_EITHER_WAY(call)     ((VOID)(call))

#endif /* AMINETXDUO_NXSTATUS_H */
