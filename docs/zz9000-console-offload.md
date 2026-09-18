# ZZ9000 console offload

Offload the httpd console (`httpd -C`, `/console`) framebuffer **readback +
encode** to the ZZ9000's Zynq (dual Cortex-A9 + FPGA), so the two slow parts --
the ~1 MB/frame Zorro readback and the 68030 RFB encode -- happen on the card
instead.  Auto-detected; falls back to the existing path when the card, the
SDK library, or the service is absent.

**v1 scope: Zorro III only** (the A3000).  Zorro II (the 64 KiB host heap the
SDK's `docs/zz9k-zorro2-services.md` warns about) is a later refinement.

## Why a loadable module, not existing services

The SDK's SURFACE service exposes the displayed framebuffer
(`ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE`, native format `BGRA8888`) and CODEC/IMAGE
exist -- but CODEC only **decompresses** (`ZZ9K_OP_DECOMPRESS*`, no deflate/
encode op) and IMAGE only **decodes** (`DECODE_PNG/JPEG`, `SCALE_IMAGE`).  There
is no encode op, so the encoder is ours.  It goes in a **loadable vendor
service** (service space `0x8000+`; JEDI already owns `0x8100`), per
`third_party/zz9000-sdk/docs/zz9k-vendor-services.md`.

Reserve **`service_id = 0x8200`** ("AmiNetXDuo console encoder").  Record it in
the SDK's vendor table.

## Architecture

- **ARM module (`0x8200`)** -- built in the zz9000-**firmware** repo with the
  Zynq toolchain, shipped as a loadable module.  It:
  1. maps the displayed framebuffer surface (`MAP_FRAMEBUFFER_SURFACE`),
  2. runs **`aminetxduo_rfb`** (`src/rfblib`, `rfb_encode.h`) -- our existing,
     host-tested, portable-C encoder -- reading the surface locally (no Zorro),
     keeping the previous frame in DDR for COPYRECT/scroll delta,
  3. writes the RFB stream to a shared buffer for the host.
  The browser client is **unchanged** -- it already decodes this stream.
- **Host (httpd)** -- new `src/tools/httpzz.c`, a peer of `httprtg.c`:
  - detect: `OpenLibrary("zz9k.library", 2)`; `ZZ9KQueryCaps`; `ZZ9KQueryService(0x8200)`.
    Missing library / caps / service ⇒ return "unavailable", httpfb.c uses the
    current path.
  - per band/frame: `MAP_FRAMEBUFFER_SURFACE`, `ALLOC_SHARED` for the output,
    `ZZ9KCall(0x8200 encode, &req, &reply)`, read the RFB bytes from the shared
    buffer, hand them to httpfb.c's WebSocket sender.
  - `httpfb.c` chooses the zz9k path when `httpzz_available()` and the console
    screen is on the ZZ9000 (RTG), else the existing `httprtg.c` readback +
    `rfb_encode`.

## Bandwidth: optional LZ4/zstd on top of RFB

The RFB stream already tiles + deltas, but the ARM can compress its output
further before it crosses the wire.  Negotiated, not fixed:

- The browser advertises what it can inflate.  It has **gzip/deflate natively**
  (`DecompressionStream`); **LZ4 and zstd need a small JS/WASM lib** bundled
  into `shell.html` (e.g. a compact LZ4 block decoder, or `fzstd` for zstd).
- The ARM module wraps the RFB bytes in the best codec both sides agree on and
  the Zynq can sustain at frame rate.  **LZ4** (fast, modest ratio) vs **zstd**
  (slower, better ratio) is a **measured** choice per the `zz9k-m68kbench`-style
  model -- pick by throughput vs bandwidth on real frames.  The module bundles
  the encoder (both are portable C; the SDK's CODEC only *decompresses*, so the
  compressor is ours, alongside `rfb_encode`).
- Order on the wire: `RFB encode -> [LZ4|zstd|none] -> WebSocket`; the client
  reverses it. A `comp`/format tag on the frame says which was used.

## ABI reference (from the submodule)

Host request builders are header-only in `host/include/zz9k/request.h`
(`zz9k_request_query_service`, `_query_caps`, `_alloc_shared`,
`_alloc_surface`, `_mem_copy`, ...); `ZZ9KRequest` is in `host/include/zz9k/host.h`
and, for AmigaOS, `amiga/include/zz9k/library.h` (+ `proto/zz9k.h`,
`clib/zz9k_protos.h`, `fd/zz9k_lib.fd`).  `ZZ9KCall(lib, &request, &reply, timeout)`.


- Host FD: `third_party/zz9000-sdk/amiga/fd/zz9k_lib.fd`
  (`ZZ9KQueryCaps`, `ZZ9KQueryService`, `ZZ9KCall`, `ZZ9KCallAsync*`).
- Ops/structs: `include/zz9k/abi.h` (`ZZ9K_OP_*`, request/reply,
  `ZZ9KQueryServicePayload`), `surface.h` (`ZZ9K_SURFACE_FORMAT_BGRA8888`,
  `MAP_FRAMEBUFFER_SURFACE`, flags `FRAMEBUFFER`/`DISPLAYED`), `caps.h`
  (`ZZ9K_SERVICE_SURFACE`→`FRAMEBUFFER_SURFACE`, detection helpers),
  `module.h` (`ZZ9KModuleServiceDesc`, `zz9k_module_service_init`),
  `compression.h`, `sdk.h`.
- Discovery contract: a not-registered service is "feature absent" -- MUST fall
  back (`docs/zz9k-vendor-services.md`).

## Build / dist

- httpd compiles against `third_party/zz9000-sdk/include`; links `zz9k.library`
  by name at runtime (no hard dependency -- `OpenLibrary` may fail, that's the
  fallback).
- The ARM module binary ships in the AmiNetXDuo archive (e.g. `Devs/` or a
  `zz9000/` drawer); httpd loads it via the MODULE service if the firmware has
  not already, then uses it.  **Building that binary is the firmware repo's job
  (Zynq toolchain) -- host side is complete and auto-detects once it is present.**

## Status

- [x] submodule `third_party/zz9000-sdk`
- [ ] `0x8200` reserved in the SDK vendor table
- [ ] ARM module source (rfb_encode over MAP_FRAMEBUFFER_SURFACE)
- [x] host `httpzz.c` -- detect (`httpzz_available`) + encode call + fallback, cross-compiles
- [ ] wire `httpfb.c` to prefer the offload when available (RTG screen)
- [x] CMake: httpd sees the SDK headers (tool_httpd include dirs)
- [ ] dist ships the ARM module
- [ ] host tests for the detect/format glue; cross-compile clean
- [ ] optional LZ4/zstd wrap (ARM encoder + client decoder), measured choice
- [ ] handoff: firmware build of the module, on-card validation
