# CMAKE_PROJECT_INCLUDE that turns -flto on for the whole tree without
# disturbing CMAKE_C_FLAGS_INIT (-m68020 -Os -fomit-frame-pointer ...), which a
# -DCMAKE_C_FLAGS on the command line would silently replace.
add_compile_options(-flto)
add_link_options(-flto)

# Nothing in any of these four binaries REFERENCES its romtag: exec finds it by
# scanning the loaded segment for RTC_MATCHWORD.  Under LTO the linker plugin
# reports the symbol as defined-in-IR-and-referenced-by-nothing, so the whole
# chain -- romtag, RTF_AUTOINIT init table, LVO vector table, and every vector
# it points at -- is dead code and gets removed.  The link succeeds and the
# library is empty.  --undefined names the romtag as a link root, which holds
# the entire chain and also keeps --gc-sections off it.
#
# Per target, because -u on a command that has no romtag is an undefined
# reference and fails the link.
function(_lto_keep target symbol)
    add_link_options(
        "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${target}>:-Wl,-u,${symbol}>")
endfunction()
_lto_keep(bsdsocket_library _bsd_romtag)
_lto_keep(usergroup_library _ug_romtag)
_lto_keep(tls_library       _tls_romtag)
_lto_keep(anxnet_device     _netdev_romtag)
