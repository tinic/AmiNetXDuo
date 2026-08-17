# ClassicWB test rig

A full Workbench guest on the LAN, bridged, with AmiNetXDuo installed on it the
way a user installs it: a release archive, unpacked on the drive and installed
by Commodore's Installer. The install is part of what each launch tests.

## Combinations

| Model | Kickstart | Variant | ClassicWB | Command |
|---|---|---|---|---|
| A600 | 3.1 40.63 A500-A600-A2000 | plain | 68K | `tools/classicwb.sh -m A600` |
| A600 | 3.1 40.63 A500-A600-A2000 | rtg | none | refused, exit 2 |
| A1200 | 3.1 40.68 A1200 | plain | FULL | `tools/classicwb.sh -m A1200` |
| A1200 | 3.1 40.68 A1200 | rtg | P96 | `tools/classicwb.sh -m A1200 -v rtg` |
| A3000 | 3.1 40.68 A3000 | plain | FULL | `tools/classicwb.sh -m A3000` |
| A3000 | 3.1 40.68 A3000 | rtg | P96 | `tools/classicwb.sh -m A3000 -v rtg` |

Add `-c <host>` to any of them to have the served version checked from another
machine.

The Kickstart comes from `~/amiga-assets/env.sh`. A ROM that does not match the
model and the CPU produces a black screen and an empty log.

## Why the A600 rows differ

ClassicWB FULL states 68020 and 6 MB. An A600 is a 68000, so it takes ClassicWB
68K, which is the edition published for that machine rather than a reduced
FULL. ClassicWB P96 states 68020 and a graphics card, and Picasso96 is 68020
code, so the A600 has no rtg row; the launcher refuses it by name instead of
substituting.

## What each variant gives

| Variant | Screen | Serves |
|---|---|---|
| plain | chipset, 640x256 | `/` drawer, `/shell` |
| rtg | uaegfx 640x480x8 | `/` drawer, `/shell`, `/console` |

`/console` streams the frontmost screen. The geometry word's format field is 1
on a card and 0 on the planar fallback, and a board that fails to come up is
otherwise invisible: Workbench falls back to the chipset silently.

## The split

| Part | Where | Built by |
|---|---|---|
| Snapshot | `~/amiga-assets/classicwb/snapshots/<edition>/tree` | `install.sh` |
| Manifest | `~/amiga-assets/classicwb/snapshots/<edition>/manifest.json` | `install.sh` |
| Archive | `-b <builddir>`, default `build/cm` | `dist/make-dist.sh` and `clients/dropbear/build.sh` |

The snapshot is ClassicWB and AmiSSL installed and settled, and carries no file
of ours. A
launch copies it, builds a release archive from the build directory, unpacks the
archive on the copy as a download arrives, and boots once with the Installer
running against it. `-a` takes an archive as given instead of building one.
`dbclient` is built as well, because it comes from `clients/dropbear/build.sh`
rather than from CMake and `ssh` is otherwise missing from the archive.

A run that wedges its drive costs the copy, not the install. The install adds 50
to 92 seconds; a whole launch is 77 to 151 seconds.

Four steps run after the Installer, each one the Installer's own text describes
as the user's: `anxnet.device` into `DEVS:Networks` with a `CARD=` line, `MDNS=`
on, the host name, and httpd on the chosen drawer and port.

## Options

| Option | Effect | Default |
|---|---|---|
| `-m` | model, picks the Kickstart | A1200 |
| `-v` | `plain` or `rtg` | plain |
| `-b` | build directory the archive is built from | `build/cm` |
| `-a` | install this archive instead of building one | unset |
| `-B` | bridge interface | ens18 |
| `-n` | hostname and mDNS name | `amiga-<model>-<variant>` |
| `-p` | httpd port | 80 |
| `-t` | seconds before the guest is stopped | 28800 |
| `-s` | snapshot store | `~/amiga-assets/classicwb/snapshots` |
| `-c` | ssh host that checks the served version | unset |

Output is `key=value` lines. `RESULT=UP` and exit 0 mean the guest booted, took
a lease, and answered.

## The gates

Five checks, so a launch cannot report a guest that is incomplete, stale, or
running on somebody else's driver.

| Check | Compares | Fails with |
|---|---|---|
| `install_check` | what the Installer left on the drive, against the file set derived from the archive | an incomplete install |
| `build_match` | the archive against the build directory it was built from | an archive from elsewhere |
| `version_match` | the version the guest read off the binary it loaded, against the build directory | a binary left from an earlier run |
| `driver_match` | the driver the guest is running on, against the one installed | a guest on `a2065.device` when ours was installed |
| `served_check` | the `Server:` header of the running server, against the build directory | serving another version |

`install_check` derives its expected set from the archive rather than from a
list written here, so it cannot drift from what ships. `driver_match` was proved
by a negative test: the same launch with `a2065.device` staged booted, took a
lease, served, and exited 1.

`served_check` needs `-c`. A frame the emulator host sends to a guest of its own
never reaches it, so a request from that host times out against a guest that is
serving normally and the timeout says nothing about the guest. `-c` names
another machine on the same segment. Without it the first two still run.

## AmiSSL

AmiSSL 5.27 is in the `full` and `p96` snapshots. It is not in `68k`: 5.27 needs
AmigaOS 3.0 or later and a 68020, and the A600 is a 68000. A `full` or `p96`
launch that does not find it fails, the same way `install_check` does.

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

### Proving it opens

    Stack 65536
    C:amisslprobe

Then read `DH0:amisslprobe.txt`. The probe opens the master, opens the versioned
library behind it, calls `InitAmiSSLA`, and reads `OpenSSL_version()` back out of
the library. It needs 32 KB of stack and says so rather than hanging.

### What it costs

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

## Rebuilding a snapshot

    ~/amiga-assets/classicwb/install.sh <68k|full|p96>

The archives are hash-pinned and verified before use, in
`~/amiga-assets/classicwb/archives/` and `~/amiga-assets/amissl/archives/`. The
snapshot is a cache; the script and the archives are the source of truth.

A rebuild reproduces every byte of content, but `tree_sha256` is not stable
across rebuilds: 43 `Fonts/*.font.uaem` sidecars carry a datestamp the guest
writes from its own clock.
