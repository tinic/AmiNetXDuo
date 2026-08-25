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
    # --gc-sections is required here for correctness, not for size.
    #
    # These two are the only binaries here that link the toolchain's crt0.o
    # and libc.a, and those carry DWARF.  ld's amiga backend gives every
    # .debug_* section a loadable DATA hunk, and an executable that has one
    # comes out with no HUNK_RELOC32 at all.  Measured on the same objects:
    #
    #   as linked        12 hunks, 0 relocations
    #   --gc-sections     3 hunks, 1152 relocations
    #
    # LoadSeg() then relocates nothing, so every absolute address keeps its
    # link-time value and the program jumps into low memory before reaching
    # main().  This rebooted the guest twelve seconds into every run that
    # staged Profile, with no output of its own, so it looked like a fault in
    # the program under test.  -Wl,-s and -Wl,--strip-debug do not prevent it;
    # both leave the twelve hunks.
    #
    # src/tools/ passes this already, which is why nothing else in the tree
    # has ever shown it.
    target_link_options(${name} PRIVATE -Wl,--gc-sections)
    target_link_options(${name} PRIVATE
        "-Wl,-Map=$<TARGET_FILE_DIR:${name}>/${name}.map")

    # The one exception to the self-containment above, and it is compiler
    # runtime rather than stack: -u pulls the archive before LTRANS invents the
    # 64-bit libcall, which is the only moment it can beat libgcc's copy to it.
    if(TARGET aminetxduo_m68k_rt)
        target_link_libraries(${name} PRIVATE aminetxduo_m68k_rt)
        target_link_options(${name} PRIVATE -Wl,-u,_ami_rt_cpu_select)
    endif()
    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${name} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()
    # Symbols, after the map is written and before the size is printed, as in
    # src/tools/.  Both binaries ship in Developer/Profile/ and neither reads
    # its own HUNK_SYMBOL: profspin takes its kernel ranges from link-time
    # addresses the compiler put in the code, and the report is built on the
    # host from the map beside this binary.
    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        # Through cmake/strip-if-readable.cmake, not straight at strip: these
        # two are the only binaries here that carry a HUNK_DEBUG block, and
        # binutils 2.46 will not read one.  See that file for the measurements.
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                    "-DSTRIP=${CMAKE_STRIP}"
                    "-DBINARY=$<TARGET_FILE:${name}>"
                    -P "${CMAKE_SOURCE_DIR}/cmake/strip-if-readable.cmake"
            VERBATIM)
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
