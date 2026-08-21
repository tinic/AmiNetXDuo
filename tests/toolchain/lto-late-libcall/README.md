# Late LTO libcall reduction

This is the compiler/linker shape that made P-256 look miscompiled.  It has no
dependency on AmiNetXDuo, AmigaOS, or an emulator; it only needs the pinned
`m68k-amigaos` toolchain.

`caller.c` calls `runtime_init()`, which makes the linker plugin pull
`runtime.o` from an archive before whole-program analysis.  The same archive
member defines `__muldi3`, but the 64-bit multiplication in `caller.c` does
not become a call to that function until RTL expansion in LTRANS.  WPA sees no
GIMPLE reference and removes the body.  The plugin's IR dummy still advertises
the definition, so the late call resolves to a zero-size symbol instead of
pulling libgcc's fallback.

Run:

```sh
PATH=/path/to/m68k-amigaos/bin:$PATH ./reproduce.sh
```

The script builds four links.  The non-LTO control, an LTO definition marked
`__attribute__((used))`, and an LTO link forced with `-u,___muldi3` retain a
recognizable stand-in body.  The unannotated LTO link does not.  It reports
`RESULT=reproduced` on the pinned GCC 16.2.0b toolchain, or
`RESULT=toolchain_fixed` if a future toolchain retains the body or lowers the
multiply without a late call.

The product fix is the `AMI_LATE_LIBCALL` annotation in
`src/common/ami_udivdi3.c`.  This is preferable to a speculative GCC patch:
the definitions are application-supplied compiler-runtime replacements, and
`used` states the otherwise-invisible reachability directly.  A compiler fix
would need to preserve every target optab libfunc definition before knowing
which operations RTL expansion will lower to calls.
