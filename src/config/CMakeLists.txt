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
