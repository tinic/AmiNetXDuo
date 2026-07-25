# usergroup.library -- AmiTCP/Roadshow's companion user/group database library.
#
# ug_library.c carries the romtag, the vector table and ug_LibEntry(), and must
# stay FIRST in the object order: the first code in a .library image has to be
# the "moveq #-1,d0 / rts" guard against someone running it as a program.

add_library(aminetxduo_usergroup STATIC
    ug_library.c        # keep first
    ug_context.c
    ug_ids.c
    ug_db.c
    ug_misc.c)

target_include_directories(aminetxduo_usergroup
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ami_alloc/ami_free and the AMI_* logging macros.
target_link_libraries(aminetxduo_usergroup PUBLIC aminetxduo_common)

# This library is deliberately standalone: it talks to exec.library and
# dos.library only, so it must not pick up ThreadX, NetX Duo or src/config.
# It also defines its own SysBase/DOSBase, because a shared library gets no C
# startup code -- do not link it into an image that already defines them.
