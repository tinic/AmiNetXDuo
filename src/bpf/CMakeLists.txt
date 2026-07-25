# The bpf_* raw packet path and the BPF filter VM (docs/RESEARCH.md 3.2,
# milestone 7).
#
# bpf_filter.c, bpf_validate.c, bpf_channel.c and bpf_tap.c contain no AmigaOS
# calls -- the platform hooks live in bpf_amiga.c -- so they build and run on
# the host against the replica layout in include/aminetxduo/bpf.h.

add_library(aminetxduo_bpf STATIC
    bpf_filter.c
    bpf_validate.c
    bpf_channel.c
    bpf_tap.c
    bpf_tap_tx.c
    bpf_amiga.c
    bpf_abi_check.c)

target_include_directories(aminetxduo_bpf
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# netxduo for bpf_tap_tx.c, which walks an NX_PACKET chain.
target_link_libraries(aminetxduo_bpf PUBLIC netxduo aminetxduo_common)

target_compile_options(aminetxduo_bpf PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------- host test --
#
#   cc -std=c11 -Wall -Wextra -DAMI_BPF_REPLICA -Iinclude \
#      -Isrc/config/test/shim -Isrc/bpf \
#      src/bpf/test/test_bpf.c src/bpf/bpf_filter.c src/bpf/bpf_validate.c \
#      src/bpf/bpf_channel.c src/bpf/bpf_tap.c -o test_bpf

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_bpf
        test/test_bpf.c
        bpf_filter.c
        bpf_validate.c
        bpf_channel.c
        bpf_tap.c)

    target_include_directories(test_bpf PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_definitions(test_bpf PRIVATE AMI_BPF_REPLICA)
    target_compile_options(test_bpf PRIVATE -Wall -Wextra)
    set_target_properties(test_bpf PROPERTIES C_STANDARD 11)

    include(CTest)
    add_test(NAME bpf_filter_vm COMMAND test_bpf)
endif()
