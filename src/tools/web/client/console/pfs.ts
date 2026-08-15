/*
 * .pfs -- a planar frame sequence, straight off the capture side.
 *
 * The player exists before the Amiga does.  A file of frames in exactly the
 * layout the capture hands over is the only way to work on the planar path,
 * the palette and the aspect correction without an emulator in the loop, and
 * it stays useful afterwards as the thing you open when a live session drew
 * something wrong and you want to look at the bytes again.
 *
 * There is no compression and no per-frame header on purpose: a capture is
 * written by whatever is fastest to write on a 68020 and read by a machine
 * that does not care, and a container that needs a decoder is a container
 * that can disagree with the wire format about what a frame is.
 *
 *   0   char magic[4] = "PFS1"
 *   4   u16  width
 *   6   u16  height
 *   8   u8   depth            1..8
 *   9   u8   flags
 *   10  u16  bytesPerRow      NOT width/8
 *   12  u16  frameCount
 *   14  u16  reserved
 *   16  palette, 3*(1<<depth) bytes, RGB8
 *   ..  frameCount frames of depth*bytesPerRow*height bytes, plane-major
 *
 * Big-endian throughout, because the machine that writes it is.
 *
 * SPDX-License-Identifier: MIT
 */

import {
  frameBytes,
  palette32,
  screenFault,
  type Screen,
} from "./planar";

export const PFS_HEADER = 16;

export interface Capture {
  readonly screen: Screen;
  readonly flags: number;
  readonly frameCount: number;
  /* Kept as well as the 32-bit form: the header panel shows the entries and
     an exporter would want the bytes back. */
  readonly rgb: Uint8Array;
  readonly palette: Uint32Array;
  readonly frames: Uint8Array;
  readonly stride: number;
}

export function frameAt(c: Capture, i: number): number {
  return i * c.stride;
}

/* The most a frameCount field can hold, which is what bounds one file. */
export const PFS_MAX_FRAMES = 0xffff;

/*
 * The other direction: planar frames the viewer already holds, into the bytes
 * parsePfs reads back.
 *
 * Here rather than in the recorder because the two have to agree byte for
 * byte and this is the file that says what the layout is.  The frames go in
 * as they are -- plane-major, bytesPerRow strides, the same buffer the
 * decoder wrote and the canvas was painted from -- so a recording is what was
 * on the screen and not a re-planarisation of what was drawn from it.
 */
export function buildPfs(screen: Screen, rgb: Uint8Array,
                         frames: readonly Uint8Array[]) {
  const fault = screenFault(screen);
  if (fault !== null) throw new Error("cannot write a .pfs with " + fault);
  if (frames.length === 0) throw new Error("no frames to write");
  if (frames.length > PFS_MAX_FRAMES) {
    throw new Error(frames.length + " frames; the header counts to " +
                    PFS_MAX_FRAMES);
  }

  const palBytes = 3 * (1 << screen.depth);
  if (rgb.length < palBytes) {
    throw new Error("palette is " + rgb.length + " bytes, depth " +
                    screen.depth + " needs " + palBytes);
  }

  const stride = frameBytes(screen);
  const out = new Uint8Array(PFS_HEADER + palBytes + frames.length * stride);
  const v = new DataView(out.buffer);

  out[0] = 0x50; out[1] = 0x46; out[2] = 0x53; out[3] = 0x31;
  v.setUint16(4, screen.width);
  v.setUint16(6, screen.height);
  out[8] = screen.depth;
  out[9] = 0;
  v.setUint16(10, screen.bytesPerRow);
  v.setUint16(12, frames.length);
  v.setUint16(14, 0);
  out.set(rgb.subarray(0, palBytes), PFS_HEADER);

  let at = PFS_HEADER + palBytes;
  for (const f of frames) {
    if (f.length !== stride) {
      throw new Error("a frame is " + f.length + " bytes, this screen is " +
                      stride);
    }
    out.set(f, at);
    at += stride;
  }

  return out;
}

/*
 * Parse, or throw a sentence a person can act on.
 *
 * Every failure here is somebody having opened the wrong file or a capture
 * that was cut off mid-write, and both want to be told which -- a viewer that
 * answers "invalid file" sends you to read the writer's source.
 */
export function parsePfs(buf: ArrayBuffer): Capture {
  const b = new Uint8Array(buf);

  if (b.length < PFS_HEADER) {
    throw new Error("only " + b.length + " bytes, a .pfs header is 16");
  }
  if (b[0] !== 0x50 || b[1] !== 0x46 || b[2] !== 0x53 || b[3] !== 0x31) {
    throw new Error("not a .pfs: the first four bytes are not PFS1");
  }

  const v = new DataView(buf);
  const screen: Screen = {
    width: v.getUint16(4),
    height: v.getUint16(6),
    depth: b[8],
    bytesPerRow: v.getUint16(10),
  };
  const flags = b[9];
  const frameCount = v.getUint16(12);

  const fault = screenFault(screen);
  if (fault !== null) throw new Error("the header says " + fault);
  if (frameCount === 0) throw new Error("the header says no frames");

  const palBytes = 3 * (1 << screen.depth);
  const stride = frameBytes(screen);
  const want = PFS_HEADER + palBytes + frameCount * stride;

  if (b.length < want) {
    /* Said as frames rather than bytes: a capture that was still being
       written is the common case and "17 of 200 frames" is the useful form
       of it. */
    const have = Math.max(0, Math.floor((b.length - PFS_HEADER - palBytes) / stride));
    throw new Error("truncated: " + have + " of " + frameCount +
                    " frames are present (" + b.length + " bytes, " +
                    want + " expected)");
  }

  const rgb = b.subarray(PFS_HEADER, PFS_HEADER + palBytes);

  return {
    screen,
    flags,
    frameCount,
    rgb,
    palette: palette32(rgb, screen.depth),
    frames: b.subarray(PFS_HEADER + palBytes, want),
    stride,
  };
}
