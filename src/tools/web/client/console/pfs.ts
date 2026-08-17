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
 *   0   char magic[4] = "PFS2"
 *   4   u16  width
 *   6   u16  height
 *   8   u8   depth            1..8 planar, 8 CLUT8, 16 RGB565
 *   9   u8   format           rfb_geom.format: 0 planar, 1 CLUT8, 2 RGB565
 *   10  u16  bytesPerRow      NOT width/8
 *   12  u16  frameCount
 *   14  u16  pointerCount     pointer images at the end; 0 when there are none
 *   16  palette, 3*(1<<depth) bytes RGB8, and NOTHING AT ALL on RGB565
 *   ..  frameCount frames of planes*bytesPerRow*height bytes, plane-major,
 *          where planes is depth planar and 1 on both RTG formats
 *   ..  frameCount frame records of 12 bytes:
 *          0  u32  milliseconds from the first frame
 *          4  s16  pointer x, in SCREEN pixels
 *          6  s16  pointer y
 *          8  u16  pointer image, 1-based; 0 is "no pointer on this frame"
 *          10 u16  reserved
 *   ..  pointerCount pointer images, each:
 *          0  u16  bytes in this record, this field included
 *          2  u16  width, in SPRITE pixels
 *          4  u16  height, in rows
 *          6  u8   depth, in planes
 *          7  u8   xScale     screen pixels per sprite pixel: 1, 2 or 4
 *          8  u8   yScale     screen rows per sprite row: 1 or 2
 *          9  u8   reserved
 *          10 s16  hotspot x, in sprite pixels
 *          12 s16  hotspot y
 *          14 u16  reserved
 *          16 colours, 3 * ((1 << depth) - 1) bytes RGB8; index 0 is
 *                 transparent and has no entry
 *          ..  depth * ((width + 15) / 16 * 2) * height bytes, plane-major
 *
 * Big-endian throughout, because the machine that writes it is.
 *
 * WHY BYTE 9 IS A FORMAT AND NOT A SET OF FLAGS
 *
 *   It was documented as flags and only ever carried bit 0, for the chunky
 *   RTG capture; every writer that has ever existed put a 0 or a 1 there.  A
 *   third format needs a third value, and the number the rest of the system
 *   already has for it is rfb_geom.format -- so the byte holds that, 0 and 1
 *   keep meaning exactly what they meant, and every file already written
 *   loads with no special case.  A value this does not know is refused rather
 *   than read as planar, which is the one thing a flags byte could not do.
 *
 *   The only arithmetic a format moves is the palette's length: RGB565 has no
 *   palette and none is written, so its frames begin at offset 16.  The frame
 *   records, the pointer table and pointerCount are all still found from
 *   frameCount * frameBytes and are unchanged.
 *
 * WHY THERE ARE TIMESTAMPS AT ALL
 *
 *   A live recording has no frame rate.  An idle screen produces a frame every
 *   few seconds and a window opening produces a burst, so replaying at any
 *   fixed rate is a lie about what happened -- and the fps gadget the player
 *   used to carry existed only because the file could not say.  Now it can,
 *   and the gadget is gone.
 *
 * WHY THE POINTER IS IN HERE AT ALL
 *
 *   It is a hardware sprite and it is NOT in the bitmap, so a recording that
 *   carries only the planes loses it completely -- and what somebody was
 *   pointing at, clicking and dragging is most of what a screen recording is
 *   for.
 *
 *   Position is per frame because it moves every frame.  The IMAGE is in a
 *   table because it hardly ever changes: a busy pointer or an application's
 *   own pointer is one more entry, not one more copy per frame.
 *
 *   The image is stored UNSCALED, in sprite pixels, with the scale beside it,
 *   because a sprite pixel is a lores pixel whatever the screen is: on a hires
 *   screen it covers two screen pixels across, on superhires four, and on an
 *   interlaced screen two rows down.  Storing the truth and the rule means the
 *   player and the live viewer scale from one source and cannot disagree.
 *
 *   Sprite colours are carried explicitly.  They come from the sprite's own
 *   palette entries -- 17, 18 and 19 for the pointer on a standard screen --
 *   and NOT from the screen's first four, so a reader that took them out of
 *   the screen palette would draw the wrong colours.  Index 0 is transparent
 *   and has no entry, which is why the table is one short.
 *
 * WHY THE TABLES ARE AT THE END
 *
 *   So a reader that only wants pixels is unchanged: the frames still begin at
 *   a fixed offset and still sit at a constant stride, which is what every
 *   decoder here indexes with and what lets a frame be a subarray rather than
 *   a copy.  It is also what a STREAMING writer needs -- wbgrab writes frames
 *   as it takes them and may stop early, and a table appended after the last
 *   frame is trivially the right length, where one reserved up front would
 *   have to be compacted.
 *
 *   Milliseconds, unsigned 32-bit, from the FIRST frame, so frame 0 is always
 *   0 and a recording can run for 49 days.  Non-decreasing; two frames may
 *   share a millisecond.
 *
 * SPDX-License-Identifier: MIT
 */

import {
  FMT_CLUT8,
  frameBytes,
  palColours,
  palette32,
  screenFault,
  screenFormat,
  type Screen,
} from "./planar";

export const PFS_HEADER = 16;

/* The palette a header carries, which is none at all on a format that has no
   palette to carry.  Both the writer and the reader place the frames from
   this, so it is the one place the offset is decided. */
export function pfsPaletteBytes(s: Screen): number {
  return 3 * palColours(s);
}

/* One frame record: when, where the pointer was, and which image it was. */
export const PFS_FRAMEREC = 12;

/* The fixed part of a pointer image record, before its colours and bits. */
export const PFS_PTRHEAD = 16;

/* One pointer image, as the file carries it and as the viewer receives it
   live: sprite pixels, with the rule for turning them into screen pixels. */
export interface Pointer {
  readonly width: number;         /* sprite pixels                       */
  readonly height: number;        /* rows                                */
  readonly depth: number;         /* planes                              */
  readonly xScale: number;        /* screen pixels per sprite pixel      */
  readonly yScale: number;        /* screen rows per sprite row          */
  readonly hotX: number;          /* sprite pixels, from the left        */
  readonly hotY: number;
  /* 3 * ((1 << depth) - 1) bytes: index 0 is transparent and absent. */
  readonly rgb: Uint8Array;
  /* depth * ((width + 15) / 16 * 2) * height, plane-major. */
  readonly bits: Uint8Array;
}

/* Where the pointer was on one frame.  `image` is 1-based into the capture's
   pointer table; 0 means there was no pointer to draw. */
export interface PointerAt {
  readonly x: number;
  readonly y: number;
  readonly image: number;
}

export function pointerRowBytes(p: { width: number }): number {
  return Math.floor((p.width + 15) / 16) * 2;
}

export function pointerBits(p: { width: number; height: number; depth: number }): number {
  return pointerRowBytes(p) * p.height * p.depth;
}

export interface Capture {
  readonly screen: Screen;
  /* Header byte 9 as it stands, which is screen.format and is kept here
     because a reader looking at a file wants the byte and not the reading. */
  readonly flags: number;
  readonly frameCount: number;
  /* Kept as well as the 32-bit form: the header panel shows the entries and
     an exporter would want the bytes back.  Both are empty on a truecolour
     capture, which has no palette in the file. */
  readonly rgb: Uint8Array;
  readonly palette: Uint32Array;
  readonly frames: Uint8Array;
  readonly stride: number;
  /* Milliseconds from the first frame, one per frame.  frameCount long, and
     times[0] is 0. */
  readonly times: Uint32Array;
  /* Where the pointer was on each frame.  frameCount long. */
  readonly pointerAt: PointerAt[];
  /* The images the frames reference, 1-based: pointers[0] is image 1. */
  readonly pointers: Pointer[];
}

/* How long the whole recording ran, in milliseconds. */
export function pfsDuration(c: Capture): number {
  return c.frameCount === 0 ? 0 : c.times[c.frameCount - 1];
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
                         frames: readonly Uint8Array[],
                         times: readonly number[],
                         pointerAt: readonly PointerAt[] = [],
                         pointers: readonly Pointer[] = []) {
  const fault = screenFault(screen);
  if (fault !== null) throw new Error("cannot write a .pfs with " + fault);
  if (frames.length === 0) throw new Error("no frames to write");
  if (frames.length > PFS_MAX_FRAMES) {
    throw new Error(frames.length + " frames; the header counts to " +
                    PFS_MAX_FRAMES);
  }
  if (times.length !== frames.length) {
    throw new Error(times.length + " timestamps for " + frames.length +
                    " frames");
  }

  const palBytes = pfsPaletteBytes(screen);
  if (rgb.length < palBytes) {
    throw new Error("palette is " + rgb.length + " bytes, " +
                    palColours(screen) + " colours need " + palBytes);
  }

  for (const p of pointers) {
    if (p.rgb.length < 3 * ((1 << p.depth) - 1)) {
      throw new Error("a pointer image of depth " + p.depth + " needs " +
                      3 * ((1 << p.depth) - 1) + " colour bytes");
    }
    if (p.bits.length !== pointerBits(p)) {
      throw new Error("a pointer image is " + p.bits.length +
                      " bytes, its shape needs " + pointerBits(p));
    }
  }

  const stride = frameBytes(screen);
  let ptrBytes = 0;
  for (const p of pointers) {
    ptrBytes += PFS_PTRHEAD + 3 * ((1 << p.depth) - 1) + pointerBits(p);
  }

  const out = new Uint8Array(PFS_HEADER + palBytes +
                             frames.length * (stride + PFS_FRAMEREC) +
                             ptrBytes);
  const v = new DataView(out.buffer);

  out[0] = 0x50; out[1] = 0x46; out[2] = 0x53; out[3] = 0x32;
  v.setUint16(4, screen.width);
  v.setUint16(6, screen.height);
  out[8] = screen.depth;
  out[9] = screenFormat(screen);
  v.setUint16(10, screen.bytesPerRow);
  v.setUint16(12, frames.length);
  v.setUint16(14, pointers.length);
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

  /* Rebased on the first, so frame 0 is 0 whatever clock the caller kept. */
  const base = times[0];
  let last = 0;
  for (let i = 0; i < times.length; i++) {
    let t = Math.round(times[i] - base);
    if (!Number.isFinite(t) || t < last) t = last;      /* never goes back */
    if (t > 0xffffffff) t = 0xffffffff;
    last = t;

    const p = pointerAt[i];
    v.setUint32(at, t);
    v.setInt16(at + 4, p === undefined ? 0 : clampS16(p.x));
    v.setInt16(at + 6, p === undefined ? 0 : clampS16(p.y));
    v.setUint16(at + 8, p === undefined ? 0 : p.image);
    v.setUint16(at + 10, 0);
    at += PFS_FRAMEREC;
  }

  for (const p of pointers) {
    const cols = 3 * ((1 << p.depth) - 1);
    v.setUint16(at, PFS_PTRHEAD + cols + pointerBits(p));
    v.setUint16(at + 2, p.width);
    v.setUint16(at + 4, p.height);
    out[at + 6] = p.depth;
    out[at + 7] = p.xScale;
    out[at + 8] = p.yScale;
    out[at + 9] = 0;
    v.setInt16(at + 10, clampS16(p.hotX));
    v.setInt16(at + 12, clampS16(p.hotY));
    v.setUint16(at + 14, 0);
    out.set(p.rgb.subarray(0, cols), at + PFS_PTRHEAD);
    out.set(p.bits, at + PFS_PTRHEAD + cols);
    at += PFS_PTRHEAD + cols + p.bits.length;
  }

  return out;
}

/* A pointer that walked off the left of an overscan screen has a negative
   position, and one on a 1280-wide screen has a big one; neither is a reason
   to refuse a recording. */
function clampS16(v: number): number {
  const n = Math.round(v);
  if (!Number.isFinite(n)) return 0;
  return n < -32768 ? -32768 : n > 32767 ? 32767 : n;
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
  if (b[0] !== 0x50 || b[1] !== 0x46 || b[2] !== 0x53 || b[3] !== 0x32) {
    throw new Error("not a .pfs: the first four bytes are not PFS2");
  }

  const v = new DataView(buf);
  const flags = b[9];
  const screen: Screen = {
    width: v.getUint16(4),
    height: v.getUint16(6),
    depth: b[8],
    bytesPerRow: v.getUint16(10),
    /* The byte was reserved and zero before there was an RTG capture and 1
       once there was, which are the same two numbers this reads now, so every
       file written before it was a format still says what it always said.
       screenFault() below refuses one this does not know rather than drawing
       it as the planar screen it is not. */
    format: flags,
    chunky: flags === FMT_CLUT8,
  };
  const frameCount = v.getUint16(12);
  const pointerCount = v.getUint16(14);

  const fault = screenFault(screen);
  if (fault !== null) throw new Error("the header says " + fault);
  if (frameCount === 0) throw new Error("the header says no frames");

  const palBytes = pfsPaletteBytes(screen);
  const stride = frameBytes(screen);
  const pixels = PFS_HEADER + palBytes + frameCount * stride;
  const want = pixels + frameCount * PFS_FRAMEREC;

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

  /* Read out rather than viewed in place: the tables are big-endian and a
     Uint32Array view would read them in the host's order, which is the wrong
     answer on every machine this runs on. */
  const times = new Uint32Array(frameCount);
  const pointerAt: PointerAt[] = [];
  for (let i = 0; i < frameCount; i++) {
    const at = pixels + i * PFS_FRAMEREC;
    times[i] = v.getUint32(at);
    pointerAt.push({
      x: v.getInt16(at + 4),
      y: v.getInt16(at + 6),
      image: v.getUint16(at + 8),
    });
  }

  const pointers: Pointer[] = [];
  let at = want;
  for (let i = 0; i < pointerCount; i++) {
    if (at + PFS_PTRHEAD > b.length) {
      throw new Error("pointer image " + (i + 1) + " of " + pointerCount +
                      " runs off the end of the file");
    }
    const size = v.getUint16(at);
    const p = {
      width: v.getUint16(at + 2),
      height: v.getUint16(at + 4),
      depth: b[at + 6],
      xScale: b[at + 7],
      yScale: b[at + 8],
      hotX: v.getInt16(at + 10),
      hotY: v.getInt16(at + 12),
      rgb: new Uint8Array(0),
      bits: new Uint8Array(0),
    };

    if (p.depth < 1 || p.depth > 8) {
      throw new Error("pointer image " + (i + 1) + " is " + p.depth +
                      " planes deep; this reads 1 to 8");
    }
    const cols = 3 * ((1 << p.depth) - 1);
    const need = PFS_PTRHEAD + cols + pointerBits(p);
    if (size !== need || at + need > b.length) {
      throw new Error("pointer image " + (i + 1) + " says " + size +
                      " bytes, its shape needs " + need);
    }

    pointers.push({
      ...p,
      rgb: b.subarray(at + PFS_PTRHEAD, at + PFS_PTRHEAD + cols),
      bits: b.subarray(at + PFS_PTRHEAD + cols, at + need),
    });
    at += need;
  }

  for (const q of pointerAt) {
    if (q.image > pointers.length) {
      throw new Error("a frame names pointer image " + q.image +
                      " and there are " + pointers.length);
    }
  }

  return {
    screen,
    flags,
    frameCount,
    rgb,
    /* Empty on a truecolour capture, and the decoder never looks at it: the
       pixels carry their own colour. */
    palette: palette32(rgb, palColours(screen)),
    frames: b.subarray(PFS_HEADER + palBytes, pixels),
    stride,
    times,
    pointerAt,
    pointers,
  };
}
