# FreeBSD's tests/sys/netinet, run against this stack.
#
# The files under this directory that carry a BSD-2-Clause header come from
# https://github.com/freebsd/freebsd-src/tree/main/tests/sys/netinet unmodified
# apart from the note appended after that header.  What makes them build is
# atf-c.h, which is ours; see its comment block for what each ATF macro becomes
# and what this platform cannot answer.
#
# Guest-side only, like every other on-Amiga harness: an AmigaOS executable that
# opens bsdsocket.library, linked against none of our code, with the interface
# coming from tests/tcpdrill/tapdev.c.
#
#   AMINETXDUO_RUN_TAG=atf ./tests/sockopt/run-sockopt.sh -b build/atf

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(atf_tcp_socket
    tcp_socket.c
    atf_main.c
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill/tapdev.c")

target_include_directories(atf_tcp_socket PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/tcpdrill"
    "${CMAKE_SOURCE_DIR}/src/sana2")

if(AMIGA_NDK_INCLUDE)
    target_include_directories(atf_tcp_socket PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

target_compile_definitions(atf_tcp_socket PRIVATE ATF_PROGNAME=tcp_socket)

# The adopted file's first line is #include <sys/socket.h>, so the NDK's gaps
# have to be closed before it is read.  atf-prelude.h says which and why.
target_compile_options(atf_tcp_socket PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/atf-prelude.h")

# Adopted sources are upstream's, so they are built to upstream's expectations
# rather than this tree's warning set.  -Wall -Wextra only, and no -Werror:
# a warning in a file we do not own is a fact about the file, not a defect to
# fix locally.
target_compile_options(atf_tcp_socket PRIVATE -Wall -Wextra)

set_target_properties(atf_tcp_socket PROPERTIES OUTPUT_NAME "AtfTcpSocket")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(atf_tcp_socket bsdsocket_library)
endif()

if(AMIGA_SIZE)
    add_custom_command(TARGET atf_tcp_socket POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:atf_tcp_socket>"
        COMMENT "AtfTcpSocket size")
endif()
