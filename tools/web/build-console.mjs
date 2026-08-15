/*
 * Build src/tools/web/console.html: the framebuffer viewer, TypeScript in,
 * one HTML file out.
 *
 *   node tools/web/build-console.mjs            write the file
 *   node tools/web/build-console.mjs --check    fail if what is committed differs
 *
 * The same shape as build.mjs, and still a SEPARATE script: that one inlines
 * a vendored terminal, two subset webfonts and their licence notice into an
 * iso-8859-1 page, and none of those exist here.  What was once also true --
 * that this output was a prototype's, uncommitted, and folding the two
 * together would mean either gating the prototype or loosening the gate on
 * the page that ships -- is not: both pages are committed artifacts now and
 * tools/ci.sh's web stage checks both.
 *
 * COMMITTED, for build.mjs's reason.  The m68k build must work on a machine
 * with no node on it, so the artifact is committed and CMake copies it; this
 * script is how it gets regenerated when a source under
 * src/tools/web/client/console/ changes, and --check is how tools/ci.sh
 * notices that somebody edited a source and forgot to run it.
 *
 * AND console.html.gz BESIDE IT
 *
 *   httpd serves the sibling to a browser that offered `Accept-Encoding:
 *   gzip` and the plain file to one that did not -- it picks it up by
 *   spelling, and there is no compressor on the Amiga.  The page is a
 *   twentieth of the Shell's, so this saves tenths of a second rather than
 *   seconds; it is here because a page shipped without one is served
 *   uncompressed and says nothing about it, which is the sort of thing found
 *   years later.
 *
 * The guards at the bottom are build.mjs's, minus the two that are about
 * things this page does not have.  What they are for is unchanged: an Amiga
 * serving this may have no route off the LAN, and a CDN script, a webfont or
 * a source map are all the same bug, a request nothing answers.
 *
 * SPDX-License-Identifier: MIT
 */

import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync, gzipSync } from "node:zlib";

import * as esbuild from "esbuild";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const WEB = join(ROOT, "src", "tools", "web");
const SRC = join(WEB, "client", "console");
const OUT = join(WEB, "console.html");
const OUT_GZ = OUT + ".gz";

const check = process.argv.includes("--check");

const bundle = await esbuild.build({
  entryPoints: [join(SRC, "main.ts")],
  bundle: true,
  format: "esm",
  target: "es2020",
  minify: true,
  sourcemap: false,
  legalComments: "none",
  charset: "ascii",
  write: false,
});

if (bundle.outputFiles.length !== 1) {
  throw new Error("expected one output file, got " + bundle.outputFiles.length);
}
const script = bundle.outputFiles[0].text.trim();

const css = await esbuild.build({
  stdin: {
    contents: readFileSync(join(SRC, "page.css"), "utf8"),
    loader: "css",
    resolveDir: SRC,
  },
  minify: true,
  sourcemap: false,
  legalComments: "none",
  charset: "ascii",
  write: false,
});
const style = css.outputFiles[0].text.trim();

const template = readFileSync(join(SRC, "index.html"), "utf8");

for (const slot of ["/*STYLE*/", "/*SCRIPT*/"]) {
  if (!template.includes(slot)) throw new Error("template has no " + slot);
}

/* Functions and not strings: a `$&` inside minified CSS is a substitution
   pattern to String.replace and would duplicate a chunk of the stylesheet. */
const html = template
  .replace("/*STYLE*/", () => style)
  .replace("/*SCRIPT*/", () => script)
  .replace(
    "  TEMPLATE.  The file that ships is src/tools/web/console.html, which is this\n" +
      "  with the stylesheet and the script inlined into it by\n" +
      "  tools/web/build-console.mjs.  Editing the built file is editing something\n" +
      "  that will be overwritten.",
    "  GENERATED.  Edit src/tools/web/client/console/ and run\n" +
      "  tools/web/build-console.mjs; an edit made here is an edit the next build\n" +
      "  throws away, and tools/ci.sh's web stage fails when the two disagree."
  );

const problems = [];

if (/sourceMappingURL/.test(html)) {
  problems.push("a sourceMappingURL survived: the browser would fetch a map");
}
for (const m of html.matchAll(/\b(?:src|href)\s*=\s*["']([^"']+)["']/g)) {
  if (!m[1].startsWith("data:")) {
    problems.push("an external reference: " + m[0].slice(0, 60));
  }
}
for (const m of html.matchAll(/url\(\s*["']?(?!data:)(?:https?:)?\/\//g)) {
  problems.push("a stylesheet fetch: " + m[0]);
}
if (/@import/.test(html)) problems.push("an @import");

if (problems.length > 0) {
  for (const p of problems) console.error("console: " + p);
  process.exit(1);
}

/* --------------------------------------------------------------- the file */

const sha = (s) => createHash("sha256").update(s).digest("hex").slice(0, 12);

/*
 * Byte 9 of the gzip container is the compressing machine, and zlib fills it
 * in from whatever it was compiled for -- 0x13 from the node on a Mac, 0x03
 * from the one on Linux.  Stamped Unix so that what is committed depends on
 * the page and not on who built it; the modification time in bytes 4..7 is
 * already zero.  build.mjs does the same to shell.html.gz.
 */
const pack = (s) => {
  const gz = gzipSync(Buffer.from(s, "utf8"), { level: 9 });
  gz[9] = 0x03;
  return gz;
};

let existing = null;
try {
  existing = readFileSync(OUT, "utf8");
} catch {
  /* first build */
}

/*
 * The .gz is checked by unpacking it, not by rebuilding it: a newer zlib that
 * packs the same bytes two per cent smaller is not a drift anybody needs to
 * be told about, and treating it as one would fail CI on a node upgrade.
 * What can actually go wrong is a compressed copy of a page that is no longer
 * the page, and that is what this catches.
 */
let packed = null;
try {
  const raw = readFileSync(OUT_GZ);
  packed = gunzipSync(raw).toString("utf8") === html ? raw : null;
} catch {
  /* absent, or not gzip at all */
}

if (check) {
  if (existing === html && packed !== null) {
    console.log("web: console.html matches its sources (%d bytes, sha %s)",
                Buffer.byteLength(html), sha(html));
    console.log("web: console.html.gz unpacks to it (%d bytes, %d%%)",
                packed.length,
                Math.round((packed.length * 100) / Buffer.byteLength(html)));
    process.exit(0);
  }
  if (existing !== html) {
    console.error("web: console.html does NOT match its sources.");
    console.error("web: committed %s, sources build to %s",
                  existing === null ? "nothing" : sha(existing), sha(html));
  }
  if (packed === null) {
    console.error("web: console.html.gz is missing, or does not unpack to "
                  + "the page beside it.");
  }
  console.error("web: run  node tools/web/build-console.mjs  and commit the "
                + "result.");
  process.exit(1);
}

if (existing === html) {
  console.log("web: console.html unchanged (%d bytes)", Buffer.byteLength(html));
} else {
  writeFileSync(OUT, html);
  console.log("web: console.html written, %d bytes, sha %s",
              Buffer.byteLength(html), sha(html));
}

if (packed !== null) {
  console.log("web: console.html.gz unchanged (%d bytes)", packed.length);
} else {
  const gz = pack(html);
  writeFileSync(OUT_GZ, gz);
  console.log("web: console.html.gz written, %d bytes, %d%% of the page",
              gz.length, Math.round((gz.length * 100) / Buffer.byteLength(html)));
}
