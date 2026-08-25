#!/usr/bin/env bash
#
# WHAT A USER SEES WHEN BRING-UP FAILS, graded.
#
#   . "$ROOT/tests/tools/bringupfail-verdict.sh"
#   bringupfail_verdict <transcript> <cause>
#
# WHY THIS EXISTS
#
#   Several harnesses in this tree assert the wording of a SUCCESS -- what
#   ShowNetStatus prints for an interface that came up, what the verdict tools
#   print when they pass.  Not one asserted what a user is told when bring-up
#   FAILS.  That is the half a user actually reads: nobody studies the output
#   of a machine that worked.
#
#   With nothing grading it, this survived in a shipping build:
#
#       CONFIGURE line.  Check the debug log for what failed
#
#   AMINETXDUO_LOG is OFF in every drawer that ships
#   (tools/check-no-diag-strings.sh), so the debug log a user is being sent to
#   read cannot be produced by the binary that just told them to read it.  The
#   advice is not merely useless, it costs the reader the evening they spend
#   looking for the file.  It survived because no arm ever failed a bring-up
#   on purpose and read the sentence back.
#
# THE CONTRACT, and it is two clauses
#
#   1. THE FIRST LINE NAMES THE FAILING OPERATION AND ITS CODE.  First,
#      because a user reads one line before deciding what to do, and because
#      an error whose first line is scene-setting buries the fact under prose.
#      The operation, because "it did not work" is not a diagnosis and
#      "opening a2065.device unit 0" is.  The code, because it is the only
#      part a user can carry to a search or a bug report unchanged, and
#      because a decoded string alone ("Input/output error") loses which of
#      several conditions produced it.
#
#      The house style is already this, where it is right:
#        AddNetInterface: could not open a2065.device unit 0 (error 32)
#
#      BOTH HALVES HAVE A SECOND ACCEPTABLE FORM, because one path in this
#      tree already does better than the house style rather than worse.  A
#      configuration fault prints no verb and no number:
#        DEVS:NetInterfaces/eth0, line 4:
#            ADDRESS cannot be '300.1.1.1'
#      The PATH names what failed as precisely as a verb would, and the LINE
#      is a locator the user can act on more directly than an error code.
#      Both are accepted.  What is never accepted is a refusal carrying
#      neither.
#
#   2. NO OUTPUT ANYWHERE SENDS THE USER TO A LOG.  Not the first line, not
#      the hints below it.  A shipping build has no debug log, so any sentence
#      that names one is a dead end by construction.  This clause is over the
#      WHOLE transcript, not the first line, because the offending sentence
#      was in the hint block.
#
# WHY IT IS A FILE OF ITS OWN, sourced without an emulator: the same reason
# tests/tools/addifup-verdict.sh is.  The live arm
# (tests/tools/run-bringupfail.sh) needs a ROM, a driver and five boots, so
# nothing else could tell an assertion that fires from one that cannot, and
# tests/tools/bringupfail-verdict-selftest.sh drives this against transcripts
# for causes that are awkward to produce on demand -- out of memory, in
# particular.
#
# Output is key=value on stdout, the return code is the verdict.
#
# SPDX-License-Identifier: MIT

# ------------------------------------------------------------- vocabulary --
#
# THE OPERATION WORDS.  A refusal has to say what was being ATTEMPTED, and
# these are the verbs and objects the bring-up path actually has.  Kept as a
# pattern rather than a prose rule so the assertion is the same on every cause.
BFV_OPERATIONS='open|opening|read|reading|add|adding|start|starting|configure|configuring|attach|attaching|allocat'

# ...OR THE OBJECT, NAMED CONCRETELY.  Not every refusal has a verb, and the
# best one in this tree has none: a bad ADDRESS is reported as
#
#     DEVS:NetInterfaces/badaddr, line 4:
#         ADDRESS cannot be '300.1.1.1'
#
# which says what failed more precisely than any verb would, by pointing at
# the file and the line.  Grading that as "names no operation" marked down the
# single best message on the bring-up path, so a concrete object -- a
# DEVS: path, or a driver name -- counts as naming what failed.
BFV_OBJECT='DEVS:[^[:space:]]+|[A-Za-z0-9_-]+\.device'

# WHAT COUNTS AS A CODE.  Any of:
#   (error 32)   (-1)   (0x20)   error 32   code 32   IoErr 32   line 4
# A bare decoded string is NOT enough -- see clause 1 -- but a decoded string
# BESIDE a number is, which is the best of both and what the house style does.
#
# A FILE AND A LINE COUNT.  The point of the code is that the user gets
# something they can carry unchanged: to a search, to a bug report, or straight
# to the thing they have to edit.  `line 4' of a named file does that at least
# as well as `(error 32)', and for a configuration fault it does it better.  A
# refusal with neither has handed the user nothing to hold.
BFV_CODE='\(-?[0-9]+\)|\(0x[0-9a-fA-F]+\)|\((error|code|IoErr|rc)[[:space:]]+-?[0-9]+\)|(error|code|IoErr|rc)[[:space:]]+-?[0-9]+|0x[0-9a-fA-F]{2,}|line[[:space:]]+[0-9]+'

# THE SENTENCE THAT MUST NOT BE THERE.  Deliberately broad on "log": the point
# is that a shipping build produces none of them, so "the debug log", "the
# logfile", "logging" and "enable the log" are all the same dead end.
#
# NOT a bare match on the letters l-o-g: "login", "logical" and "dialog" are
# words, and a check that flagged them would be turned off within a week.  The
# words that mean the noun are what is listed.
BFV_LOGADVICE='(debug|the|a|error|serial|trace)[[:space:]]+log([[:space:]]|file|s|\.|,|$)|log[[:space:]]*file|check[[:space:]]+the[[:space:]]+log|enable[[:space:]]+logging|turn[[:space:]]+on[[:space:]]+logging|see[[:space:]]+the[[:space:]]+log'

# THE FIRST LINE OF THE REFUSAL, WHICH IS NOT THE FIRST LINE OF THE OUTPUT.
#
# AddNetInterface echoes what it is about to do before it does it:
#
#     nodev: nosuchcard.device unit 0
#     nodev: starting the network...
#     AddNetInterface: nodev was not added to the running network
#       There is no nosuchcard.device on this machine.
#
# The first two are progress, addressed by the INTERFACE's name, and a tool is
# entitled to print them.  Grading them as "the first line" reported every
# cause as naming no operation, which is both wrong and useless: it would put
# a red beside a message that is doing its job.
#
# So the refusal is the first line prefixed with the COMMAND's name, which is
# what tool_error() writes (src/tools/tool_util.c:79-92) and what
# tool_printf() status lines never do.  Pass the command name when it is
# known; the default is a CamelCase prefix, because every command in this tree
# is CamelCase and every interface name in it is not.
bfv_first_line() { # transcript [tool]
    local tool="${2:-[A-Z][A-Za-z0-9]*}" line

    line=$(tr -d '\r' < "$1" 2>/dev/null | grep -aE "^${tool}:[[:space:]]" | head -1)
    if [ -n "$line" ]; then
        printf '%s\n' "$line"
        return 0
    fi

    # NOT EVERY REFUSAL IS tool_error()'s.  A configuration fault is reported
    # as a "Problems in the configuration:" block whose lines are indented and
    # whose first useful one names the file and the line.  There is no command
    # prefix anywhere in it, so looking only for one reported the best message
    # on this path as SILENCE -- which would have been this grader inventing a
    # defect, the exact failure mode it exists to prevent.
    tr -d '\r' < "$1" 2>/dev/null |
        grep -aE '^[[:space:]]*DEVS:[^[:space:]]*,?[[:space:]]*(line[[:space:]]+[0-9]+)?:' |
        head -1 |
        sed 's/^[[:space:]]*//'
}

# bringupfail_verdict TRANSCRIPT CAUSE [TOOL] -> 0 pass, 1 fail
bringupfail_verdict() {
    local report="$1" cause="${2:-unnamed}" tool="${3:-}"
    local failed=0 first offender

    _bf_fail() { echo "bringupfail_fail=$1"; failed=1; }

    echo "bringupfail_cause=$cause"

    if [ ! -s "$report" ]; then
        echo "bringupfail_report=missing"
        _bf_fail no_transcript
        echo "bringupfail_result=FAIL"
        return 1
    fi

    # ------------------------------------------------ clause 1, first line --

    first=$(bfv_first_line "$report" "$tool")
    if [ -z "$first" ]; then
        echo "bringupfail_first_line=none"
        # A bring-up that failed and said NOTHING is the worst case of all and
        # is named separately: it is not a wording defect, it is silence.
        _bf_fail no_message_at_all
        echo "bringupfail_result=FAIL"
        return 1
    fi
    echo "bringupfail_first_line=$first"

    # THE MESSAGE, NOT THE PREFIX.  `AddNetInterface: eth0 failed (error 32)'
    # names no operation, and matching the whole line graded it as though it
    # did: the tool's own name contains "Add".  Every command on this path is
    # named for the thing it does -- AddNetInterface, ConfigureNetInterface,
    # NetSetup -- so the prefix will always contain an operation word and can
    # never be evidence of one.  Everything up to the first ": " goes.
    local msg="${first#*: }"
    echo "bringupfail_message=$msg"

    if printf '%s\n' "$msg" | grep -qiE "$BFV_OPERATIONS" ||
       printf '%s\n' "$first" | grep -qE "$BFV_OBJECT"; then
        echo "bringupfail_names_operation=yes"
    else
        echo "bringupfail_names_operation=no"
        _bf_fail first_line_names_no_operation
    fi

    if printf '%s\n' "$first" | grep -qE "$BFV_CODE"; then
        echo "bringupfail_names_code=yes"
    else
        echo "bringupfail_names_code=no"
        _bf_fail first_line_carries_no_code
    fi

    # ------------------------------------------- clause 2, the whole thing --

    offender=$(tr -d '\r' < "$report" | grep -inE "$BFV_LOGADVICE" | head -3)
    if [ -z "$offender" ]; then
        echo "bringupfail_log_advice=absent"
    else
        echo "bringupfail_log_advice=present"
        printf '%s\n' "$offender" |
            while IFS= read -r l; do echo "bringupfail_log_line=$l"; done
        _bf_fail sends_user_to_a_log_that_shipping_builds_cannot_produce
    fi

    if [ "$failed" = 0 ]; then
        echo "bringupfail_result=PASS"
        return 0
    fi
    echo "bringupfail_result=FAIL"
    return 1
}
