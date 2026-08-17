# AmiSSL on the ClassicWB rig

AmiSSL 5.27 is OpenSSL 3.6.2 as an Amiga shared library. It is installed in the
ClassicWB snapshots, so a guest has HTTPS available to any application that
wants it. See [CLASSICWB.md](CLASSICWB.md) for the rig itself.

It is in the `full` and `p96` snapshots and not in `68k`: 5.27 needs AmigaOS 3.0
or later and a 68020, and the A600 is a 68000. A `full` or `p96` launch that does
not find it fails, the same way `install_check` does.

| Path | Holds |
|---|---|
| `LIBS:amisslmaster.library` | the version-negotiating front end |
| `LIBS:AmiSSL/amissl_v362.library` | OpenSSL 3.6.2, 3.5 MB, 68020-40 build |
| `AmiSSL:` | assigned to `SYS:AmiSSL`, holds `Certs` with 290 root CAs |
| `C:OpenSSL` | the command-line tool |

This is not the vendor layout. The Installer puts the libraries under
`AmiSSL:Libs` and adds an `Assign LIBS: AmiSSL:Libs ADD`, which makes the open
depend on a User-Startup line having run. `amisslmaster.library` builds the
literal path `LIBS:AmiSSL/amissl_v%ld.library`, so the versioned library goes
straight there and opens with no assign. `AmiSSL:` stays, because that is how an
application finds the root CAs. `UserCerts` and `Private` are absent: git cannot
carry an empty directory.

Launch keys: `amissl`, `amissl_library`, `amissl_library_bytes`, `amissl_master`,
`amissl_certs`, `amissl_check`, `amissl_probe`. On `68k`, `amissl=absent` with
`amissl_reason`. The version is read from the binary's own `$VER` tag, never from
its filename.

## Proving it opens

    Stack 65536
    C:amisslprobe

Then read `DH0:amisslprobe.txt`. The probe opens the master, opens the versioned
library behind it, calls `InitAmiSSLA`, and reads `OpenSSL_version()` back out of
the library. It needs 32 KB of stack and says so rather than hanging.

## What it costs

Free memory in KB: before the open, with the library in, the largest free block
then, and after everything closes.

| Model | Snapshot | Before | With it | Largest | Closed | Open takes |
|---|---|---|---|---|---|---|
| A1200 | full | 6,854 | 3,377 | 1,912 | 3,451 | 0.34 s |
| A1200 rtg | p96 | 6,556 | 3,079 | 1,991 | 3,153 | 0.34 s |
| A3000 | full | 15,190 | 11,713 | 8,191 | 11,787 | 0.06 s |

On an A1200 AmiSSL takes half the free memory and leaves the largest free block
under 2 MB, so it is the dominant allocation on that machine. On an A3000 it is
3.4 MB of 15 and the largest block does not move. The library does not expunge
when its last opener closes, so the next opener pays nothing.
