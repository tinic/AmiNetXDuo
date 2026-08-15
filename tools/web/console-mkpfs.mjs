/*
 * Write a .pfs capture that nothing captured.
 *
 *   node tools/web/console-mkpfs.mjs OUT.pfs [W H DEPTH FRAMES [BYTESPERROW]]
 *
 * The capture side is being written on another branch and the viewer could
 * not wait for it.  What this draws is chosen to be checkable rather than
 * pretty -- see console-host.mjs -- and the fifth argument is there because
 * bytesPerRow is the field a decoder is most likely to assume is width/8:
 * pass one that is not and the picture shears if anything downstream guessed.
 *
 * SPDX-License-Identifier: MIT
 */

import { writeFileSync } from "node:fs";

import { synth, writePfs } from "./console-host.mjs";

const [out, w, h, depth, frames, bpr] = process.argv.slice(2);

if (!out) {
  console.error("usage: console-mkpfs.mjs OUT.pfs [W H DEPTH FRAMES [BYTESPERROW]]");
  process.exit(2);
}

const cap = synth(
  Number(w ?? 640), Number(h ?? 256), Number(depth ?? 3), Number(frames ?? 60),
  bpr === undefined ? undefined : Number(bpr),
);

const buf = writePfs(cap);
writeFileSync(out, buf);

console.log("wrote %s: %dx%dx%d, bpr %d, %d frames, %d bytes",
            out, cap.screen.width, cap.screen.height, cap.screen.depth,
            cap.screen.bytesPerRow, cap.frameCount, buf.length);
