# ClassicWB test rig

A full Workbench guest on the LAN, bridged, with httpd, bsdsocket.library and
the tools staged onto it from a build directory.

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
| Payload | `-b <builddir>`, default `build/cm` | `cmake --build` |

The snapshot is ClassicWB installed and settled, and carries no file of ours.
The launcher copies it per run and stages the payload onto the copy, so a fresh
build reaches a running guest with `-b`, and a run that wedges its drive costs
the copy rather than the install.

## Options

| Option | Effect | Default |
|---|---|---|
| `-m` | model, picks the Kickstart | A1200 |
| `-v` | `plain` or `rtg` | plain |
| `-b` | build directory the payload comes from | `build/cm` |
| `-B` | bridge interface | ens18 |
| `-n` | hostname and mDNS name | `amiga-<model>-<variant>` |
| `-p` | httpd port | 80 |
| `-t` | seconds before the guest is stopped | 28800 |
| `-s` | snapshot store | `~/amiga-assets/classicwb/snapshots` |
| `-c` | ssh host that checks the served version | unset |

Output is `key=value` lines. `RESULT=UP` and exit 0 mean the guest booted, took
a lease, and answered.

## The version gate

Three checks, so a launch cannot serve a binary left over from an earlier one.

| Check | Compares | Fails with |
|---|---|---|
| `guest_httpd_version` | the version the guest read off the binary it loaded, against the build directory | staged from somewhere else |
| `staged_sha256` | the bytes on the drive against the bytes in the build directory | not the one in the build |
| `served_check` | the `Server:` header of the running server, against the build directory | serving another version |

The third needs `-c`. A frame the emulator host sends to a guest of its own
never reaches it, so a request from that host times out against a guest that is
serving normally and the timeout says nothing about the guest. `-c` names
another machine on the same segment. Without it the first two still run.

## Rebuilding a snapshot

    ~/amiga-assets/classicwb/install.sh <68k|full|p96>

The archives are hash-pinned and verified before use. The snapshot is a cache;
the script and the archives are the source of truth.
