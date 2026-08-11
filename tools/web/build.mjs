/*
 * Build src/tools/web/terminal.html: TypeScript in, one HTML file out.
 *
 *   node tools/web/build.mjs            write the file
 *   node tools/web/build.mjs --check    fail if what is committed differs
 *
 * NOT PART OF `cmake --build`, ON PURPOSE
 *
 *   The m68k build must work on a machine with no node on it, so the artifact
 *   is COMMITTED and CMake copies it.  This script is how it gets regenerated
 *   when a source under src/tools/web/client/ changes, and --check is how
 *   tools/ci.sh notices that somebody edited a source and forgot to run it.
 *
 * WHY esbuild AND NOT vite
 *
 *   The whole job is "TypeScript in, one minified script out" with no dev
 *   server, no plugins and no HMR; esbuild is that in one call, and vite is a
 *   framework for the parts of the problem this does not have.
 *
 * WHY ONE FILE
 *
 *   An Amiga serves this over a LAN and may have no route off it.  A CDN
 *   script, a webfont, a separate stylesheet or a source map fetched at
 *   runtime are all the same bug: a request the browser makes and nothing
 *   answers.  Everything is inlined and the checks at the bottom refuse to
 *   write a file that would make one.
 *
 * SPDX-License-Identifier: MIT
 */

import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import * as esbuild from "esbuild";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const WEB = join(ROOT, "src", "tools", "web");
const VENDOR = join(WEB, "vendor", "xterm");
const OUT = join(WEB, "terminal.html");

const check = process.argv.includes("--check");

/* ------------------------------------------------------------- the script */

const bundle = await esbuild.build({
  entryPoints: [join(WEB, "client", "main.ts")],
  bundle: true,
  format: "esm",
  target: "es2020",
  minify: true,
  /* No map, and no comment pointing at one either: the page is served by a
     machine that may have nothing behind it. */
  sourcemap: false,
  legalComments: "none",
  charset: "ascii",
  write: false,
  /* The vendored typings declare these two module names ambiently, which is
     how TypeScript resolves the imports; here the same names resolve to the
     prebuilt ESM those typings describe.  Nothing comes from node_modules. */
  alias: {
    "@xterm/xterm": join(VENDOR, "xterm.mjs"),
    "@xterm/addon-fit": join(VENDOR, "addon-fit.mjs"),
  },
});

if (bundle.outputFiles.length !== 1) {
  throw new Error("expected one output file, got " + bundle.outputFiles.length);
}
const script = bundle.outputFiles[0].text.trim();

/* -------------------------------------------------------------- the style */

const css = await esbuild.build({
  stdin: {
    contents:
      readFileSync(join(VENDOR, "xterm.css"), "utf8") +
      "\n" +
      readFileSync(join(WEB, "client", "page.css"), "utf8"),
    loader: "css",
    resolveDir: WEB,
  },
  minify: true,
  sourcemap: false,
  legalComments: "none",
  charset: "ascii",
  write: false,
});
const style = css.outputFiles[0].text.trim();

/* --------------------------------------------------------------- assembly */

const template = readFileSync(join(WEB, "client", "index.html"), "utf8");

for (const slot of ["/*STYLE*/", "/*SCRIPT*/"]) {
  if (!template.includes(slot)) throw new Error("template has no " + slot);
}

/*
 * String replacement, not a regex: a `$&` inside minified CSS is a
 * substitution pattern to String.replace and would silently duplicate a
 * chunk of the stylesheet.
 */
const html = template
  .replace("/*STYLE*/", () => style)
  .replace("/*SCRIPT*/", () => script)
  .replace(
    "  TEMPLATE.  The file that ships is src/tools/web/terminal.html, which is this\n" +
      "  with the stylesheets and the script inlined into it by tools/web/build.mjs.\n" +
      "  Editing the built file is editing something that will be overwritten.",
    "  GENERATED.  Edit src/tools/web/client/ and run tools/web/build.mjs; an edit\n" +
      "  made here is an edit the next build throws away, and tools/ci.sh's web\n" +
      "  stage fails when the two disagree."
  );

/* ------------------------------------------------------------- the checks */

const problems = [];

if (/sourceMappingURL/.test(html)) {
  problems.push("a sourceMappingURL survived: the browser would fetch a map");
}
/* Anything that would make the browser go and ask for something.  A data:
   URI is the one form that cannot: it IS the resource.  The socket's address
   is the page's own path and is built at runtime, so it is not an attribute
   here and does not have to be excused. */
for (const m of html.matchAll(/\b(?:src|href)\s*=\s*["']([^"']+)["']/g)) {
  if (!m[1].startsWith("data:")) {
    problems.push("an external reference: " + m[0].slice(0, 60));
  }
}
for (const m of html.matchAll(/url\(\s*["']?(?!data:)(?:https?:)?\/\//g)) {
  problems.push("a stylesheet fetch: " + m[0]);
}
if (/@import/.test(html)) problems.push("an @import");
if (/@font-face/.test(html)) problems.push("an @font-face");

for (const ch of html) {
  if (ch.charCodeAt(0) > 126 && ch !== "\n") {
    problems.push("a byte above 126 in what is declared iso-8859-1");
    break;
  }
}

if (problems.length > 0) {
  for (const p of problems) console.error("build: " + p);
  process.exit(1);
}

/* --------------------------------------------------------------- the file */

const sha = (s) => createHash("sha256").update(s).digest("hex").slice(0, 12);

let existing = null;
try {
  existing = readFileSync(OUT, "utf8");
} catch {
  /* first build */
}

if (check) {
  if (existing === html) {
    console.log("web: terminal.html matches its sources (%d bytes, sha %s)",
                Buffer.byteLength(html), sha(html));
    process.exit(0);
  }
  console.error("web: terminal.html does NOT match its sources.");
  console.error("web: committed %s, sources build to %s",
                existing === null ? "nothing" : sha(existing), sha(html));
  console.error("web: run  node tools/web/build.mjs  and commit the result.");
  process.exit(1);
}

if (existing === html) {
  console.log("web: terminal.html unchanged (%d bytes)", Buffer.byteLength(html));
} else {
  writeFileSync(OUT, html);
  console.log("web: terminal.html written, %d bytes, sha %s",
              Buffer.byteLength(html), sha(html));
}
