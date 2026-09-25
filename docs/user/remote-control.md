# Host-side remote control for `httpd`

`tools/web/remote-control.mjs` wraps the existing `/shell`, WebDAV and
`/console` interfaces in one machine-readable command. It runs on the host;
there is no new Amiga-side command or server endpoint. Node.js 26 is tested. For
screenshots and graphical input, install the existing web-build dependencies
with `npm ci --prefix tools/web`; Shell and file transfers need only Node.js.
The root `GET /` page includes a short HTML comment advertising the endpoints
that were enabled when `httpd` started.

Set the address once, then call it from scripts or an AI tool runner:

```sh
export ANXD_URL=http://amiga.local:8080
node tools/web/remote-control.mjs run 'Version'
node tools/web/remote-control.mjs --session 0 run 'Version'
node tools/web/remote-control.mjs --timeout 60 run 'ShowNetStatus EVENTS'
node tools/web/remote-control.mjs get /DH0/report.txt ./report.txt
node tools/web/remote-control.mjs put ./config.txt /DH0/config.txt
node tools/web/remote-control.mjs screenshot ./workbench.png
node tools/web/remote-control.mjs click 120 42 ./after-click.png
node tools/web/remote-control.mjs key Enter ./after-enter.png
```

Every successful call prints one JSON object. `run` returns `exitCode`,
`output` and `durationMs`; its process exit status is 1 when the Amiga command
returned nonzero, 2 on a transport or adapter error. `output` is the Shell's
terminal stream, not separated stdout/stderr. The adapter removes ordinary
echoes of the command and its completion probe, but interactive programs and
control sequences can still appear. It uses a random completion marker and
captures `$RC` immediately after the command, with a configurable timeout and
1 MiB output limit (`--max-output BYTES`). A timeout interrupts and closes the
Shell session; it does not prove that a spawned background task stopped.

`/shell` is slot 0, and `/shell?session=1` is an independent Shell. The host
tool defaults to slot 1 so it does not collide with a browser on slot 0; use
`--session 0` to select the browser's slot. Each slot has one socket owner.
After a socket disconnects, a still-running Shell (including current directory
and Shell variables) remains available for reconnection for five minutes,
then expires.
It does not survive an `httpd` restart or machine reboot. Buffered output is
bounded and bytes already taken by a lost socket may not be replayed. `take=1`
can displace a live socket only in the selected slot; it does not grant access
control or create another slot.

`get` and `put` use HTTP paths such as `/DH0/file`, not AmigaDOS paths such as
`DH0:file`. `put` overwrites the remote file and `get` overwrites the local
file. Neither makes backups. `screenshot` saves a native-pixel PNG decoded by
the same tested code as the browser console. `click` and `key` act on that
console and save a PNG after the action. Click coordinates are native screen
pixels; `key` names a physical key in `src/tools/web/client/console/rawkey.ts`
(for example `Enter`, `F1`, `ArrowLeft`). The screenshot does not include the
hardware pointer sprite. A changed frontmost screen causes a new geometry
before capture.

Each Shell slot and the console have one owner. If a browser already holds the
selected slot or console, the adapter fails rather than taking it away. This tool does not add access
control: `httpd` remains wide open to anything that can reach its port.

Run the host-side protocol tests with:

```sh
node --test tools/web/remote-control.test.mjs
```
