# codesets.library, built from third_party/libcodesets unmodified.
#
# Upstream's own src/Makefile targets clib2 and -m68020-60; this builds the
# same objects at the tree's CPU (-m68000 in the shipping `any` build) against
# newlib, with cs_libc.c standing in for what clib2 had and newlib cannot give
# a library with no crt0.  libinit.c is first: its _start, upstream's
# "moveq #20,d0 / rts" (RETURN_FAIL to a Shell that runs it), lands at offset
# 0.  cmake/check-library-entry.cmake wants our moveq #-1 and is not applied.
#
# No LTO: nothing references the romtag symbolically, and the vendored code is
# built the way upstream builds it rather than merged into one unit.

if(CMAKE_CROSSCOMPILING)
    set(CS_SRC "${CMAKE_SOURCE_DIR}/third_party/libcodesets/src")
    set(CS_UPSTREAM
        "${CS_SRC}/libinit.c"      # keep first
        "${CS_SRC}/init.c"
        "${CS_SRC}/utils.c"
        "${CS_SRC}/base64.c"
        "${CS_SRC}/convertUTF.c"
        "${CS_SRC}/codesets.c"
        "${CS_SRC}/codepages.c")

    add_executable(codesets_library ${CS_UPSTREAM} cs_libc.c)
    set_target_properties(codesets_library PROPERTIES
        OUTPUT_NAME "codesets.library")
    target_include_directories(codesets_library PRIVATE
        "${CS_SRC}" "${CMAKE_SOURCE_DIR}/third_party/libcodesets/include"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(codesets_library PRIVATE __amigaos3__)
    target_compile_options(codesets_library PRIVATE -fno-lto
        -include "${CMAKE_CURRENT_SOURCE_DIR}/cs_libc.h")
    # saveds is a -fbaserel attribute; upstream's SDI headers ask for it and
    # this build has no base-relative data.
    set_source_files_properties(${CS_UPSTREAM}
        PROPERTIES COMPILE_OPTIONS "-Wno-attributes")
    target_link_options(codesets_library PRIVATE -fno-lto -nostartfiles
        "-Wl,-Map=$<TARGET_FILE_DIR:codesets_library>/codesets.library.map")

    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        add_custom_command(TARGET codesets_library POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:codesets_library>"
            VERBATIM)
    endif()

    if(AMIGA_SIZE)
        add_custom_command(TARGET codesets_library POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:codesets_library>"
            COMMENT "codesets.library size")
    endif()
endif()
