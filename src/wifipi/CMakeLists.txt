# anxwifipi.device: the Raspberry Pi 4's own Wi-Fi (a Broadcom/Cypress 43455
# on SDIO) behind a PiStorm32 running Emu68, and nothing else.
#
# The driver is third_party/wifipi -- the MPL-2.0 fork of Michal Schulz's
# WiFiPi.device at github.com/tinic/WiFiPi.device, branch gcc16, which carries
# the receiver work (drain loop, the card-line poller, the 200 us write path,
# AmiNetXDuo's single-copy receive) and its own LICENSE and NOTICE.  This file
# builds those sources the way the tree builds its own devices and gives the
# image the tree's name and version; nothing of the driver lives here.
#
# It links no C library on purpose (-nostdlib, -ffreestanding): a call the
# compiler invents -- memcpy for a struct copy, __udivdi3 -- fails the link
# instead of being resolved from libnix behind the archive scan, the trap
# lto-repro/ documents.  -fno-builtin and -fno-tree-loop-distribute-patterns
# keep GCC from inventing them in the first place.
option(AMINETXDUO_WIFIPI_LINK
       "Link DEVS:Networks/anxwifipi.device, the Pi 4 Wi-Fi SANA-II driver" ON)

if(AMINETXDUO_WIFIPI_LINK AND CMAKE_CROSSCOMPILING)
    set(WIFIPI "${CMAKE_SOURCE_DIR}/third_party/wifipi")
    if(NOT EXISTS "${WIFIPI}/src/device.c")
        message(FATAL_ERROR
            "third_party/wifipi is empty.  git submodule update --init third_party/wifipi, "
            "or configure with -DAMINETXDUO_WIFIPI_LINK=OFF.")
    endif()

    # devicetree.resource's headers, generated from its function table with
    # the toolchain's sfdc: the driver finds its SDIO host, mailbox, GPIO
    # block and interrupt line in Emu68's device tree.
    set(_dt_inc "${CMAKE_CURRENT_BINARY_DIR}/include")
    set(_dt_sfd "${WIFIPI}/sfd/devicetree_lib.sfd")
    set(_dt_headers "")
    foreach(_gen
            "clib;clib/devicetree_protos.h"
            "pragmas;pragmas/devicetree_pragmas.h"
            "proto;proto/devicetree.h"
            "macros;inline/devicetree.h")
        list(GET _gen 0 _mode)
        list(GET _gen 1 _out)
        get_filename_component(_dir "${_dt_inc}/${_out}" DIRECTORY)
        add_custom_command(
            OUTPUT "${_dt_inc}/${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
            COMMAND "${AMIGA_TOOLCHAIN_BIN}/sfdc" "${_dt_sfd}"
                    "--mode=${_mode}" "--output=${_dt_inc}/${_out}"
            DEPENDS "${_dt_sfd}"
            COMMENT "devicetree ${_out}"
            VERBATIM)
        list(APPEND _dt_headers "${_dt_inc}/${_out}")
    endforeach()
    add_custom_target(anxwifipi_headers DEPENDS ${_dt_headers})

    add_executable(anxwifipi_device
        "${WIFIPI}/src/device.c"     # keep first: the entry stub and the ROMTag
        "${WIFIPI}/src/init.c"
        "${WIFIPI}/src/mbox.c"
        "${WIFIPI}/src/sdio.c"
        "${WIFIPI}/src/end.c"
        "${WIFIPI}/src/wifipi.c"
        "${WIFIPI}/src/packet.c"
        "${WIFIPI}/src/d11.c"
        "${WIFIPI}/src/unit.c"
        "${WIFIPI}/src/findtoken.c")
    add_dependencies(anxwifipi_device anxwifipi_headers)
    set_target_properties(anxwifipi_device PROPERTIES OUTPUT_NAME "anxwifipi.device")

    # The name and the $VER string are the tree's: TOOL_VERSTAG() from the
    # generated <aminetxduo/version.h>, the same line every other image says.
    target_compile_definitions(anxwifipi_device PRIVATE
        "WIFIPI_DEVICE_NAME=\"anxwifipi.device\""
        "WIFIPI_TASK_UNIT=\"AmiNetXDuo anxwifipi unit\""
        "WIFIPI_TASK_POLLER=\"AmiNetXDuo anxwifipi poller\""
        "WIFIPI_TASK_SCANNER=\"AmiNetXDuo anxwifipi scanner\""
        "WIFIPI_TASK_RECEIVER=\"AmiNetXDuo anxwifipi receiver\""
        "WIFIPI_VERSION_HEADER=<aminetxduo/version.h>"
        WIFIPI_VERSION=1
        WIFIPI_REVISION=0)
    target_include_directories(anxwifipi_device PRIVATE
        "${WIFIPI}/include"
        "${_dt_inc}"
        "${CMAKE_BINARY_DIR}/include")
    # A Pi is never a 68000: whichever CPU arm the tree is building, this image
    # is 68020 code (divu.l and friends inline; the all-CPU arm would call
    # libgcc's __udivsi3, which -nostdlib refuses).  -fno-optimize-sibling-calls
    # for the same reason the top level applies it to 68020+ trees: the pinned
    # linker resolves a cross-section 32-bit sibling branch twelve bytes short.
    target_compile_options(anxwifipi_device PRIVATE
        -m68020 -mtune=68040 -fno-optimize-sibling-calls
        -Os -Wall -Wextra -Werror -fomit-frame-pointer
        -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns)
    # -Wl,-u,_RomTag: nothing in the image references the ROMTag (exec finds it
    # by scanning), so under -flto it is a link root or it is gone.  The
    # driver's own tools/check-image.sh proves the chain in the bytes below.
    target_link_options(anxwifipi_device PRIVATE
        -m68020 -mtune=68040 -fno-optimize-sibling-calls
        -nostartfiles -nostdlib -ffreestanding
        "-Wl,-e,__start" "-Wl,-u,_RomTag"
        "-Wl,-Map=$<TARGET_FILE_DIR:anxwifipi_device>/anxwifipi.device.map")

    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        add_custom_command(TARGET anxwifipi_device POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:anxwifipi_device>"
            VERBATIM)
    endif()
    add_custom_command(TARGET anxwifipi_device POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DLIBRARY_BINARY=$<TARGET_FILE:anxwifipi_device>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-library-entry.cmake"
        VERBATIM)
    add_custom_command(TARGET anxwifipi_device POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DTOOL_BINARY=$<TARGET_FILE:anxwifipi_device>"
                -P "${CMAKE_SOURCE_DIR}/cmake/check-version-tag.cmake"
        VERBATIM)
    add_custom_command(TARGET anxwifipi_device POST_BUILD
        COMMAND "${WIFIPI}/tools/check-image.sh"
                "$<TARGET_FILE:anxwifipi_device>"
                "$<TARGET_FILE_DIR:anxwifipi_device>/anxwifipi.device.map"
                anxwifipi.device
        VERBATIM)
    if(AMIGA_SIZE)
        add_custom_command(TARGET anxwifipi_device POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:anxwifipi_device>"
            COMMENT "anxwifipi.device size")
    endif()
endif()
