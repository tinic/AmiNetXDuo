# On-Amiga test harnesses.

add_subdirectory(ram_driver)
add_subdirectory(soak)
add_subdirectory(netstack)
add_subdirectory(libraries)
add_subdirectory(crypto68k)
if(AMINETXDUO_BPF)
    add_subdirectory(mbuf_bpf)
endif()
if(AMINETXDUO_TLS)
    add_subdirectory(tls)
endif()
