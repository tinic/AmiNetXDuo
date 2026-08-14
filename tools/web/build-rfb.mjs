/*
 * Build build/web/rfb.html: the framebuffer viewer, TypeScript in, one file
 * out.
 *
 *   node tools/web/build-rfb.mjs
 *
 * The same shape as build.mjs and deliberately a SEPARATE script rather than
 * a second entry point in it.  build.mjs's output is committed and checked by
 * tools/ci.sh's web stage; this is a prototype whose output is not committed
 * at all, and folding the two together would mean either gating the prototype
 * or loosening the gate on the page that ships.
 *
 * Into build/, which is gitignored, for the same reason.  shell.html is
 * committed because an Amiga with no node on it has to serve it; nothing
 * serves this one yet, so it is rebuilt by whoever wants to look at it.
 *
 * The guards at the bottom are build.mjs's, minus the two that are about
 * things this page does not have -- the vendored font and its licence notice,
 * and the Latin-1 byte range that the Shell's output needs and a canvas does
 * not.  What they are for is unchanged: an Amiga serving this may have no
 * route off the LAN, and a CDN script, a webfont or a source map are all the
 * same bug, a request nothing answers.
 *
 * SPDX-License-Identifier: MIT
 */

import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import * as esbuild from "esbuild";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const SRC = join(ROOT, "src", "tools", "web", "client", "rfb");
const OUTDIR = join(ROOT, "build", "web");
const OUT = join(OUTDIR, "rfb.html");

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
    "  TEMPLATE.  The page that runs is build/web/rfb.html, which is this with the\n" +
      "  stylesheet and the script inlined by tools/web/build-rfb.mjs.",
    "  GENERATED.  Edit src/tools/web/client/rfb/ and run tools/web/build-rfb.mjs."
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
  for (const p of problems) console.error("rfb: " + p);
  process.exit(1);
}

mkdirSync(OUTDIR, { recursive: true });
writeFileSync(OUT, html);

console.log("rfb: build/web/rfb.html written, %d bytes, sha %s",
            Buffer.byteLength(html),
            createHash("sha256").update(html).digest("hex").slice(0, 12));
