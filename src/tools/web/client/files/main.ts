/* A small WebDAV file manager for httpd. No filename ever becomes markup.
 * SPDX-License-Identifier: MIT
 */

import {
  defaultKeymap,
  history as undoHistory,
  historyKeymap,
  indentWithTab,
} from "@codemirror/commands";
import { searchKeymap } from "@codemirror/search";
import { EditorState } from "@codemirror/state";
import {
  drawSelection,
  EditorView,
  highlightSpecialChars,
  keymap,
  lineNumbers,
} from "@codemirror/view";

type DavEntry = {
  href: string;
  name: string;
  drawer: boolean;
  size: number;
  modified: string;
};

const byId = <T extends HTMLElement>(id: string): T => {
  const node = document.getElementById(id);
  if (node === null) throw new Error("page has no #" + id);
  return node as T;
};

const rows = byId<HTMLTableSectionElement>("rows");
const empty = byId<HTMLDivElement>("empty");
const status = byId<HTMLSpanElement>("status");
const progress = byId<HTMLProgressElement>("progress");
const pick = byId<HTMLInputElement>("pick");
const crumbs = byId<HTMLElement>("crumbs");
const up = byId<HTMLButtonElement>("up");
const mkdir = byId<HTMLButtonElement>("mkdir");
const upload = byId<HTMLButtonElement>("upload");
const drop = byId<HTMLDivElement>("drop");
const askDialog = byId<HTMLDialogElement>("ask");
const askTitle = byId<HTMLHeadingElement>("ask-title");
const askText = byId<HTMLParagraphElement>("ask-text");
const askField = byId<HTMLLabelElement>("ask-field");
const askName = byId<HTMLInputElement>("ask-name");
const askOk = byId<HTMLButtonElement>("ask-ok");
const editorDialog = byId<HTMLDialogElement>("editor-dialog");
const editorHost = byId<HTMLDivElement>("editor-host");
const editorName = byId<HTMLElement>("editor-name");
const editorPath = byId<HTMLElement>("editor-path");
const editorState = byId<HTMLElement>("editor-state");
const editorMessage = byId<HTMLElement>("editor-message");
const editorClose = byId<HTMLButtonElement>("editor-close");
const editorSave = byId<HTMLButtonElement>("editor-save");

let current = "/";
let loadSerial = 0;
let volumeRoot = false;
let dragDepth = 0;
let editorView: EditorView | null = null;
let editorHref = "";
let editorEtag = "";
let editorSaved = "";
let editorSaving = false;

function say(text: string, bad = false): void {
  status.textContent = text;
  status.className = bad ? "bad" : "";
}

function ask(
  title: string,
  text: string,
  action: string,
  value?: string,
): Promise<string | null> {
  askTitle.textContent = title;
  askText.textContent = text;
  askOk.textContent = action;
  askOk.className = action === "Delete" ? "danger" : "primary";
  askField.hidden = value === undefined;
  askName.value = value ?? "";
  askDialog.returnValue = "cancel";
  askDialog.showModal();
  if (value !== undefined) {
    askName.focus();
    askName.select();
  } else {
    askOk.focus();
  }

  return new Promise((resolve) => {
    const closed = (): void => {
      askDialog.removeEventListener("close", closed);
      resolve(askDialog.returnValue === "ok" ? askName.value : null);
    };
    askDialog.addEventListener("close", closed);
  });
}

function hex(c: string): number {
  const n = parseInt(c, 16);
  return Number.isNaN(n) ? -1 : n;
}

/* The server percent-encodes Amiga filename BYTES. decodeURIComponent assumes
   UTF-8 and rejects perfectly valid names such as a lone 0xe9. */
export function displaySegment(encoded: string): string {
  let out = "";
  for (let i = 0; i < encoded.length; i++) {
    if (encoded[i] === "%" && i + 2 < encoded.length) {
      const hi = hex(encoded[i + 1]);
      const lo = hex(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += String.fromCharCode((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += encoded[i];
  }
  return out;
}

/* A new name must be representable by the one-byte Amiga filesystem API.
   Encoding all but RFC 3986's unreserved set also keeps %, # and ? literal. */
export function encodeName(name: string): string {
  if (name.length === 0 || name === "." || name === ".." || /[:/\\]/.test(name))
    throw new Error("Use a name without colon, slash or backslash.");

  let out = "";
  for (const c of name) {
    const n = c.codePointAt(0)!;
    if (n > 255)
      throw new Error("That name contains a character AmigaOS cannot store.");
    if (
      (n >= 65 && n <= 90) ||
      (n >= 97 && n <= 122) ||
      (n >= 48 && n <= 57) ||
      c === "-" ||
      c === "." ||
      c === "_" ||
      c === "~"
    )
      out += c;
    else out += "%" + n.toString(16).toUpperCase().padStart(2, "0");
  }
  return out;
}

function cleanDrawer(path: string): string {
  if (!path.startsWith("/")) path = "/";
  const q = path.indexOf("?");
  if (q >= 0) path = path.slice(0, q);
  if (!path.endsWith("/")) path += "/";
  return path;
}

function pathFromHash(): string {
  return cleanDrawer(location.hash.length > 1 ? location.hash.slice(1) : "/");
}

function parentOf(path: string): string {
  const parts = path.slice(1, -1).split("/").filter(Boolean);
  parts.pop();
  return "/" + (parts.length ? parts.join("/") + "/" : "");
}

function childOf(path: string, name: string, drawer = false): string {
  return cleanDrawer(path) + encodeName(name) + (drawer ? "/" : "");
}

function elementChildren(node: ParentNode, local: string): Element[] {
  return Array.from(node.querySelectorAll("*")).filter(
    (e) => e.localName === local,
  );
}

function firstText(node: ParentNode, local: string): string {
  return elementChildren(node, local)[0]?.textContent ?? "";
}

function leafName(href: string): string {
  const p = href.endsWith("/") ? href.slice(0, -1) : href;
  const parts = p.split("/");
  return displaySegment(parts[parts.length - 1] ?? "");
}

function parseListing(xml: string, path: string): DavEntry[] {
  const doc = new DOMParser().parseFromString(xml, "application/xml");
  if (doc.querySelector("parsererror") !== null)
    throw new Error("The directory listing was not valid XML.");

  const wanted = cleanDrawer(path);
  const entries: DavEntry[] = [];
  for (const response of elementChildren(doc, "response")) {
    const raw = firstText(response, "href");
    if (!raw) continue;
    let href: string;
    try {
      const u = new URL(raw, location.href);
      href = u.pathname;
    } catch {
      continue;
    }
    const drawer = elementChildren(response, "collection").length !== 0;
    if (cleanDrawer(href) === wanted && drawer) continue;
    entries.push({
      href,
      name: leafName(href),
      drawer,
      size: Number(firstText(response, "getcontentlength")) || 0,
      modified: firstText(response, "getlastmodified"),
    });
  }
  entries.sort(
    (a, b) =>
      Number(b.drawer) - Number(a.drawer) ||
      a.name.localeCompare(b.name, undefined, { sensitivity: "base" }),
  );
  return entries;
}

async function request(
  method: string,
  href: string,
  headers: Record<string, string> = {},
  body?: BodyInit,
): Promise<Response> {
  const response = await fetch(href, {
    method,
    headers,
    body,
    cache: "no-store",
  });
  if (!response.ok) {
    let detail = "";
    try {
      detail = (await response.text()).trim();
    } catch {
      /* no body */
    }
    throw new Error(
      method +
        " failed (" +
        response.status +
        ")" +
        (detail ? ": " + detail.replace(/\s+/g, " ") : ""),
    );
  }
  return response;
}

function formatSize(n: number): string {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + " KB";
  return (n / (1024 * 1024)).toFixed(n < 10485760 ? 1 : 0) + " MB";
}

function drawCrumbs(): void {
  crumbs.replaceChildren();
  const root = document.createElement("button");
  root.type = "button";
  root.textContent = volumeRoot ? "Volumes" : "Files";
  root.onclick = () => go("/");
  crumbs.append(root);

  let href = "/";
  for (const part of current.slice(1, -1).split("/").filter(Boolean)) {
    crumbs.append(document.createTextNode(" / "));
    href += part + "/";
    const button = document.createElement("button");
    const destination = href;
    button.type = "button";
    button.textContent = displaySegment(part);
    button.onclick = () => go(destination);
    crumbs.append(button);
  }
  up.disabled = current === "/";
  mkdir.disabled = volumeRoot && current === "/";
  upload.disabled = volumeRoot && current === "/";
}

function opButton(
  label: string,
  className: string,
  act: () => void,
): HTMLButtonElement {
  const button = document.createElement("button");
  button.type = "button";
  button.className = className;
  button.textContent = label;
  button.onclick = act;
  return button;
}

function draw(entries: DavEntry[]): void {
  rows.replaceChildren();
  empty.hidden = entries.length !== 0;
  for (const entry of entries) {
    const tr = document.createElement("tr");
    const name = document.createElement("td");
    const link = document.createElement("a");
    const icon = document.createElement("span");
    icon.className = "icon" + (entry.drawer ? " drawer" : "");
    link.className = "name";
    link.href = entry.drawer ? "#" + cleanDrawer(entry.href) : entry.href;
    link.append(icon, document.createTextNode(entry.name || "(unnamed)"));
    name.append(link);

    const size = document.createElement("td");
    size.className = "size";
    size.textContent = entry.drawer ? "-" : formatSize(entry.size);
    const modified = document.createElement("td");
    modified.className = "modified";
    const time = Date.parse(entry.modified);
    modified.textContent = Number.isNaN(time)
      ? entry.modified
      : new Date(time).toLocaleString();
    const ops = document.createElement("td");
    ops.className = "ops";
    if (!(volumeRoot && current === "/")) {
      if (!entry.drawer) {
        ops.append(opButton("Edit", "edit", () => void editEntry(entry)));
        const download = document.createElement("a");
        download.className = "download";
        download.href = entry.href;
        download.download = entry.name;
        download.textContent = "Download";
        ops.append(download);
      }
      ops.append(
        opButton("Rename", "rename", () => void renameEntry(entry)),
        opButton("Delete", "delete", () => void deleteEntry(entry)),
      );
    }
    tr.append(name, size, modified, ops);
    rows.append(tr);
  }
}

async function load(): Promise<void> {
  const serial = ++loadSerial;
  const path = pathFromHash();
  current = path;
  drawCrumbs();
  say("Reading " + path + " ...");
  try {
    const response = await request(
      "PROPFIND",
      path,
      {
        Depth: "1",
        "Content-Type": "application/xml; charset=utf-8",
      },
      '<?xml version="1.0"?><propfind xmlns="DAV:"><prop>' +
        "<resourcetype/><getcontentlength/><getlastmodified/>" +
        "</prop></propfind>",
    );
    volumeRoot = response.headers.get("X-AmiNetXDuo-Root") === "volumes";
    drawCrumbs();
    const entries = parseListing(await response.text(), path);
    if (serial !== loadSerial) return;
    draw(entries);
    say(entries.length + (entries.length === 1 ? " item" : " items"));
  } catch (error) {
    if (serial !== loadSerial) return;
    draw([]);
    say(error instanceof Error ? error.message : String(error), true);
  }
}

function go(path: string): void {
  const hash = "#" + cleanDrawer(path);
  if (location.hash === hash) void load();
  else location.hash = hash;
}

async function createDrawer(): Promise<void> {
  if (volumeRoot && current === "/") return;
  const name = await ask(
    "New drawer",
    "Choose a name for the new drawer.",
    "Create",
    "",
  );
  if (name === null || name === "") return;
  try {
    say("Creating " + name + " ...");
    await request("MKCOL", childOf(current, name, true));
    await load();
  } catch (error) {
    say(error instanceof Error ? error.message : String(error), true);
  }
}

async function renameEntry(entry: DavEntry): Promise<void> {
  const name = await ask(
    "Rename " + entry.name,
    "Enter the name this item should have.",
    "Rename",
    entry.name,
  );
  if (name === null || name === "" || name === entry.name) return;
  try {
    say("Renaming " + entry.name + " ...");
    await request("MOVE", entry.href, {
      Destination: new URL(childOf(current, name, entry.drawer), location.href)
        .href,
      Overwrite: "F",
    });
    await load();
  } catch (error) {
    say(error instanceof Error ? error.message : String(error), true);
  }
}

async function deleteEntry(entry: DavEntry): Promise<void> {
  const answer = await ask(
    "Delete " + entry.name + "?",
    entry.drawer
      ? "This deletes the drawer and everything in it."
      : "This file will be deleted.",
    "Delete",
  );
  if (answer === null) return;
  try {
    say("Deleting " + entry.name + " ...");
    await request("DELETE", entry.href);
    await load();
  } catch (error) {
    say(error instanceof Error ? error.message : String(error), true);
  }
}

function latin1Decode(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  let text = "";

  /* Avoid both quadratic concatenation and spreading a large file onto the
     JavaScript call stack. */
  for (let at = 0; at < bytes.length; at += 8192) {
    const end = Math.min(at + 8192, bytes.length);
    let part = "";
    for (let i = at; i < end; i++) part += String.fromCharCode(bytes[i]);
    text += part;
  }
  return text;
}

function latin1Encode(text: string): Uint8Array<ArrayBuffer> {
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    const value = text.charCodeAt(i);
    if (value > 255)
      throw new Error(
        "The file contains a character that AmigaOS cannot store as one byte.",
      );
    bytes[i] = value;
  }
  return bytes;
}

function editorText(): string {
  return editorView?.state.sliceDoc() ?? "";
}

function editorDirty(): boolean {
  return editorView !== null && editorText() !== editorSaved;
}

function drawEditorState(message = "Amiga 8-bit text", bad = false): void {
  const dirty = editorDirty();
  editorState.textContent = editorSaving
    ? "Saving..."
    : dirty
      ? "Modified"
      : "Saved";
  editorState.className = dirty ? "changed" : "";
  editorSave.disabled = editorSaving || !dirty;
  editorClose.disabled = editorSaving;
  editorMessage.textContent = message;
  editorMessage.className = bad ? "bad" : "";
}

function editableType(response: Response): boolean {
  const type = (response.headers.get("Content-Type") ?? "")
    .split(";", 1)[0]
    .trim()
    .toLowerCase();
  return (
    type.startsWith("text/") ||
    type === "application/javascript" ||
    type === "application/json" ||
    type === "application/xml"
  );
}

async function editEntry(entry: DavEntry): Promise<void> {
  say("Opening " + entry.name + " ...");
  try {
    const response = await request("GET", entry.href);
    if (!editableType(response)) {
      if (response.body !== null) await response.body.cancel();
      throw new Error(entry.name + " is binary; download it to edit it.");
    }

    const text = latin1Decode(await response.arrayBuffer());
    const separator = text.includes("\r\n")
      ? "\r\n"
      : text.includes("\r")
        ? "\r"
        : "\n";

    editorHref = entry.href;
    editorEtag = response.headers.get("ETag") ?? "";
    editorSaved = text;
    editorName.textContent = entry.name;
    editorPath.textContent = entry.href
      .split("/")
      .map(displaySegment)
      .join("/");
    editorView?.destroy();
    editorView = null;
    editorHost.replaceChildren();
    if (!editorDialog.open) editorDialog.showModal();

    editorView = new EditorView({
      doc: text,
      parent: editorHost,
      extensions: [
        lineNumbers(),
        highlightSpecialChars(),
        undoHistory(),
        drawSelection(),
        EditorState.lineSeparator.of(separator),
        EditorView.lineWrapping,
        keymap.of([
          {
            key: "Mod-s",
            preventDefault: true,
            run: () => {
              void saveEditor();
              return true;
            },
          },
          indentWithTab,
          ...defaultKeymap,
          ...searchKeymap,
          ...historyKeymap,
        ]),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) drawEditorState();
        }),
      ],
    });
    drawEditorState();
    editorView.focus();
    say("Editing " + entry.name);
  } catch (error) {
    say(error instanceof Error ? error.message : String(error), true);
  }
}

async function saveEditor(): Promise<void> {
  if (editorView === null || editorSaving || !editorDirty()) return;

  let bytes: Uint8Array<ArrayBuffer>;
  try {
    bytes = latin1Encode(editorText());
  } catch (error) {
    drawEditorState(error instanceof Error ? error.message : String(error), true);
    return;
  }

  editorSaving = true;
  drawEditorState("Writing " + editorName.textContent + " ...");
  try {
    const headers: Record<string, string> = {
      "Content-Type": "application/octet-stream",
    };
    if (editorEtag !== "") headers["If-Match"] = editorEtag;

    const response = await fetch(editorHref, {
      method: "PUT",
      headers,
      body: new Blob([bytes], { type: "application/octet-stream" }),
      cache: "no-store",
    });
    if (response.status === 412)
      throw new Error(
        "The file changed after it was opened. Close and reopen it before saving.",
      );
    if (!response.ok)
      throw new Error("Save failed (" + response.status + ").");

    editorSaved = editorText();
    try {
      const head = await request("HEAD", editorHref);
      editorEtag = head.headers.get("ETag") ?? "";
    } catch {
      editorEtag = "";
    }
    editorSaving = false;
    drawEditorState("Saved " + editorName.textContent);
    say("Saved " + editorName.textContent);
    await load();
  } catch (error) {
    editorSaving = false;
    drawEditorState(error instanceof Error ? error.message : String(error), true);
  }
}

async function closeEditor(): Promise<void> {
  if (editorSaving) return;
  if (editorDirty()) {
    const answer = await ask(
      "Discard changes?",
      "The changes to " + editorName.textContent + " have not been saved.",
      "Discard",
    );
    if (answer === null) return;
  }
  editorDialog.close();
}

function put(file: File, href: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", href);
    xhr.upload.onprogress = (event) => {
      if (event.lengthComputable) {
        progress.max = event.total;
        progress.value = event.loaded;
      }
    };
    xhr.onload = () =>
      xhr.status >= 200 && xhr.status < 300
        ? resolve()
        : reject(new Error("Upload failed (" + xhr.status + ")"));
    xhr.onerror = () =>
      reject(new Error("Upload failed: the server did not answer."));
    xhr.onabort = () => reject(new Error("Upload cancelled."));
    xhr.send(file);
  });
}

async function uploadFiles(files: FileList | File[]): Promise<void> {
  if (volumeRoot && current === "/") return;
  const list = Array.from(files);
  if (list.length === 0) return;
  progress.hidden = false;
  try {
    for (let i = 0; i < list.length; i++) {
      const file = list[i];
      say(
        "Uploading " + file.name + " (" + (i + 1) + "/" + list.length + ") ...",
      );
      progress.max = file.size || 1;
      progress.value = 0;
      await put(file, childOf(current, file.name));
    }
    await load();
  } catch (error) {
    say(error instanceof Error ? error.message : String(error), true);
  } finally {
    progress.hidden = true;
  }
}

byId("refresh").onclick = () => void load();
up.onclick = () => go(parentOf(current));
mkdir.onclick = () => void createDrawer();
upload.onclick = () => pick.click();
editorSave.onclick = () => void saveEditor();
editorClose.onclick = () => void closeEditor();
editorDialog.addEventListener("cancel", (event) => {
  event.preventDefault();
  void closeEditor();
});
editorDialog.addEventListener("close", () => {
  editorView?.destroy();
  editorView = null;
  editorHost.replaceChildren();
  editorHref = "";
  editorEtag = "";
  editorSaved = "";
});
pick.onchange = () => {
  if (pick.files) void uploadFiles(pick.files);
  pick.value = "";
};
window.addEventListener("hashchange", () => void load());
window.addEventListener("dragenter", (event) => {
  event.preventDefault();
  dragDepth++;
  drop.classList.add("on");
});
window.addEventListener("dragover", (event) => event.preventDefault());
window.addEventListener("dragleave", () => {
  dragDepth--;
  if (dragDepth <= 0) {
    dragDepth = 0;
    drop.classList.remove("on");
  }
});
window.addEventListener("drop", (event) => {
  event.preventDefault();
  dragDepth = 0;
  drop.classList.remove("on");
  if (event.dataTransfer?.files) void uploadFiles(event.dataTransfer.files);
});
window.addEventListener("beforeunload", (event) => {
  if (!editorDirty()) return;
  event.preventDefault();
  event.returnValue = "";
});

if (location.hash.length <= 1) history.replaceState(null, "", "#/");
void load();
