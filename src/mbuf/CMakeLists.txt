# 4.4BSD mbuf emulation for the mbuf_* LVOs (docs/RESEARCH.md 3.2, milestone 7).
#
# mbuf_alloc.c and mbuf_ops.c contain no AmigaOS calls -- the two locking hooks
# live in mbuf_amiga.c -- so the same sources build and run on the host against
# the replica layout in include/aminetxduo/mbuf.h. See the host test below.

add_library(aminetxduo_mbuf STATIC
    mbuf_alloc.c
    mbuf_ops.c
    mbuf_amiga.c
    mbuf_packet.c
    mbuf_abi_check.c)

target_include_directories(aminetxduo_mbuf
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# netxduo for mbuf_packet.c's NX_PACKET bridge.
target_link_libraries(aminetxduo_mbuf PUBLIC netxduo aminetxduo_common)

target_compile_options(aminetxduo_mbuf PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------- host test --
#
# Host only: links stdio and the stub <exec/types.h> from src/config/test/shim,
# neither of which belongs in an Amiga build, and compiles the replica struct
# rather than the NDK one.
#
#   cc -std=c11 -Wall -Wextra -DAMI_MBUF_REPLICA -Iinclude \
#      -Isrc/config/test/shim -Isrc/mbuf \
#      src/mbuf/test/test_mbuf.c src/mbuf/mbuf_alloc.c src/mbuf/mbuf_ops.c \
#      -o test_mbuf

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_mbuf
        test/test_mbuf.c
        mbuf_alloc.c
        mbuf_ops.c)

    target_include_directories(test_mbuf PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_definitions(test_mbuf PRIVATE AMI_MBUF_REPLICA)
    target_compile_options(test_mbuf PRIVATE -Wall -Wextra)
    set_target_properties(test_mbuf PROPERTIES C_STANDARD 11)

    include(CTest)
    add_test(NAME mbuf_chains COMMAND test_mbuf)
endif()
