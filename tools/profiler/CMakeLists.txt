# Profile, a sampling profiler for AmigaOS/m68k.
#
# Self-contained on purpose.  Nothing here links anything from src/, port/ or
# third_party/, and nothing in the stack knows it exists: this directory is
# meant to be lifted out into its own repository once it has settled, and the
# way to keep that cheap is to never let a dependency grow back.
#
#   cmake --build <build> --parallel --target Profile profspin
#
# Prove it before believing it, on more than one CPU, because the exception
# frame is what is in question.  tools/profiler/selftest.sh is the whole loop
# run, resolve, check containment and proportionality, in one command:
#
#   AMINETXDUO_KICKSTART=<a 3.1 ROM>   tools/profiler/selftest.sh -b build/p20
#   AMINETXDUO_KICKSTART=<a 2.04 ROM>  tools/profiler/selftest.sh -b build/p00 -m A500
#
# The ROM has to be one the model can boot.  An A1200 ROM in an A500 profile
# does not fail as a bad ROM: FS-UAE dies host-side before the guest runs, and
# that looks exactly like the emulator being broken for that machine.
#
# SPDX-License-Identifier: MIT

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# The core.  prof_vector.S needs no arch flags, MOVEC is hand-encoded there
# precisely so it assembles for the 68000 profile too, but pass them anyway
# so the object matches the rest of the build.
add_library(profile_core STATIC prof.c prof_vector.S)
target_include_directories(profile_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(profile_core PRIVATE -Wall -Wextra)
set_source_files_properties(prof_vector.S PROPERTIES
    COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(profile_core PUBLIC "${AMIGA_NDK_INCLUDE}")
endif()

# A linker map for every binary that might be profiled, this tool's own
# included.  tools/profiler/profreport.py needs it: the statics the linker
# drops from HUNK_SYMBOL are recoverable only from the map's placement of each
# object plus nm on that object.
function(profile_binary name)
    target_link_options(${name} PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:${name}>/${name}.map")
    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${name} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()
    if(AMIGA_SIZE)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${name}>"
            COMMENT "${name} size")
    endif()
endfunction()

# The tool.
add_executable(Profile profile.c)
target_link_libraries(Profile PRIVATE profile_core)
target_compile_options(Profile PRIVATE -Wall -Wextra)
profile_binary(Profile)

# An ordinary program to point it at.  It links NOTHING from here, that is
# the point of it: the profiler has to work on a program that was not built
# for it, and profspin is the one whose exact byte ranges we happen to know.
add_executable(profspin profspin.c profspin.S)
target_compile_options(profspin PRIVATE -Wall -Wextra)
set_source_files_properties(profspin.S PROPERTIES
    COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
profile_binary(profspin)
