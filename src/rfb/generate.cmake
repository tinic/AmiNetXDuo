# One part of the synthetic captures, for the round-trip shards to share.
#
# Its own ctest cases, registered as FIXTURES_SETUP, because generating them
# inside every shard would pay for them sixty times over.  Split into parts
# because as a single case it was the slowest test in the tree once the round
# trip itself had been sharded: 4.50 s under the sanitizer on the machine that
# wrote this and 6.85 s on the CI runner, against a ten-second budget.  The
# cost is CPU and not disk -- this host writes the whole 251 MB in 0.11 s.
#
# rfbgen --part I/N writes the captures whose position is I modulo N, and the
# parts together are byte for byte what one whole run writes.
#
# SPDX-License-Identifier: MIT

file(MAKE_DIRECTORY "${DIR}")

execute_process(COMMAND "${GEN}" --part "${PART}/${PARTS}" "${DIR}"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "rfbgen failed: ${rc}")
endif()

file(GLOB seqs "${DIR}/*.pfs")
if(seqs STREQUAL "")
    message(FATAL_ERROR "rfbgen produced nothing in ${DIR}")
endif()

list(LENGTH seqs n)
message("rfbgen part ${PART}/${PARTS}: ${n} captures now in ${DIR}")
