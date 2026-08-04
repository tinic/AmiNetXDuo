# usergroup.library -- AmiTCP/Roadshow's companion user/group database library.
#
# ug_library.c carries the romtag, the vector table and ug_LibEntry(), and must
# stay FIRST in the object order: the first code in a .library image has to be
# the "moveq #-1,d0 / rts" guard against someone running it as a program.

option(AMINETXDUO_USERGROUP_LINK
       "Link LIBS:usergroup.library as an AmigaOS shared library" ON)

set(USERGROUP_SOURCES
    ug_library.c        # keep first
    ug_context.c
    ug_ids.c
    ug_db.c
    ug_parse.c
    ug_misc.c)

add_library(aminetxduo_usergroup STATIC ${USERGROUP_SOURCES})

target_include_directories(aminetxduo_usergroup
    PUBLIC  ${AMINETXDUO_CORE_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ami_alloc/ami_free and the AMI_* logging macros.
target_link_libraries(aminetxduo_usergroup PUBLIC aminetxduo_common)

# This library is deliberately standalone: it talks to exec.library and
# dos.library only, so it must not pick up ThreadX, NetX Duo or src/config.
# It also defines its own SysBase/DOSBase, because a shared library gets no C
# startup code -- do not link it into an image that already defines them.

# --------------------------------------------------------------------- link --
#
# Same shape as bsdsocket.library: the sources are listed directly rather than
# pulled from the archive, because nothing references the romtag symbolically --
# Exec finds it by scanning the loaded segment -- and ug_library.c has to be
# first so its "moveq #-1,d0 / rts" lands at offset 0 of the first code hunk.
#
# -nostartfiles, not -nostdlib: there is no crt0, but memset/memcpy and gcc's
# integer helpers still have to come from somewhere. This library talks only to
# exec.library and dos.library, so nothing pulls newlib's reentrant machinery.

if(AMINETXDUO_USERGROUP_LINK)
    add_executable(usergroup_library ${USERGROUP_SOURCES})
    set_target_properties(usergroup_library PROPERTIES
        OUTPUT_NAME "usergroup.library")

    target_include_directories(usergroup_library PRIVATE
        ${AMINETXDUO_CORE_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(usergroup_library PRIVATE aminetxduo_common)
    target_link_options(usergroup_library PRIVATE -nostartfiles)

    if(AMIGA_SIZE)
        add_custom_command(TARGET usergroup_library POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:usergroup_library>"
            COMMENT "usergroup.library size")
    endif()
endif()

# ---------------------------------------------------------------- host test --
#
# ug_parse.c is the half of the database with no dos.library in it, split out
# of ug_db.c so it can be built and run here. The files it reads live in DEVS:
# and are not written by us, so the line-ending and arena cases are worth a
# test that runs on every build rather than only on an Amiga.
#
#   cc -std=c99 -Wall -Wextra -Iinclude -Isrc/usergroup \
#      -Isrc/config/test/shim src/usergroup/test/test_usergroup.c \
#      src/usergroup/ug_parse.c -o test_usergroup

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_usergroup
        test/test_usergroup.c
        ug_parse.c)

    target_include_directories(test_usergroup PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_options(test_usergroup PRIVATE -Wall -Wextra)

    include(CTest)
    add_test(NAME usergroup_parsers COMMAND test_usergroup)
endif()
