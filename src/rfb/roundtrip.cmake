# Generate the synthetic captures, then encode and decode every frame of every
# one of them under every strategy and every tile size the sweep uses.
# rfbbench exits non-zero the moment a decoded frame differs from its input.
#
# SPDX-License-Identifier: MIT

file(MAKE_DIRECTORY "${DIR}")

execute_process(COMMAND "${GEN}" "${DIR}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "rfbgen failed: ${rc}")
endif()

file(GLOB seqs "${DIR}/*.pfs")
if(seqs STREQUAL "")
    message(FATAL_ERROR "rfbgen produced nothing in ${DIR}")
endif()

execute_process(COMMAND "${BENCH}" --reps 1 --tiles 8x8,16x8,32x16 ${seqs}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "round trip failed: ${rc}\n${err}")
endif()

string(REGEX MATCHALL "rt_fail=[1-9][0-9]*" bad "${out}")
if(NOT bad STREQUAL "")
    message(FATAL_ERROR "round trip mismatches: ${bad}")
endif()
