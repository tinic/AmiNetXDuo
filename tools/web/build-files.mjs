/* Build httpd's dependency-free WebDAV file manager into one HTML file.
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
const SRC = join(WEB, "client", "files");
const OUT = join(WEB, "files.html");
const OUT_GZ = OUT + ".gz";
const check = process.argv.includes("--check");

const js = await esbuild.build({
  entryPoints: [join(SRC, "main.ts")],
  bundle: true,
  format: "esm",
  target: "es2020",
  minify: true,
  sourcemap: false,
  legalComments: "none",
  charset: "ascii",
  nodePaths: [join(HERE, "node_modules")],
  write: false,
});
if (js.outputFiles.length !== 1) throw new Error("expected one script output");

const pageCss = readFileSync(join(SRC, "page.css"), "utf8");
const css = await esbuild.build({
  stdin: {
    contents: pageCss,
    loader: "css",
    resolveDir: SRC,
  },
  minify: true,
  sourcemap: false,
  legalComments: "none",
  charset: "ascii",
  write: false,
});

const template = readFileSync(join(SRC, "index.html"), "utf8");
const script = js.outputFiles[0].text.trim();
const html = template
  .replace("/*STYLE*/", () => css.outputFiles[0].text.trim())
  .replace("/*SCRIPT*/", () => script)
  .replace(
    "  TEMPLATE. The shipping src/tools/web/files.html has this page's CSS and\n" +
      "  TypeScript inlined by tools/web/build-files.mjs. Edit this directory, not\n" +
      "  the generated page.",
    "  GENERATED. Edit src/tools/web/client/files/ and run\n" +
      "  tools/web/build-files.mjs; tools/ci.sh rejects a stale copy.",
  );

const problems = [];
if (/sourceMappingURL/.test(html)) problems.push("a source map survived");
if (
  !/#editor-host\s+\.cm-cursor[\s\S]*?border-left-color:\s*var\(--editor-caret\)/.test(
    pageCss,
  )
)
  problems.push("the CodeMirror cursor has no explicit visible colour");
/* Filenames, server error text and DAV properties are hostile input.  The
   client builds its rows with textContent/createTextNode; keep future edits
   from quietly reopening the innerHTML injection found in the third-party
   client this page replaced. */
for (const unsafe of [
  /\.innerHTML\b/,
  /\.outerHTML\b/,
  /insertAdjacentHTML\b/,
  /\beval\s*\(/,
  /new Function\b/,
]) {
  if (unsafe.test(script)) problems.push("an unsafe DOM/code sink: " + unsafe);
}
for (const m of html.matchAll(/\b(?:src|href)\s*=\s*["']([^"']+)["']/g)) {
  if (!m[1].startsWith("data:"))
    problems.push("an external reference: " + m[0]);
}
if (/@import|url\(\s*["']?(?!data:)(?:https?:)?\/\//.test(html))
  problems.push("an external stylesheet reference");
if (problems.length) throw new Error(problems.join("; "));

const sha = (s) => createHash("sha256").update(s).digest("hex").slice(0, 12);
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
let packed = null;
try {
  const raw = readFileSync(OUT_GZ);
  packed = gunzipSync(raw).toString("utf8") === html ? raw : null;
} catch {
  /* first build */
}

if (check) {
  if (existing === html && packed !== null) {
    console.log(
      "web: files.html matches its sources (%d bytes, %d bytes gzip, sha %s)",
      Buffer.byteLength(html),
      packed.length,
      sha(html),
    );
    process.exit(0);
  }
  if (existing !== html)
    console.error("web: files.html does NOT match its sources");
  if (packed === null) console.error("web: files.html.gz is missing or stale");
  console.error(
    "web: run  node tools/web/build-files.mjs  and commit the result",
  );
  process.exit(1);
}

if (existing !== html) writeFileSync(OUT, html);
if (packed === null) writeFileSync(OUT_GZ, pack(html));
console.log(
  "web: files.html ready (%d bytes, %d bytes gzip, sha %s)",
  Buffer.byteLength(html),
  (packed ?? pack(html)).length,
  sha(html),
);
