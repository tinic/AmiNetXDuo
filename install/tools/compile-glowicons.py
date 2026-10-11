#!/usr/bin/env python3
"""Compile two-state transparent PNG masters into Amiga GlowIcon artwork.

Requires Pillow only when changing artwork. Packaging uses the checked-in
.glow files and the standard-library-only makeicon.py. Each master has two
equal cells horizontally (normal, selected). Shared framing keeps the states
aligned; one shared palette reserves pen zero for transparency.

SPDX-License-Identifier: MIT
"""

import argparse
from pathlib import Path
import struct

from PIL import Image


def values(image):
    # Pillow 12.1 replaced getdata; retain compatibility with older Pillow.
    reader = getattr(image, "get_flattened_data", image.getdata)
    return reader()


def chunk(kind, payload):
    return kind + struct.pack(">I", len(payload)) + payload + b"\0" * (len(payload) & 1)


def packbits(data):
    """Byte-depth GlowIcon RLE, with at most 128 entries per packet."""
    out = bytearray()
    i = 0
    while i < len(data):
        run = 1
        while i + run < len(data) and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes((257 - run, data[i]))
            i += run
            continue
        start = i
        i += run
        while i < len(data) and i - start < 128:
            if i + 2 < len(data) and data[i] == data[i + 1] == data[i + 2]:
                break
            i += 1
        out.append(i - start - 1)
        out += data[start:i]
    return bytes(out)


def compile_sheet(path, outdir, size):
    with Image.open(path) as source:
        sheet = source.convert("RGBA")
    if sheet.width % 2:
        raise ValueError(f"{path}: expected two equal horizontal cells")
    half = sheet.width // 2
    states = [sheet.crop((i * half, 0, (i + 1) * half, sheet.height)) for i in range(2)]
    bounds = [state.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox()
              for state in states]
    if any(box is None for box in bounds):
        raise ValueError(f"{path}: empty normal or selected state")
    left = min(box[0] for box in bounds)
    top = min(box[1] for box in bounds)
    right = max(box[2] for box in bounds)
    bottom = max(box[3] for box in bounds)
    scale = (size - 4) / max(right - left, bottom - top)
    dimensions = (max(1, round((right - left) * scale)),
                  max(1, round((bottom - top) * scale)))
    rendered = []
    for state in states:
        image = state.crop((left, top, right, bottom)).resize(dimensions, Image.Resampling.LANCZOS)
        frame = Image.new("RGBA", (size, size))
        frame.paste(image, ((size - dimensions[0]) // 2, (size - dimensions[1]) // 2))
        rendered.append(frame)

    # Quantize only opaque colors; transparent padding must not consume the
    # palette budget. Then map both states to that same fixed palette.
    colors = [pixel[:3] for state in rendered for pixel in values(state) if pixel[3] >= 128]
    samples = Image.new("RGB", (len(colors), 1))
    samples.putdata(colors)
    quantized = samples.quantize(colors=127, method=Image.Quantize.MEDIANCUT,
                                dither=Image.Dither.NONE)
    color_count = max(values(quantized)) + 1
    palette = b"\0\0\0" + bytes(quantized.getpalette()[:color_count * 3])
    indices = iter(values(quantized))
    payloads = []
    for name, state in zip(("normal", "selected"), rendered):
        pixels = bytes(next(indices) + 1 if pixel[3] >= 128 else 0 for pixel in values(state))
        packed = packbits(pixels)
        compressed = len(packed) < len(pixels)
        image_data = packed if compressed else pixels
        header = struct.pack(">BBBBBBHH", 0, color_count, 3, int(compressed), 0, 8,
                             len(image_data) - 1, len(palette) - 1)
        payloads.append(chunk(b"IMAG", header + image_data + palette))
        preview = Image.frombytes("P", (size, size), pixels)
        preview.putpalette(palette + b"\0" * (768 - len(palette)))
        preview.save(outdir / (path.stem + "-" + name + ".png"), transparency=0, optimize=True)
    face = chunk(b"FACE", struct.pack(">BBBBH", size - 1, size - 1, 1, 0, len(palette) - 1))
    body = b"ICON" + face + b"".join(payloads)
    target = outdir / (path.stem + ".glow")
    target.write_bytes(b"FORM" + struct.pack(">I", len(body)) + body)
    print(f"{target}: {target.stat().st_size} bytes, {size}x{size}, {color_count + 1} colors")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=48)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("sheets", type=Path, nargs="+")
    args = parser.parse_args()
    if not 8 <= args.size <= 256:
        parser.error("size must be 8..256 pixels")
    args.outdir.mkdir(parents=True, exist_ok=True)
    for path in args.sheets:
        compile_sheet(path, args.outdir, args.size)


if __name__ == "__main__":
    main()
