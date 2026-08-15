/*
 * Decode what the C encoder actually emitted.
 *
 *   node tools/web/console-xcheck.mjs CAPTURE.pfs [more.pfs ...] [--tile 16x8]
 *
 * console-selftest.mjs round-trips the wire format through console-host.mjs,
 * which is a second implementation of it written from the same header -- so a
 * misreading of that header passes both sides of it.  This closes that: it
 * compiles the encoder that ships, encodes a real capture with it, and hands
 * the bytes to the browser's decoder.  A frame that comes back different is a
 * disagreement between two independent implementations and one of them is
 * wrong.
 *
 * The encoder lives on branch proto/rfb-encoder until it merges.  RFB_TREE
 * points at a checkout of it; with the branch merged the default is this tree
 * and the variable is not needed.  Skipped, not failed, when the sources are
 * not there.
 *
 * Output is key=value and the exit code.
 *
 * SPDX-License-Identifier: MIT
 */

import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import * as esbuild from "esbuild";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const TREE = process.env.RFB_TREE || ROOT;

const args = process.argv.slice(2);
const tile = (args[args.indexOf("--tile") + 1] || "16x8").split("x").map(Number);
const captures = args.filter((a) => a.endsWith(".pfs"));

const header = join(TREE, "include", "aminetxduo", "rfb_encode.h");
const source = join(TREE, "src", "rfb", "rfb_encode.c");

if (!existsSync(header) || !existsSync(source)) {
  console.log("skip=the encoder is not in " + TREE +
              " (set RFB_TREE to a proto/rfb-encoder checkout)");
  process.exit(0);
}
if (captures.length === 0) {
  console.error("usage: console-xcheck.mjs CAPTURE.pfs [...] [--tile 16x8]");
  process.exit(2);
}

/*
 * The driver.  It exists because rfbbench decodes its own output internally
 * and never writes the wire bytes anywhere; this is the smallest program that
 * puts them in a file, and it is here rather than in the encoder's tree
 * because it is this side's verification and not that side's.
 */
const DRIVER = `
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aminetxduo/rfb_encode.h"

static unsigned rd16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}

int main(int argc, char **argv)
{
    FILE *f, *o; unsigned char *buf; long n;
    unsigned w,h,depth,bpr,frames,pal,stride,i;
    rfb_geom g; rfb_encoder e; rfb_scroll_cfg cfg;
    unsigned char *shadow,*scratch,*out;
    unsigned tw = (unsigned)atoi(argv[3]), th = (unsigned)atoi(argv[4]);
    unsigned long flags = RFB_F_BASELINE|RFB_F_COPYRECT|RFB_F_SCROLL_ADAPTIVE;

    if (argc < 5) return 2;
    f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f,0,SEEK_END); n = ftell(f); fseek(f,0,SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf,1,(size_t)n,f) != (size_t)n) return 2;
    fclose(f);
    if (memcmp(buf,"PFS2",4)) return 2;

    w=rd16(buf+4); h=rd16(buf+6); depth=buf[8]; bpr=rd16(buf+10);
    frames=rd16(buf+12); pal=3u*(1u<<depth); stride=bpr*h*depth;

    g.width=(rfb_u16)w; g.height=(rfb_u16)h; g.bytes_per_row=(rfb_u16)bpr;
    g.depth=(rfb_u8)depth; g.tile_w=(rfb_u8)tw; g.tile_h=(rfb_u8)th;
    rfb_scroll_defaults(&cfg);
    shadow = calloc(1, rfb_shadow_size(&g));
    scratch = calloc(1, rfb_scratch_size(&g,(rfb_u32)flags,&cfg));
    out = malloc(rfb_worst_case_frame(&g));
    if (rfb_encoder_init(&e,&g,(rfb_u32)flags,&cfg,shadow,rfb_shadow_size(&g),
                         scratch,rfb_scratch_size(&g,(rfb_u32)flags,&cfg)) < 0) return 2;

    o = fopen(argv[2], "wb"); if (!o) return 2;
    for (i = 0; i < frames; i++) {
        const unsigned char *src = buf + 16 + pal + (size_t)i * stride;
        long len = rfb_encode_frame(&e, src, out, rfb_worst_case_frame(&g));
        unsigned char hdr[4];
        if (len < 0) return 3;
        hdr[0]=(unsigned char)(len>>24); hdr[1]=(unsigned char)(len>>16);
        hdr[2]=(unsigned char)(len>>8);  hdr[3]=(unsigned char)len;
        fwrite(hdr,1,4,o); fwrite(out,1,(size_t)len,o);
    }
    fclose(o);
    printf("c_frames=%u c_bytes=%u c_copies=%u c_tiles=%u\\n",
           (unsigned)e.st.frames, (unsigned)e.st.bytes_out,
           (unsigned)e.st.copies, (unsigned)e.st.tiles_dirty);
    return 0;
}
`;

const work = mkdtempSync(join(tmpdir(), "console-xcheck-"));
let bad = 0;

try {
  const core = join(work, "core.mjs");
  await esbuild.build({
    stdin: {
      contents: 'export * from "./planar";\nexport * from "./pfs";\n' +
                'export * from "./tiles";\n',
      resolveDir: join(ROOT, "src", "tools", "web", "client", "console"),
      loader: "ts",
    },
    bundle: true,
    format: "esm",
    target: "es2020",
    outfile: core,
  });
  const M = await import(pathToFileURL(core).href);

  const csrc = join(work, "driver.c");
  const cbin = join(work, "driver");
  writeFileSync(csrc, DRIVER);
  execFileSync("cc", ["-O2", "-I", join(TREE, "include"), "-o", cbin, csrc, source],
               { stdio: ["ignore", "inherit", "inherit"] });

  for (const pfs of captures) {
    const wire = join(work, "wire.bin");
    const said = execFileSync(cbin, [pfs, wire, String(tile[0]), String(tile[1])],
                              { encoding: "utf8" }).trim();

    const file = readFileSync(pfs);
    const cap = M.parsePfs(file.buffer.slice(file.byteOffset,
                                             file.byteOffset + file.byteLength));
    const g = M.makeGeometry(cap.screen, tile[0], tile[1]);
    const fb = new Uint8Array(cap.stride);
    const scratch = new Uint8Array(M.scratchBytes(g));
    const bin = readFileSync(wire);

    let at = 0, i = 0, wrong = 0, copies = 0, tiles = 0;
    while (at + 4 <= bin.length) {
      const len = bin.readUInt32BE(at);
      at += 4;
      const d = M.applyUpdate(g, bin.subarray(at, at + len), fb, scratch);
      at += len;
      copies += d.copies;
      tiles += d.tiles;
      const want = cap.frames.subarray(i * cap.stride, (i + 1) * cap.stride);
      if (Buffer.compare(Buffer.from(fb), Buffer.from(want)) !== 0) {
        if (wrong === 0) console.log("first_mismatch=" + i);
        wrong++;
      }
      i++;
    }

    console.log("capture=" + pfs);
    console.log("  " + said);
    console.log("  ts_frames=" + i + " ts_copies=" + copies +
                " ts_tiles=" + tiles + " mismatches=" + wrong);
    if (wrong > 0 || i !== cap.frameCount) bad++;
  }
} catch (e) {
  console.log("fail=" + (e instanceof Error ? e.message : String(e)));
  bad++;
} finally {
  rmSync(work, { recursive: true, force: true });
}

console.log(bad === 0 ? "xcheck=ok" : "xcheck=" + bad + " captures disagreed");
process.exit(bad === 0 ? 0 : 1);
