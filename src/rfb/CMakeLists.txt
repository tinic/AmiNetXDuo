# Remote-framebuffer frame encoder.
#
# rfb_encode.c is plain C99 with no AmigaOS calls and no allocation in the
# per-frame path, so the same source is what the Amiga side links and what the
# host measurement CLI links.  Everything platform-specific -- grabbing the
# BitMap, owning the socket -- lives in the caller.
#
# Host tools:
#
#   cc -std=c99 -O2 -Wall -Wextra -Iinclude \
#      src/rfb/host/rfbbench.c src/rfb/rfb_encode.c -o rfbbench
#   cc -std=c99 -O2 -Wall -Wextra src/rfb/host/rfbgen.c -o rfbgen
#   rfbgen <dir> && rfbbench <dir>/*.pfs

add_library(aminetxduo_rfb STATIC rfb_encode.c rfb_words.c)

target_include_directories(aminetxduo_rfb
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# -ffunction-sections so a Shell command that only encodes does not carry the
# scroll detector; -fno-optimize-sibling-calls travels with it, always, for the
# 32-bit PC-relative branch this toolchain mis-resolves out of a cross-section
# tail call (docs/RESEARCH.md 25).
target_compile_options(aminetxduo_rfb PRIVATE
    -ffunction-sections -fno-optimize-sibling-calls -Wall -Wextra)

if(NOT CMAKE_CROSSCOMPILING)
    # rfb_encode.c for rfb_pal_colours(): how long the palette in front of the
    # frames is per format, which is what places every frame in the file.
    add_executable(rfbgen host/rfbgen.c rfb_encode.c)
    target_include_directories(rfbgen PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(rfbgen PRIVATE -Wall -Wextra)
    set_target_properties(rfbgen PROPERTIES C_STANDARD 99)

    add_executable(rfbbench host/rfbbench.c rfb_encode.c)
    target_include_directories(rfbbench PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(rfbbench PRIVATE -Wall -Wextra)
    set_target_properties(rfbbench PROPERTIES C_STANDARD 99)

    # --deflate prices an entropy coder on top of the encoder's output, which
    # is how the decision not to have one stays a measurement.  Optional: the
    # bench builds and every other arm works without zlib.
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_compile_definitions(rfbbench PRIVATE RFBBENCH_ZLIB)
        target_link_libraries(rfbbench PRIVATE ZLIB::ZLIB)
    endif()

    # The control words against the spellings the browser reads.  Host-side
    # for the reason rfb_words.c is its own file: the words are what the two
    # ends have to agree on, and they can be checked with no Amiga in the room.
    # rfb_encode.c comes along for rfb_pal_colours(): how long a `pal` is per
    # format is the one number both ends size a buffer from without having
    # seen a frame, so it is checked beside the words that carry it.
    add_executable(rfbwords host/rfbwords.c rfb_words.c rfb_encode.c)
    target_include_directories(rfbwords PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(rfbwords PRIVATE -Wall -Wextra)
    set_target_properties(rfbwords PROPERTIES C_STANDARD 99)

    include(CTest)
    add_test(NAME rfb_words COMMAND rfbwords)

    # rfbbench decodes every frame it encodes and compares it with the frame
    # that went in, for every strategy, and exits non-zero on a mismatch.  A
    # compression number from an encoder nobody round-tripped is worthless, so
    # the round trip is the test.
    #
    # SHARDED, and see roundtrip.cmake for why: as one case it was 90% of the
    # host test run and 95% of the sanitizer one, and no test here may run
    # longer than ten seconds.  The axes are the ones the sweep already had --
    # tile size, source layout, and a slice of the captures -- so the union of
    # the cases is the sweep that was there before, not a smaller one.
    #
    # Ten slices of the captures and four of the strategy table.
    #
    # Ten slices alone left the worst case at 11.59 s on the CI runner -- an
    # uneven slice, and no ordering fixes that, because what a capture costs
    # is not predictable from anything visible before it is encoded (see
    # roundtrip.cmake).  The strategy axis bounds what a case CAN cost instead
    # of guessing what it will: a quarter of a table of sixteen, whichever
    # captures land in the slice.
    #
    # FOUR AND NOT TWO, because the budget has to hold on a machine this one
    # cannot measure.  The runner is 2.17x this host on the encode cases and
    # 3.0x on the generator, so the margin is set against 3x: a case has to
    # come in under 3.3 s here to be trusted at ten there.  Two slices left
    # the worst at 6.12 s here, which is 13 s there -- the same mistake as
    # the size ordering, made against a different number.
    include(CTest)
    set(RFB_PFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/pfs")
    set(RFB_SHARDS 10)
    set(RFB_SGROUPS 4)

    # Four parts, for the reason generate.cmake gives.  ctest runs every
    # FIXTURES_SETUP case of a fixture before any case that requires it, so
    # the round trip still sees the whole corpus.
    set(RFB_GEN_PARTS 4)
    math(EXPR _lastpart "${RFB_GEN_PARTS} - 1")
    foreach(part RANGE ${_lastpart})
        add_test(NAME rfb_captures_${part}
                 COMMAND "${CMAKE_COMMAND}"
                         "-DGEN=$<TARGET_FILE:rfbgen>"
                         "-DDIR=${RFB_PFS_DIR}"
                         "-DPART=${part}"
                         "-DPARTS=${RFB_GEN_PARTS}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/generate.cmake")
        set_tests_properties(rfb_captures_${part}
                             PROPERTIES FIXTURES_SETUP rfb_captures)
    endforeach()

    math(EXPR _lastshard "${RFB_SHARDS} - 1")
    math(EXPR _lastgroup "${RFB_SGROUPS} - 1")
    foreach(layout plane interleaved)
        foreach(tile 8x8 16x8 32x16)
            foreach(shard RANGE ${_lastshard})
                foreach(sgroup RANGE ${_lastgroup})
                    set(_n rfb_roundtrip_${layout}_${tile}_${shard}_${sgroup})
                    add_test(NAME ${_n}
                             COMMAND "${CMAKE_COMMAND}"
                                     "-DBENCH=$<TARGET_FILE:rfbbench>"
                                     "-DDIR=${RFB_PFS_DIR}"
                                     "-DTILE=${tile}"
                                     "-DLAYOUT=${layout}"
                                     "-DSHARD=${shard}"
                                     "-DSHARDS=${RFB_SHARDS}"
                                     "-DSGROUP=${sgroup}"
                                     "-DSGROUPS=${RFB_SGROUPS}"
                                     -P "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.cmake")
                    set_tests_properties(${_n} PROPERTIES
                                         FIXTURES_REQUIRED rfb_captures)
                endforeach()
            endforeach()
        endforeach()
    endforeach()
endif()
