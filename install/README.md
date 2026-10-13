# install/

The Installer script, the configuration files that the archive ships, and the
tooling that proves both work. `dist/make-dist.sh` assembles all of it into the
distribution archive.

```
Install-AmiNetXDuo          the Installer script
Install-AmiNetXDuo.info     its icon: project, default tool "Installer"
Installer                   modified Installer 2.17, with lower-memory
                            parsing and 68000 / Kickstart 2.04 support
AmiNetXDuo.info             drawer icon
Drawer.info Document.info   generic icons for Docs/, Examples/, ReadMe
Guide.info                  icon for AmiNetXDuo.guide, default tool "MultiView"

devs/Internet/              protocols, services, networks, shipped as-is
examples/NetInterfaces/     one commented interface file per card
examples/Internet/          routes, name_resolution, hosts, service_discovery

tools/makeicon.py           writes the .info files
tools/showicon.py           reads one back and draws it, independently
tools/compile-glowicons.py  compiles transparent two-state PNG artwork (Pillow)
icons/glow/                 compiled color images, previews and PNG masters
tools/checkscript.py        static checks on the Installer script

test/run-workbench.sh       installs the archive on a real Workbench 3.1
test/installdrive.c         drives the Installer inside the guest
test/bootcheck.c            boots the installed machine and checks it works
test/icontest.c             hands the generated .info files to icon.library
test/test_glowicons.py      checks classic fallback, color data and malformed inputs
test/smbprobe.c             SMB mount probe, driven by test/run-smbmount.sh
test/peer-drill.sh          what the second machine does to the Amiga
```

The user-facing text in the archive is `dist/ReadMe` and `docs/user/`. This file
is not shipped.

## Icons

The default `.info` files combine the original classic four-color images and
launch settings with 48x48, 128-color GlowIcons. Color-capable `icon.library` versions
display the new artwork; stock older libraries load the classic fallback.
Normal and selected color states have identical transparent bounds; the selected
state adds an inward gold rim while keeping interior pixels stationary. This adds
22,454 bytes across the seven icon templates and no library code.

The main drawer defaults to 620x236 pixels and fits a 640x256 Workbench.
Packaging places the main icons on a four-column grid. Private installations
put NetMeter and NetPrefs in the parent AmiNetXDuo drawer; command copies
remain in `C/` for CLI compatibility, without duplicate Workbench icons.
The commands drawer has a 340x116 window. Generic document/drawer
templates and the parent AmiNetXDuo icon stay unsnapped so reusing them in
other drawers does not give unrelated icons overlapping fixed positions.

Packaging needs only Python's standard library. To build a classic-only
archive, set `AMINETXDUO_ICON_STYLE=classic` when invoking `dist/make-dist.sh`.
For separate icon templates:

```sh
python3 install/tools/makeicon.py build/classic-icons --style classic
python3 install/tools/makeicon.py build/glow-icons --style glow
```

Artwork regeneration, including palette reduction and GlowIcon encoding:

```sh
python3 install/tools/compile-glowicons.py --outdir install/icons/glow \
    install/icons/glow/masters/*.png
python3 install/tools/makeicon.py install
python3 install/tools/showicon.py install/*.info
python3 install/test/test_glowicons.py
```

Only artwork regeneration requires Pillow. Each PNG master contains a normal
state on the left and a selected reference on the right. The compiler frames both
states together, reserves transparent pen zero and encodes IFF FACE/IMAG
chunks with byte-depth PackBits compression. Both color palettes are stored
explicitly. The independent validator checks chunk lengths, RLE expansion,
palette indices and both states, rather than accepting arbitrary trailing data.

`icontest` validates all seven icons and their metadata through the native
library. On V44+ it also validates both color states and checks the decoded
pixels/palettes are preserved by `PutDiskObject`. Optional arguments name an
icon directory and report file, allowing a RAM-only run:

```text
icontest RAM:GlowIcons RAM:glowicons-report.txt
```

On an old library a native write can retain only the classic representation;
the native test checks its metadata and fallback. Installing a newer icon set
again restores the color extension. MUI application images and MagicWB palette
icons are separate formats and are not part of this GlowIcon set.

## The script

The default profile uses installed RAM, including memory
currently allocated: micro through 2 MiB, minimal below 8 MiB, full from
8 MiB, or fat on Emu68 from 8 MiB. Intermediate and Expert can override it.
Exec V39+ provides `MEMF_TOTAL`; Kickstart 2.04/2.05 use the managed memory
header ranges under `Forbid()` instead, since their Exec ignores that flag.
The memory probe rounds boot reservations to the next 64 KiB. A failed query
recommends minimal; it never selects fat from a failed probe.

Network startup defaults to manual for micro, regardless of RAM, and for every
profile on machines with at most 4 MiB of installed RAM. A failed memory probe
also defaults to manual. Intermediate and Expert can enable automatic startup;
Novice takes the recommendation. Manual startup skips both bringing the network
up at the end of installation and adding its boot command. The final page gives
the `AddNetInterface` command to start the configured interface when needed.

Packaging removes comments and indentation with `tools/packscript.py` to
reduce Installer's input buffer. It checks that every token and its line
number remain identical; the readable script remains here. On 1 MiB rigs,
`installdrive.c` can use `DRIVE_INSTALL_STACK=12000UL` instead of the
conservative 40000-byte test stack. Workbench 3.2 itself requires 2 MiB;
test 1 MiB configurations with Kickstart/Workbench 3.1.

`PCMCIA/` carries unmodified Prism2/Xircom drivers, pccard.library, original
source archives and their redistribution notices. `tools/pcmcia-drivers.py`
checks pinned bytes before staging. Run `python3 install/test/test_pcmcia_package.py`
to check that missing or altered notices/source fail closed. Existing external
files are retained; uninstall compares installer-added files against its saved
copies and preserves files a user subsequently changes.

Written for Commodore's Installer, whose language is documented in
`installer.doc` (Commodore, 9 February 1993) and implemented in the Installer
2.17 sources. Where the two disagree, the sources win. `compile.c`'s symbol
tables are the authority for which keywords exist and which parameters are legal
in which statement.

Three properties of the tool shape the whole script.

**At NOVICE level every `ask...` returns its `(default)` without drawing
anything, and `(message ...)` prints nothing.** Novice mode therefore works only
if every default is independently correct. That is why network devices are
auto-detected from Emu68's device tree, Zorro Autoconfig, PCMCIA identity
tuples and UAE's resident device, with known drivers in `DEVS:` and
`DEVS:Networks` only as a fallback. The address mode defaults to DHCP. It is
also why no validation loop can depend on a `(message)` to make progress. At
level 0 that loop spins forever behind a blank screen. Each loop is satisfied
unconditionally when `@user-level` is 0.

**`(startup)` replaces the `;BEGIN AmiNetXDuo` .. `;END AmiNetXDuo` block in
`S:User-Startup` and leaves every other application's lines alone.** That is
what makes a second run of the installer safe.

**Two things in the language look like C and are not.** `install/tools/checkscript.py`
tests for both:

* `("fmt" a b)` takes only the *first* element as the format string. Adjacent
  string literals are not concatenated. A format spelled as several literals
  therefore formats the second literal's address through the first one's `%s`
  and silently drops the rest. Build long formats with `(cat ...)` and pass the
  variable.
* A `(choices ...)` list that does not fit on a page is silently truncated.
  `layout_box_gads()` creates fewer gadgets than there are choices, and
  `default_radio()` then marks one that does not exist. The installation dies on
  "askchoice: No choices selected" with no hint that the labels were too long.
  Keep radio labels under 22 characters and put the detail in the help text.
  Installer 2.17 has also been measured creating no gadgets at all for 14
  `askoptions` entries; the network-device page therefore uses ten two-column
  family checkmarks and short follow-up pages for family variants.

## Testing

```sh
install/test/run-workbench.sh -l AVERAGE -a build/dist/AmiNetXDuo-<version>.lha
install/test/run-workbench.sh -l AVERAGE -H -a <archive>   # the terminal arm
install/test/run-all.sh -a <archive>                       # release matrix
```

This is what `tools/ci.sh e2e` runs, and what `.github/workflows/emulator.yml`
calls in the release job. It stages a bare machine with a real Workbench 3.1,
runs the real Installer on the unpacked release archive, and power-cycles. The
stock Startup-Sequence must then reach `S:User-Startup` and bring the network up
on its own. Every scenario also mounts a populated `Work:` volume and checks
that the installer's destination remains `SYS:` without changing its contents.

`-l` is `NOVICE`, `AVERAGE` or `EXPERT`. `-H` makes the run three installs and
adds a second machine. It then checks four more things:

- that a pre-existing `S:User-Startup` survives
- that the managed block is replaced rather than appended to
- that the third install takes the added lines away again
- that the peer, while the machine is up, can fetch `/shell` and `/files`, PUT and GET a file
  byte-for-byte, and have the Amiga `lha x` the release archive

`AMINETXDUO_PEER` names that machine and has no default. The host that runs the
emulator cannot be it. Without a peer the run exits 3 rather than passing.

The in-guest driver also accepts `-q` answer files staged by
`run-workbench.sh`. Each non-comment line names one required action:

```
1|BOOL|No, leave them out
1|CHOICE|3|3|1
1|STRING|eth0|lan.1
1|ABORT|Yes, install them
```

The fields are Installer run number, action, and its operands. `CHOICE` uses
option count, gadget id and the zero-based ordinal among matching pages.
`ABORT` identifies the page by one of its button labels and clicks Installer's
real Abort gadget. Every action must be consumed or the run fails. This is what
lets the release matrix test several non-default answers together, prove a
cancellation writes nothing, and select different profiles on a reinstall
without silently falling back to Installer defaults.

The Emu68 rows replace `InstallNetProbe` only inside the throw-away test drive
with `install/test/netprobe-fixture.c`. This lets Amiberry exercise the
Installer's Ethernet-only, Wi-Fi-only and dual-device decisions; it does not
stand in for tests of the real probe. The real helper is also run under
Amiberry against emulated Zorro and PCMCIA hardware, so its expansion.library
and card.resource paths are exercised rather than inferred from source.

Ingredients, none of which are ours to ship:

| | |
|---|---|
| Workbench 3.1 ADFs | `~/amigaos/adf/amiga-wb31_{workbench,extras,fonts,locale,storage}.adf`, or `AMINETXDUO_ADF_DIR` |
| Kickstart 3.1 | `AMINETXDUO_KICKSTART`, else the A1200 40.68 image |
| Commodore Installer | `build/Installer`, or `AMINETXDUO_INSTALLER` |
| `a2065.device` | `build/a2065.device`, or `AMINETXDUO_A2065` |
| amitools' `xdftool` | `AMINETXDUO_XDFTOOL`, or on `$PATH` (`pip install amitools`) |
| `lha` | to unpack the archive on the host. Lhasa is sufficient |

`tests/HARNESSES` records the state of every harness here, including the ones
nothing invokes.

The Installer has no batch mode and its buttons have no keyboard shortcuts. For
that reason `installdrive` finds the Proceed gadget by its gadget ID and posts
to the window the GADGETUP that Intuition normally posts. See the comment at the
top of `test/installdrive.c`.
