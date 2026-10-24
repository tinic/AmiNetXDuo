# forbidrec, every Forbid()/Disable() hold timed; frload, the Amiga half of
# its workload; hush, digital silence through AHI for the USB audio path.
# Instruments: built for every cross configuration so the m68k warning gate
# covers them, shipped nowhere.  run-amiberry.sh is the gate;
# run-a1200.sh is the measurement.
#
# SPDX-License-Identifier: MIT

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(forbidrec forbidrec.c forbidrec_stub.S
               "${PROJECT_SOURCE_DIR}/src/netdev/netdev_dtree.c")
target_include_directories(forbidrec PRIVATE "${PROJECT_SOURCE_DIR}/src/netdev")
set_source_files_properties(forbidrec_stub.S PROPERTIES
    COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")

add_executable(frload frload.c)
add_executable(hush hush.c)

foreach(t forbidrec frload hush)
    # libnix, as every on-Amiga harness here links: the NDK's bsdsocket
    # headers agree with its sys/ headers and with nothing else.
    target_compile_options(${t} PRIVATE -Wall -Wextra -noixemul)
    target_link_options(${t} PRIVATE -noixemul)
    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${t} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()
    # As tools/profiler: the map is what names the self-test's PCs, and
    # --gc-sections keeps the toolchain's DWARF out of loadable hunks.
    target_link_options(${t} PRIVATE -Wl,--gc-sections
                        "-Wl,-Map=$<TARGET_FILE_DIR:${t}>/${t}.map")
    if(TARGET aminetxduo_m68k_rt)
        target_link_libraries(${t} PRIVATE aminetxduo_m68k_rt)
        target_link_options(${t} PRIVATE -Wl,-u,_ami_rt_cpu_select)
    endif()
endforeach()
