# Whether a stalled TCP connection is visible, and whether TCP_USER_TIMEOUT
# cuts one short.
#
# Guest side only.  The timing arithmetic is already covered without a network
# by tests/netstack/host/test_tcp_retries_host.c, which drives the fast
# periodic timer by hand; this one is here because a deadline that works
# against a simulated clock and not against a real link would pass that.
#
#   AMINETXDUO_RUN_TAG=tcpstall ./tests/tcpstall/run-tcpstall.sh -B ens18

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(tcpstall_test tcpstall_test.c)

target_include_directories(tcpstall_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(tcpstall_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(tcpstall_test PROPERTIES OUTPUT_NAME "tcpstall_test")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(tcpstall_test bsdsocket_library)
endif()
