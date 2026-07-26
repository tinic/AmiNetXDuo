add_library(aminetxduo_sana2 STATIC
    sana2_device.c
    sana2_copy.c
    sana2_rx.c
    sana2_tx.c
    sana2_driver.c)

target_include_directories(aminetxduo_sana2
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# aminetxduo_net68k explicitly, not by way of netxduo: the checksum
# replacement is switchable and this does not depend on it -- sana2_copy.c
# wants n68k_copy_bytes() whatever the checksum is doing.
target_link_libraries(aminetxduo_sana2 PUBLIC
    netxduo aminetxduo_common aminetxduo_net68k)

# The two capture taps in sana2_rx.c and sana2_tx.c. Header-only would not be
# enough: ami_bpf_tap_rx/tx live in src/bpf/.
if(AMINETXDUO_BPF)
    target_link_libraries(aminetxduo_sana2 PUBLIC aminetxduo_bpf)
endif()

# AMINETXDUO_IPV6 (which adds the 0x86DD reader here) is set globally by the
# root CMakeLists, not per-target: it changes the NetX Duo struct layout, so
# every translation unit in the tree has to agree. See the comment there.
