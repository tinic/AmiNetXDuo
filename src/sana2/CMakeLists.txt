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

if(AMINETXDUO_IPV6)
    # Adds the 0x86DD reader; must agree with the rest of the tree.
    target_compile_definitions(aminetxduo_sana2 PUBLIC AMINETXDUO_IPV6)
endif()
