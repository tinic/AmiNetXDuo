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
    add_executable(rfbgen host/rfbgen.c)
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
    add_executable(rfbwords host/rfbwords.c rfb_words.c)
    target_include_directories(rfbwords PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(rfbwords PRIVATE -Wall -Wextra)
    set_target_properties(rfbwords PROPERTIES C_STANDARD 99)

    include(CTest)
    add_test(NAME rfb_words COMMAND rfbwords)

    # rfbbench decodes every frame it encodes and compares it with the frame
    # that went in, for every strategy, and exits non-zero on a mismatch.  A
    # compression number from an encoder nobody round-tripped is worthless, so
    # the round trip is the test.
    include(CTest)
    add_test(NAME rfb_roundtrip
             COMMAND "${CMAKE_COMMAND}"
                     "-DGEN=$<TARGET_FILE:rfbgen>"
                     "-DBENCH=$<TARGET_FILE:rfbbench>"
                     "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/pfs"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.cmake")
endif()
