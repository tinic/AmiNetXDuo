# GlowIcon artwork

Original designs for the AmiNetXDuo launcher icons. `Guide` shares the document artwork; NetWireless,
NetWirelessPrefs and Uninstall-AmiNetXDuo add artwork for the wireless tools
and removal action. Each icon has 48x48 normal and selected images with a shared 128-color
palette and transparent pen zero. The selected state is derived from the normal image with an inward golden
outline. Its opaque silhouette and interior pixel positions are identical,
preventing a jump when Workbench selects it. The original second master cell
is retained as an artwork reference; it no longer supplies selection pixels.

The initial transparent PNG masters were generated with Codex's built-in image tool on
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

## Wireless and uninstall additions (2026-10-11)

Generated using the built-in image tool with real transparency. NetPrefs was
the style reference for NetWireless; NetWireless was the reference for
NetWirelessPrefs; Install-AmiNetXDuo was the reference for Uninstall.
The existing encoder and stationary selected-state derivation are unchanged.
Compiled previews are shown in `wireless-uninstall-preview.png`.
These artwork assets are independent of launch tooltypes and Installer wiring.

Exact prompts used for the three new masters:

### NetWireless

Use case: stylized-concept. Create an original transparent PNG master sprite sheet for the Amiga Workbench NetWireless GlowIcon. The attached image is a STYLE REFERENCE ONLY for blue enamel, silver edging, dark outlines, top-left lighting, hand-painted late-1990s look and slight three-quarter perspective. New subject: a compact blue-and-silver Wi-Fi antenna on a small blue metal base, with two bold silver/cyan radio arcs above it. Bold simple silhouette readable at 48x48 pixels. Exactly two equal horizontal square cells, normal on left, selected on right. The antenna/base/radio arcs must be identical geometry, scale, orientation and framing in both cells, centered with generous equal transparent padding. Selected adds a restrained thin golden rim. No router ports or sliders, no text, labels, logos, checkerboard, floor, extra objects or external cast shadow. Genuinely transparent background. The final code derives the selected state from normal, so prioritize the left normal image and keep the second image at identical dimensions.

### NetWirelessPrefs

Use case: stylized-concept. Create an original transparent PNG master sprite sheet for Amiga Workbench NetWirelessPrefs GlowIcon. The attached NetWireless icon is a STYLE AND SUBJECT REFERENCE, not an edit target. Match its hand-painted late-1990s blue enamel, silver edging, dark outlines, top-left lighting and slight three-quarter perspective. Subject: the same recognizable compact blue-and-silver Wi-Fi antenna on a small blue metal base with two bold silver/cyan radio arcs; add one prominent chunky silver settings gear with a blue hub at the lower-right front of the base. The gear must remain clearly readable at 48x48; do not obscure the radio arcs. Exactly two equal horizontal square cells, normal left, selected right, identical antenna/base/arcs/gear geometry, scale, orientation and framing in both cells, centered with generous equal transparent padding. Selected adds a restrained thin golden rim. No text, labels, logos, checkerboard, floor, unrelated extra objects or external cast shadow. Genuinely transparent background. Our encoder derives stationary selection from normal, so prioritize a clean left normal icon.

### Uninstall-AmiNetXDuo

Use case: stylized-concept. Create an original transparent PNG master sprite sheet for Amiga Workbench Uninstall-AmiNetXDuo GlowIcon. The attached Install-AmiNetXDuo image is a STYLE REFERENCE ONLY, not an edit target. Match its blue enamel metal package/tray, silver edging, dark outlines, top-left lighting, hand-painted late-1990s look and slight three-quarter perspective. New subject: a compact blue/silver software package box with one very bold orange-red removal arrow pointing outward to the RIGHT, leaving the package, unmistakably different from the attached green downward installation arrow. Simple clean silhouette readable at 48x48. Exactly two equal horizontal square cells: normal left, selected right. Package and rightward arrow have identical geometry, scale, orientation and framing in both cells, centered with generous equal transparent padding. Selected adds a restrained thin golden rim. No downward green arrow, no trash bin, no text, labels, logos, checkerboard, floor, extra unrelated objects or external cast shadow. Genuinely transparent background. Our encoder derives stationary selection from normal, so prioritize a clean left normal icon.

## NetEnv addition (2026-10-31)

The built-in image tool generated the transparent two-cell NetEnv master
using NetPrefs as the style reference. A network port feeds three value
cards, representing configuration exported to other desktop programs.
NetEnv has its own classic image and compiled GlowIcon instead of borrowing
NetSwitch. Launch tooltypes and the 8192-byte stack are unchanged.

Normal and selected color images still come from the existing stationary
selection algorithm. The compiler pads unused palette entries to retain
the 128-entry shipping format when quantization needs fewer colors; it
does not add or change image pixels. `NetEnv-preview.png` compares the
compiled normal/selected images and the classic fallback at 3x scale.

Exact generation prompt:

Use case: stylized-concept. Asset: original NetEnv launcher GlowIcon for an Amiga
Workbench network suite. The attached NetPrefs image is a STYLE REFERENCE ONLY: match
the glossy cobalt-blue enamel, broad silver chrome trim, dark outlines, upper-left
lighting, hand-painted late-1990s Workbench style and slight three-quarter view. NetEnv
publishes network address/settings values for other desktop programs; it is not an
editor, switch, wireless antenna or traffic meter. New subject: a compact blue chrome-
edged network module, with one bold network-node symbol on its front, feeding THREE
small off-white value cards to the right through a simple clear branching silver
connector. The three cards each have just one short dark-blue horizontal mark; no actual
text or numbers. One unified balanced object, very simple bold silhouette, legible when
reduced to 48x48 pixels. No toggle levers, sliders, gear, Wi-Fi arcs, cloud, plant or
globe scenery. Exactly TWO EQUAL horizontal square cells: normal icon on left, same icon
selected on right with restrained thin golden rim. Identical geometry, scale,
perspective and placement in both cells, generous equal transparent padding and all
content fully inside each half. Genuinely transparent background, no checkerboard,
floor, exterior cast shadow, labels, logos, watermarks or extra unrelated objects. Our
compiler derives stationary selection from normal, so prioritize the left normal image.
