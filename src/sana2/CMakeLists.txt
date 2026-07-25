add_library(aminetxduo_sana2 STATIC
    sana2_device.c
    sana2_copy.c
    sana2_rx.c
    sana2_tx.c
    sana2_driver.c)

target_include_directories(aminetxduo_sana2
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aminetxduo_sana2 PUBLIC netxduo aminetxduo_common)

# AMINETXDUO_IPV6 (which adds the 0x86DD reader here) is set globally by the
# root CMakeLists, not per-target: it changes the NetX Duo struct layout, so
# every translation unit in the tree has to agree. See the comment there.
