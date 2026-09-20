# install/

The Installer script, the configuration files that the archive ships, and the
tooling that proves both work. `dist/make-dist.sh` assembles all of it into the
distribution archive.

```
Install-AmiNetXDuo          the Installer script
Install-AmiNetXDuo.info     its icon: project, default tool "Installer"
Installer                   Commodore's Installer, bundled so an install
                            needs nothing else on the machine
AmiNetXDuo.info             drawer icon
Drawer.info Document.info   generic icons for Docs/, Examples/, ReadMe
Guide.info                  icon for AmiNetXDuo.guide, default tool "MultiView"

devs/Internet/              protocols, services, networks, shipped as-is
examples/NetInterfaces/     one commented interface file per card
examples/Internet/          routes, name_resolution, hosts, service_discovery

tools/makeicon.py           writes the .info files
tools/showicon.py           reads one back and draws it, independently
tools/checkscript.py        static checks on the Installer script

test/run-workbench.sh       installs the archive on a real Workbench 3.1
test/installdrive.c         drives the Installer inside the guest
test/bootcheck.c            boots the installed machine and checks it works
test/icontest.c             hands the generated .info files to icon.library
test/smbprobe.c             SMB mount probe, driven by test/run-smbmount.sh
test/peer-drill.sh          what the second machine does to the Amiga
```

The user-facing text in the archive is `dist/ReadMe` and `docs/user/`. This file
is not shipped.

## The script

Written for Commodore's Installer, whose language is documented in
`installer.doc` (Commodore, 9 February 1993) and implemented in the Installer
2.17 sources. Where the two disagree, the sources win. `compile.c`'s symbol
tables are the authority for which keywords exist and which parameters are legal
in which statement.

Three properties of the tool shape the whole script.

**At NOVICE level every `ask...` returns its `(default)` without drawing
anything, and `(message ...)` prints nothing.** Novice mode therefore works only
if every default is independently correct. That is why the network card is
auto-detected from known drivers in `DEVS:` and `DEVS:Networks`, and why the
address mode defaults to DHCP. It is also why no validation loop can depend on a
`(message)` to make progress. At level 0 that loop spins forever behind a blank
screen. Each loop is satisfied unconditionally when `@user-level` is 0.

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
on its own.

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
```

The fields are Installer run number, action, and its operands. `CHOICE` uses
option count, gadget id and the zero-based ordinal among matching pages. Every
action must be consumed or the run fails. This is what lets the release matrix
test several non-default answers together and select different profiles on a
reinstall without silently falling back to Installer defaults.

The Emu68 rows replace `InstallNetProbe` only inside the throw-away test drive
with `install/test/netprobe-fixture.c`. This lets Amiberry exercise the
Installer's Ethernet-only, Wi-Fi-only and dual-device decisions; it does not
stand in for the separate tests of the real probe and drivers on Emu68.

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
