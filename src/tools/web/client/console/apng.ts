/*
 * A capture as an animated PNG, so something other than this page can open it.
 *
 * WHY A FILE FORMAT AT ALL, WHEN .pfs EXISTS
 *
 *   .pfs is the capture side's own layout and is the right thing to record:
 *   it is what the Amiga wrote, frame for frame, with the palette and the
 *   pointer beside it, and nothing is thrown away.  What it is not is a file
 *   anybody else can open.  Somebody who records a session has evidence, and
 *   evidence that only its own player reads is evidence that cannot be sent
 *   to anyone.  This is the way out of that, and .pfs stays exactly as it is.
 *
 * WHY APNG AND NOT MP4
 *
 *   The source is a palette-mapped or chunky Amiga screen: flat runs of
 *   exact colours, one-pixel bevels, dithered patterns.  Every codec an MP4
 *   normally carries is lossy and subsamples the chroma, so a 1-pixel red
 *   line on grey comes back a smear -- which destroys the one property a
 *   recording of a screen is kept for.  H.264 has a lossless mode; no browser
 *   will encode it, WebCodecs or otherwise, and reaching it means vendoring
 *   something on the order of a megabyte of WASM into a page that has to be
 *   one self-contained file and is served off a 68020.
 *
 *   PNG's compressor is already in the browser -- CompressionStream("deflate")
 *   IS a zlib stream, which is exactly what an IDAT holds -- so an APNG needs
 *   no vendored encoder at all: this file is the whole of it.  It is lossless
 *   by construction, it carries per-frame timing to the millisecond, which is
 *   what a capture with no frame rate needs, and ffmpeg, GIMP, Firefox,
 *   Chrome, Safari and macOS Preview all open one.
 *
 *   THE TRADE IS NAMED AND IS NOT SILENT: this is not an MP4, and a video
 *   editor that takes only MP4 will not take it.  `ffmpeg -i out.png out.mp4`
 *   is one command away for anyone who needs that, and it is the right place
 *   for the lossy step to happen -- on their machine, with their settings,
 *   rather than baked into the only copy of the recording.
 *
 * WHAT MAKES IT SMALL
 *
 *   A screen recording is mostly a screen that is not changing.  Each frame
 *   after the first is written as the BOUNDING BOX of what changed since the
 *   one before, which is an APNG frame with an offset, and a frame that
 *   changed nothing at all is not written: its time is added to the frame in
 *   front of it instead.  An idle Workbench therefore costs a few hundred
 *   bytes a second rather than a full picture.
 *
 * SPDX-License-Identifier: MIT
 */

/* Bytes a pixel in the file.  Truecolour, no alpha: the composed frame is
   opaque by construction and an alpha channel would be a fourth plane of
   0xff.  Type 3 with a palette would be smaller raw, and after deflate it is
   very nearly the same file -- and it cannot hold HAM or a 16-bit screen,
   which would mean two encoders and one of them exercised rarely. */
const BPP = 3;

const SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

/* delay_num and delay_den are both 16-bit, so a frame held longer than 65.5
   seconds has to be timed in hundredths instead of thousandths. */
const MS_DEN = 1000;
const CS_DEN = 100;
const DELAY_MAX = 0xffff;

/* Where the fields this rewrites sit inside a finished fcTL chunk: four bytes
   of length and four of type before the 26 of data, and the CRC after it. */
const FCTL_DATA = 8;
const FCTL_DELAY = FCTL_DATA + 20;
const FCTL_CRC = FCTL_DATA + 26;

const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) !== 0 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c;
  }
  return t;
})();

function crc32(b: Uint8Array, from: number, to: number): number {
  let c = ~0;
  for (let i = from; i < to; i++) c = CRC_TABLE[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (~c) >>> 0;
}

function be32(b: Uint8Array, at: number, v: number): void {
  b[at] = (v >>> 24) & 0xff;
  b[at + 1] = (v >>> 16) & 0xff;
  b[at + 2] = (v >>> 8) & 0xff;
  b[at + 3] = v & 0xff;
}

function be16(b: Uint8Array, at: number, v: number): void {
  b[at] = (v >>> 8) & 0xff;
  b[at + 1] = v & 0xff;
}

/* Length, type, data, CRC over the type and the data. */
function chunk(type: string, data: Uint8Array): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(12 + data.length);
  be32(out, 0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  be32(out, 8 + data.length, crc32(out, 4, 8 + data.length));
  return out;
}

/*
 * zlib, from the browser, with no library in the page.
 *
 * CompressionStream("deflate") is the zlib WRAPPER -- header and Adler-32 --
 * and not the raw form, which is what a PNG data stream is and the reason
 * this needs nothing vendored.  "deflate-raw" would be the wrong one.
 */
async function deflate(raw: Uint8Array<ArrayBuffer>):
    Promise<Uint8Array<ArrayBuffer>> {
  const cs = new CompressionStream("deflate");
  const w = cs.writable.getWriter();
  /* Started and not awaited, because the reader below is what drains it:
     awaiting the write first deadlocks on a stream nobody is reading. */
  const written = (async () => {
    await w.write(raw);
    await w.close();
  })();
  const out = new Uint8Array(await new Response(cs.readable).arrayBuffer());
  await written;
  return out;
}

function paeth(a: number, b: number, c: number): number {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

interface Rect { x0: number; y0: number; x1: number; y1: number }

/*
 * What changed, as one rectangle, or null when nothing did.
 *
 * One box and not a list of them: an APNG frame IS a rectangle, so a second
 * region would have to be a second frame at the same instant, and a screen
 * that changed in two corners is rare next to one where a single window moved.
 */
function damage(a: Uint32Array, b: Uint32Array,
                       w: number, h: number): Rect | null {
  let y0 = -1;
  let y1 = -1;
  for (let y = 0; y < h; y++) {
    const row = y * w;
    for (let x = 0; x < w; x++) {
      if (a[row + x] !== b[row + x]) {
        if (y0 < 0) y0 = y;
        y1 = y;
        break;
      }
    }
  }
  if (y0 < 0) return null;

  let x0 = w;
  let x1 = -1;
  for (let y = y0; y <= y1; y++) {
    const row = y * w;
    for (let x = 0; x < x0; x++) {
      if (a[row + x] !== b[row + x]) { x0 = x; break; }
    }
    for (let x = w - 1; x > x1; x--) {
      if (a[row + x] !== b[row + x]) { x1 = x; break; }
    }
  }
  return { x0, y0, x1: x1 + 1, y1: y1 + 1 };
}

/*
 * A rectangle of RGBA into PNG scanlines: one filter byte and then RGB.
 *
 * The filter is chosen per row by the minimum sum of absolute differences,
 * which is what libpng's own heuristic does.  It is worth having on this
 * material rather than filtering 0 everywhere: a Workbench bevel is a
 * vertical edge that Sub codes to nothing, and a flat window is a horizontal
 * one that Up does.
 */
function scanlines(px: Uint8Array, imgW: number,
                   r: Rect): Uint8Array<ArrayBuffer> {
  const rw = r.x1 - r.x0;
  const rh = r.y1 - r.y0;
  const stride = rw * BPP;
  const out = new Uint8Array(rh * (stride + 1));

  let prev = new Uint8Array(stride);
  let cur = new Uint8Array(stride);
  const f = [new Uint8Array(stride), new Uint8Array(stride),
             new Uint8Array(stride), new Uint8Array(stride)];
  const sum = [0, 0, 0, 0, 0];

  for (let y = 0; y < rh; y++) {
    let s = ((r.y0 + y) * imgW + r.x0) * 4;
    for (let i = 0; i < stride; i += BPP) {
      cur[i] = px[s];
      cur[i + 1] = px[s + 1];
      cur[i + 2] = px[s + 2];
      s += 4;
    }

    sum[0] = sum[1] = sum[2] = sum[3] = sum[4] = 0;
    for (let i = 0; i < stride; i++) {
      const a = i >= BPP ? cur[i - BPP] : 0;
      const b = prev[i];
      const c = i >= BPP ? prev[i - BPP] : 0;
      const x = cur[i];

      const v1 = (x - a) & 0xff;
      const v2 = (x - b) & 0xff;
      const v3 = (x - ((a + b) >> 1)) & 0xff;
      const v4 = (x - paeth(a, b, c)) & 0xff;
      f[0][i] = v1; f[1][i] = v2; f[2][i] = v3; f[3][i] = v4;

      /* The bytes read as signed, which is what the heuristic measures. */
      sum[0] += x < 128 ? x : 256 - x;
      sum[1] += v1 < 128 ? v1 : 256 - v1;
      sum[2] += v2 < 128 ? v2 : 256 - v2;
      sum[3] += v3 < 128 ? v3 : 256 - v3;
      sum[4] += v4 < 128 ? v4 : 256 - v4;
    }

    let best = 0;
    for (let k = 1; k < 5; k++) if (sum[k] < sum[best]) best = k;

    const at = y * (stride + 1);
    out[at] = best;
    out.set(best === 0 ? cur : f[best - 1], at + 1);

    const swap = prev;
    prev = cur;
    cur = swap;
  }

  return out;
}

/* Milliseconds as the two fields an fcTL carries. */
function delayFields(ms: number): { num: number; den: number } {
  const m = Math.max(0, Math.round(ms));
  if (m <= DELAY_MAX) return { num: m, den: MS_DEN };
  return { num: Math.min(DELAY_MAX, Math.round(m / 10)), den: CS_DEN };
}

/*
 * The writer.  Frames go in one at a time and only the previous one is held,
 * because a 640x512 recording of any length would not fit in a page that kept
 * them all: what accumulates is the COMPRESSED file and nothing else.
 *
 * The RGBA a caller hands to add() is kept by reference until the next add(),
 * as the frame the next one is compared against.  Hand it a fresh buffer each
 * time -- View.compose() allocates one -- and never write into one it has.
 */
export class Apng {
  private readonly parts: Uint8Array<ArrayBuffer>[] = [];
  private readonly w: number;
  private readonly h: number;

  private prev: Uint32Array | null = null;
  private seq = 0;
  private count = 0;

  /* The fcTL of the frame last written, and the time it has been given so
     far.  Both exist so that a frame identical to it can be dropped and its
     time added here instead of costing a picture. */
  private lastFctl: Uint8Array<ArrayBuffer> | null = null;
  private lastMs = 0;

  constructor(w: number, h: number) {
    if (!Number.isInteger(w) || !Number.isInteger(h) || w <= 0 || h <= 0) {
      throw new Error("an APNG cannot be " + w + "x" + h);
    }
    this.w = w;
    this.h = h;
  }

  /* Frames actually written, which is not how many were offered: the ones
     that changed nothing were folded into their predecessor. */
  get frames(): number { return this.count; }

  async add(rgba: Uint8Array<ArrayBuffer> | Uint8ClampedArray<ArrayBuffer>,
            delayMs: number): Promise<void> {
    const want = this.w * this.h * 4;
    if (rgba.length !== want) {
      throw new Error("a frame is " + rgba.length + " bytes, " + this.w + "x" +
                      this.h + " needs " + want);
    }

    const px = rgba instanceof Uint8Array
      ? rgba : new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.length);
    if ((px.byteOffset & 3) !== 0) {
      throw new Error("a frame buffer has to be longword aligned");
    }
    const words = new Uint32Array(px.buffer, px.byteOffset, this.w * this.h);

    let r: Rect = { x0: 0, y0: 0, x1: this.w, y1: this.h };
    if (this.prev !== null) {
      const d = damage(this.prev, words, this.w, this.h);
      if (d === null) { this.hold(delayMs); return; }
      r = d;
    }

    const z = await deflate(scanlines(px, this.w, r));

    const d = delayFields(delayMs);
    const head = new Uint8Array(26);
    be32(head, 0, this.seq++);
    be32(head, 4, r.x1 - r.x0);
    be32(head, 8, r.y1 - r.y0);
    be32(head, 12, r.x0);
    be32(head, 16, r.y0);
    be16(head, 20, d.num);
    be16(head, 22, d.den);
    head[24] = 0;         /* dispose: leave this frame where it is       */
    head[25] = 0;         /* blend: the new pixels replace what is under */

    const fctl = chunk("fcTL", head);
    this.parts.push(fctl);
    this.lastFctl = fctl;
    this.lastMs = delayMs;

    if (this.count === 0) {
      /* The first frame is the still picture as well, so it is an IDAT and
         carries no sequence number of its own. */
      this.parts.push(chunk("IDAT", z));
    } else {
      const body = new Uint8Array(4 + z.length);
      be32(body, 0, this.seq++);
      body.set(z, 4);
      this.parts.push(chunk("fdAT", body));
    }

    this.count++;
    this.prev = words;
  }

  /* A frame that drew nothing new: the one already written stays up longer. */
  private hold(ms: number): void {
    const c = this.lastFctl;
    if (c === null) return;
    this.lastMs += ms;
    const d = delayFields(this.lastMs);
    be16(c, FCTL_DELAY, d.num);
    be16(c, FCTL_DELAY + 2, d.den);
    be32(c, FCTL_CRC, crc32(c, 4, FCTL_CRC));
  }

  finish(): Uint8Array<ArrayBuffer> {
    if (this.count === 0) throw new Error("no frames to write");

    const ihdr = new Uint8Array(13);
    be32(ihdr, 0, this.w);
    be32(ihdr, 4, this.h);
    ihdr[8] = 8;          /* bits a sample */
    ihdr[9] = 2;          /* truecolour    */

    const actl = new Uint8Array(8);
    be32(actl, 0, this.count);
    be32(actl, 4, 0);     /* plays: round for ever */

    const head = [
      Uint8Array.from(SIGNATURE),
      chunk("IHDR", ihdr),
      chunk("acTL", actl),
    ];
    const tail = [chunk("IEND", new Uint8Array(0))];

    let n = 0;
    for (const p of head) n += p.length;
    for (const p of this.parts) n += p.length;
    for (const p of tail) n += p.length;

    const out = new Uint8Array(n);
    let at = 0;
    for (const p of head) { out.set(p, at); at += p.length; }
    for (const p of this.parts) { out.set(p, at); at += p.length; }
    for (const p of tail) { out.set(p, at); at += p.length; }
    return out;
  }
}
