# GlowIcon artwork

Six original designs, used by seven icon templates (`Guide` shares the document
artwork). Each icon has 48x48 normal and selected images with a shared 128-color
palette and transparent pen zero. The selected state uses a golden outline.

The transparent PNG masters were generated with Codex's built-in image tool on
2026-10-11. They are original artwork, not copies of Commodore or third-party
icon packs. The tool does not expose a model selector; no specific underlying
model is claimed. The earlier model recommendation was separate from the tool
actually used here.

## Prompt set

Common direction: a transparent, horizontal, two-state Amiga Workbench
GlowIcon sprite sheet; normal on the left and selected on the right. Identical
object geometry, equal scale and padding. Hand-painted late-1990s style, blue
enamel, silver edging, dark outlines, top-left lighting and slight three-quarter
perspective. Bold shapes readable at 48x48 pixels. The selected image adds a
restrained golden rim. No text, numbers, labels, logos, checkerboard background,
extra objects, floor or external cast shadow.

Subjects:

- **NetPrefs:** compact network preferences control box, two silver Ethernet
  sockets and two chunky adjustment sliders. This first image was the style
  reference for NetMeter, Installer and Document.
- **NetMeter:** upright network traffic instrument, white semicircular dial,
  green needle and opposing green/cyan upload/download arrows.
- **AmiNetXDuo:** open blue file drawer, curved silver handle, blue-and-green
  globe rising from inside and a silver equatorial arc. NetPrefs was the style
  reference; this image became the style reference for the generic drawer.
- **Installer:** bold green downward arrow entering the slot in a blue metal
  installation tray.
- **Drawer:** open blue file drawer, curved silver handle and two off-white
  folders inside, without the network globe.
- **Document:** white sheet with folded upper-right corner, blue globe,
  three blue lines and thin blue backing edge. Used for ReadMe and AmigaGuide.

## Regeneration

See `install/README.md`. `compile-glowicons.py` converts these masters to
palette-indexed PNG previews and `.glow` IFF data. `makeicon.py` appends that
data to complete classic `.info` files. Master and preview PNGs are development
assets; only the `.info` files enter the release archive.

Format references:

- [AROS native GlowIcon reader/writer](https://github.com/aros-development-team/AROS/blob/master/workbench/libs/icon/diskobj35io.c)
- [Iconic icon format documentation](https://www.hollywood-mal.com/docs/pdf/iconic.pdf)

The encoder was written for this project from the format documentation. It
does not vendor the AROS implementation. The independent parser accepts the
specific FACE/two-IMAG representation we ship, rather than every historical
extension or image depth.
