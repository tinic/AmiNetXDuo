# install/

The Installer script, the example configuration files, and the tooling that
proves both of them work.

```
Install-AmiNetXDuo          the Installer script
Install-AmiNetXDuo.info     its icon: project, default tool "Installer"
AmiNetXDuo.info             drawer icon
Drawer.info Document.info   generic icons for Docs/, Examples/, ReadMe
Guide.info                  icon for AmiNetXDuo.guide, default tool "MultiView"

devs/Internet/              protocols, services, networks -- shipped as-is
examples/NetInterfaces/     one commented interface file per card
examples/Internet/          routes, name_resolution, hosts

tools/makeicon.py           writes the .info files
tools/showicon.py           reads one back and draws it, independently
tools/checkscript.py        static checks on the Installer script

test/installdrive.c         drives the Installer under FS-UAE
test/bootcheck.c            boots the installed machine and checks it works
test/run-installer-fsuae.sh one scenario
test/run-all.sh             all five
```

`dist/make-dist.sh` assembles all of it into the distribution archive.

## The script

Written for Commodore's Installer, whose language is documented in
`installer.doc` (Commodore, 9 February 1993) and implemented in the Installer
2.17 sources.  Where the two disagree, the sources win -- `compile.c`'s
symbol tables are the authority for which keywords exist and which parameters
are legal in which statement.

Three properties of the tool shape the whole script and are worth knowing
before editing it:

**At NOVICE level every `ask...` returns its `(default)` without drawing
anything, and `(message ...)` prints nothing.**  Novice mode therefore only
works if every default is independently correct, which is why the network
card is auto-detected by looking for known drivers in `DEVS:` and
`DEVS:Networks` and why the address mode defaults to DHCP.  It is also why no
validation loop may depend on a `(message)` to make progress: at level 0 the
loop would spin forever behind a blank screen.  Each one is satisfied
unconditionally when `@user-level` is 0.

**`(startup)` replaces the `;BEGIN AmiNetXDuo` .. `;END AmiNetXDuo` block in
`S:User-Startup` and leaves every other application's lines alone.**  That is
what makes re-running the installer safe.

**Two things in the language look like C and are not.**  Both were found by
running the thing, not by reading about it, and `tools/checkscript.py` now
tests for both:

* `("fmt" a b)` takes only the *first* element as the format string.
  Adjacent string literals are not concatenated, so a format spelled as
  several literals formats the second literal's address through the first
  one's `%s` and silently drops the rest.  Build long formats with `(cat ...)`
  and pass the variable.
* A `(choices ...)` list that does not fit on a page is silently truncated:
  `layout_box_gads()` creates fewer gadgets than there are choices,
  `default_radio()` then marks one that does not exist, and the installation
  dies on "askchoice: No choices selected" with no hint that the labels were
  too long.  Keep radio labels under about 22 characters and put the detail
  in the help text.

## Testing

```sh
install/test/run-all.sh
```

Needs, besides the usual FS-UAE setup:

* `build/Installer` or `AMINETXDUO_INSTALLER=<path>` -- Commodore's Installer,
  which is in the root of the Workbench 3.1 Install disk.  Not ours to ship.
* `build/a2065.device` or `AMINETXDUO_A2065=<path>`.

Each scenario stages a bare machine -- an empty `LIBS:`, a `DEVS:` holding
only the card driver, an empty `S:` -- plus the unpacked distribution
archive, and runs the real Installer on it.  FS-UAE mounts the staging
directory as a hard drive, so everything the Installer writes lands on the
host and is checked there.  Then, for every scenario that produces a
DHCP configuration, the machine is booted again with an emulated A2065 on
SLIRP and `bootcheck` runs whatever the installer put in `S:User-Startup`
and requires an address to arrive.

The Installer has no batch mode and its buttons have no keyboard shortcuts,
so `installdrive` finds the Proceed gadget by its gadget ID and posts the
window the GADGETUP that Intuition would have posted.  See the comment at the
top of `test/installdrive.c`.
