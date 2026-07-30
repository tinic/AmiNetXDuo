# On-Amiga test harnesses.

add_subdirectory(ram_driver)
add_subdirectory(soak)
add_subdirectory(netstack)
add_subdirectory(libraries)
add_subdirectory(crypto68k)
add_subdirectory(perf)
add_subdirectory(tools)
add_subdirectory(tcpdrill)

# Host-only, and it says so itself: the sanitizer fuzz drivers for the parsers
# that read bytes we did not write. Nothing to build when cross-compiling.
add_subdirectory(fuzz)
add_subdirectory(leak)
add_subdirectory(concurrent)
add_subdirectory(bracket)
if(AMINETXDUO_IPV6)
    add_subdirectory(ipv6)
endif()
if(AMINETXDUO_BPF)
    add_subdirectory(mbuf_bpf)
endif()
if(AMINETXDUO_TLS)
    add_subdirectory(tls)
endif()
