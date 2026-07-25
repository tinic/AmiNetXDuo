# Milestone 7 test: the mbuf_* emulation and the bpf_* raw path on real 68k.
#
# The exhaustive batteries are the host tests (src/mbuf/test, src/bpf/test);
# this covers what a host cannot: the real MSIZE/MLEN/MHLEN and bpf_hdr sizes,
# 128-byte alignment out of a real AllocVec(), the NX_PACKET bridge, the
# transmit tap, Forbid()/Signal()/GetSysTime(), and a crash-guarded run of the
# filter VM against malformed and random programs.

add_executable(mbuf_bpf_test mbuf_bpf_test.c)

target_link_libraries(mbuf_bpf_test PRIVATE
    aminetxduo_mbuf aminetxduo_bpf aminetxduo_common netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(mbuf_bpf_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(mbuf_bpf_test PROPERTIES OUTPUT_NAME "mbuf_bpf_test")

if(AMIGA_SIZE)
    add_custom_command(TARGET mbuf_bpf_test POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:mbuf_bpf_test>"
        COMMENT "mbuf_bpf_test size")
endif()
