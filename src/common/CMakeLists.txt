add_library(aminetxduo_common STATIC compat.c crashguard.c)
target_include_directories(aminetxduo_common PUBLIC ${AMINETXDUO_CORE_INCLUDES})
target_link_libraries(aminetxduo_common PUBLIC threadx)
