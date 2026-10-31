# Live Wi-Fi status icon

NetWireless draws its AppIcon procedurally in `src/tools/netwireless_sig.c`.
It is separate from the launcher artwork in `../glow/` and needs no external
image files at runtime.

The 32x32 radio symbol uses eight neutral greys. Its dot and three arcs show
four strength levels. Unavailable, off and error have small shape badges;
not connected has hollow arcs. Normal and selected color images have the same
opaque silhouette. Workbench before V44 uses the same geometry on its own
background, foreground and light pens.

`wifi-preview.png` shows pixels from the actual C renderer on sample light,
dark and selected backgrounds, plus the classic Workbench palette, enlarged
2x without filtering. It is a code-rendered preview, not a Workbench capture.
