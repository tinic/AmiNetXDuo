# Shared-library load test: opens LIBS:usergroup.library and
# LIBS:bsdsocket.library through the ABI and calls a few LVOs.
#
# Deliberately linked against nothing of ours -- the point is that the library
# images stand on their own.

add_executable(library_test library_test.c)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(library_test PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(library_test PROPERTIES OUTPUT_NAME "library_test")

if(AMINETXDUO_BSDSOCKET_LINK)
    add_dependencies(library_test bsdsocket_library)
endif()
if(AMINETXDUO_USERGROUP_LINK)
    add_dependencies(library_test usergroup_library)
endif()
