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

The snapshot is ClassicWB installed and settled, and carries no file of ours. A
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

## Rebuilding a snapshot

    ~/amiga-assets/classicwb/install.sh <68k|full|p96>

The archives are hash-pinned and verified before use. The snapshot is a cache;
the script and the archives are the source of truth.
