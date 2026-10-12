# The netdev delay, measured on a real chipset against a clock that is not the
# driver's own.  Produces an AmigaOS executable; it does not run on the build
# host, and the host half of the same question is
# src/netdev/test/test_netdev_clock.c, which does.
#
# netdev_clock.c is COMPILED IN rather than linked from the device: the point
# is to time the very code anxnet.device runs, and the device does not export
# it.  Nothing else of the driver comes with it -- the file has no dependency
# beyond exec/types.h, which is what makes this possible.

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

# Exercises real SANA-II driver rings with a controlled Ethernet loopback.
add_executable(netmulticast_test multicast.c)
add_executable(netmulticast_launcher multicast-launcher.c)
target_include_directories(netmulticast_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/sana2")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(netmulticast_test PRIVATE "${AMIGA_NDK_INCLUDE}")
    target_include_directories(netmulticast_launcher PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

add_executable(netdelay_test
    netdelay.c
    "${CMAKE_SOURCE_DIR}/src/netdev/netdev_clock.c")

target_include_directories(netdelay_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/netdev")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(netdelay_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(netdelay_test PROPERTIES OUTPUT_NAME "netdelay_test")
