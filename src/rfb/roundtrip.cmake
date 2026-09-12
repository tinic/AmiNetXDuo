# One shard of the round trip: encode and decode every frame of every capture
# in this shard, at TILE, in LAYOUT, under every strategy.  rfbbench exits
# non-zero the moment a decoded frame differs from its input.
#
# WHY IT IS SHARDED.  It used to be one ctest case over every capture, every
# tile size and both layouts, and that case WAS the host test suite: 29.27 s of
# a 32.52 s run on the CI runner and 93.51 s of 98.66 s under the sanitizer,
# against 0.70 s for the next slowest of the other 137 tests.  No test here is
# to run longer than ten seconds -- tools/check-test-duration.sh holds that --
# and a single case cannot: one capture at one tile size under the sanitizer
# is four seconds on its own here and twice that on the runner.  So the sweep
# is cut along FOUR axes -- tile size, source layout, a slice of the captures
# and a slice of the strategy table -- and ctest's own -j runs the cases.
# Every axis is positional, so nothing has to name a capture or a strategy.
#
# SPDX-License-Identifier: MIT

file(GLOB seqs "${DIR}/*.pfs")
if(seqs STREQUAL "")
    message(FATAL_ERROR "no captures in ${DIR}: the rfb_captures fixture did "
                        "not run, or ran somewhere else")
endif()

# GLOB returns them sorted by name, so shard I of N is the same set of
# captures on every host and in every arm.
#
# ORDERING BY FILE SIZE WAS TRIED AND IS WRONG.  It looked like a way to
# spread the expensive captures, and bytes are not the encoder's work: idle8
# is 28 MB and a second, full_rgb is 12 MB and four.  The shards it produced
# were no more even, and the worst of them was 11.59 s on the CI runner
# against 5.35 s on the machine that chose the split.  The budget is held by
# bounding what a case CAN cost -- one tile, one slice of the captures, one
# half of the strategy table -- not by predicting what it will.
list(SORT seqs)

if(LAYOUT STREQUAL "interleaved")
    set(layout_arg --interleaved)
else()
    set(layout_arg)
endif()

execute_process(COMMAND "${BENCH}" --reps 1 --tiles "${TILE}"
                        --shard "${SHARD}/${SHARDS}"
                        --strats "${SGROUP}/${SGROUPS}" ${layout_arg}
                        ${seqs}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "round trip failed: ${rc}\n${err}")
endif()
string(REGEX MATCHALL "rt_fail=[1-9][0-9]*" bad "${out}")
if(NOT bad STREQUAL "")
    message(FATAL_ERROR "round trip mismatches: ${bad}")
endif()
