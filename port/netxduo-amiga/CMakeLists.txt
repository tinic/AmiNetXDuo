# NetX Duo AmigaOS port -- headers only.
#
# nx_port.h and nx_user.h are picked up by the vendored NetX Duo core through
# AMINETXDUO_PORT_INCLUDES in the top-level CMakeLists, which is why there is
# no compiled object here.  The interface target exists so that components
# outside the core (the SANA-II shim, bsdsocket.library, the tests) can depend
# on the tuning explicitly.

add_library(netxduo_port INTERFACE)

target_include_directories(netxduo_port INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/inc")

target_link_libraries(netxduo_port INTERFACE netxduo threadx_port)
