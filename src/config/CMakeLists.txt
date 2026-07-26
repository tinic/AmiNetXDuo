# Roadshow-layout configuration and the DEVS:Internet netdb store.
#
# config_text.c / config_parse.c / netdb.c are free of AmigaDOS calls -- all
# file access goes through the ami_cfg_read_file() hook that config_file.c
# implements -- so the same sources build and run on the host (see below).

add_library(aminetxduo_config STATIC
    config_text.c
    config_parse.c
    netdb.c
    config_file.c)

target_include_directories(aminetxduo_config
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aminetxduo_config PUBLIC aminetxduo_common)

# This archive has two very different consumers.  bsdsocket.library wants all
# of it; a Shell command wants a slice -- Online reads one interface file and
# never touches netdb.c, and before -ffunction-sections it carried the whole
# parser anyway, because a pulled archive member is kept whole.  Splitting the
# functions lets src/tools/'s --gc-sections drop what a given command cannot
# reach; with the same flag on the commands' own objects that is 12 KB off
# Online and 9 KB off host.  The library link does not collect,
# so all it pays is per-section padding -- 1,848 bytes across bsdsocket.library,
# measured.  See docs/RESEARCH.md §21.
target_compile_options(aminetxduo_config PRIVATE -ffunction-sections)
# -fno-optimize-sibling-calls TRAVELS WITH -ffunction-sections, ALWAYS.
#
# A tail call to a function in ANOTHER SECTION of the SAME object is the one
# construct that makes this toolchain emit a 32-bit PC-relative branch against
# a local section symbol, and the linker resolves that relocation TWELVE BYTES
# SHORT -- into the middle of the preceding function.  `ping` rebooted the
# machine on exactly this: `return tool_break();` at the end of
# tool_delay_ticks() landed inside tool_break_arm()'s `move.l #4096,d1`.
#
# -ffunction-sections is what creates the cross-section reference, so the two
# flags are one decision and must not be separated.  cmake/check-pcrel-branches
# .cmake fails the build if one gets through anyway.  docs/RESEARCH.md §25.
target_compile_options(aminetxduo_config PRIVATE -fno-optimize-sibling-calls)

# ---------------------------------------------------------------- host test --
#
# Only when configuring for the host: the test links against stdio and a stub
# <exec/types.h>, neither of which belongs in an Amiga build. Run it with
#   cmake -S . -B build-host && cmake --build build-host --parallel && ctest
# or directly:
#   cc -std=c99 -Wall -Wextra -Iinclude -Isrc/config/test/shim \
#      src/config/test/test_config.c src/config/config_text.c \
#      src/config/config_parse.c src/config/netdb.c -o test_config

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_config
        test/test_config.c
        config_text.c
        config_parse.c
        netdb.c)

    target_include_directories(test_config PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim")

    target_compile_options(test_config PRIVATE -Wall -Wextra)

    include(CTest)
    add_test(NAME config_parsers COMMAND test_config)
endif()
