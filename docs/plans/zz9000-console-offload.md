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

Reserve **`service_id = 0x8200`** ("AmiNetXDuo console encoder"); recorded in
the SDK fork's vendor table (`docs/zz9k-vendor-services.md`).

The SDK ships the manifest/registry metadata (`module.h`, `service_registry.h`)
but **no public loader ABI** -- `docs/zz9k-modules.md` is explicit that handler
tables stay runtime-private and there is no ELF loader in the SDK.  So the
`0x8200` handler is **firmware code, built into / loaded by ZZ9000OS** exactly
as the JEDI `0x8100` renderer is ("ships as a loadable module").  **httpd never
loads anything** -- it only discovers the service through `QUERY_SERVICE` and
uses it, or falls back.  The handoff is therefore a firmware build, not a
binary AmiNetXDuo can drop into its archive and load itself.

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

- The browser has **gzip/deflate natively** (`DecompressionStream`), so
  **`HTTPZZ_CODEC_DEFLATE`** (raw DEFLATE, `deflate-raw`) is the zero-dependency
  win -- no JS added to `shell.html`.  **LZ4** needs a compact block decoder in
  JS/WASM.  Note the SDK's `compression.h` enumerates DEFLATE/ZLIB/GZIP/
  `LZ4_BLOCK`/LZMA/LH* but **no zstd**; `HTTPZZ_CODEC_ZSTD` stays reserved in the
  enum but is not the SDK path.
- The RFB stream is *already* tile-delta + PackBits, so a second pass buys less
  than on raw pixels and costs ARM cycles.  v1 ships **`NONE`**; the `codec`
  field is the negotiation hook.  Turn a codec on only when a measurement on
  real frames (throughput vs bandwidth) says it pays -- and only DEFLATE needs
  no client-side code.  The compressor is the module's (the SDK CODEC only
  *decompresses*).
- Order on the wire: `RFB encode -> [DEFLATE|LZ4|none] -> WebSocket`; the client
  reverses it. The reply's `codec` field says which was used; the host disables
  the offload if the card ever answers a codec the client was not told to
  expect (see `httpfb.c`).

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

## Reference handler (firmware)

A drop-in core for the ZZ9000OS `0x8200` handler.  It shares AmiNetXDuo's wire
contract (`src/tools/httpzz.h`) and encoder (`src/rfblib`, built for the ARM);
only the surface/shared-buffer handle lookups and the big-endian byte helpers
are firmware-supplied (they already exist in ZZ9000OS).  All request integers
arrive big-endian; the reply overwrites the inline payload in place.  The card
frames bands from the geometry in the request -- never from the surface's own
dimensions -- so `tiles_x`/`tiles_y` and every delta match the host byte for
byte.  The scroll config is `rfb_scroll_defaults()` on both sides, so it is not
sent.

```c
#include "zz9k/abi.h"
#include "aminetxduo/rfb_encode.h"   /* the shared encoder, built for ARM */
#include "httpzz.h"                  /* the shared wire contract          */
#include <string.h>

/* Firmware-supplied (already in ZZ9000OS): resolve a surface / shared-buffer
   handle to a local ARM pointer, and big-endian byte access. */
extern void    *zz_surface_base(uint32_t handle);              /* ZZ9KSurface.arm_addr */
extern void    *zz_shared_base(uint32_t handle, uint32_t *cap);
extern uint16_t be16(const uint8_t *p);
extern uint32_t be32(const uint8_t *p);
extern void     put_be16(uint8_t *p, uint16_t v);
extern void     put_be32(uint8_t *p, uint32_t v);

/* One console, one delta baseline.  Shadow/scratch live in DDR, sized by
   rfb_shadow_size()/rfb_scratch_size(); (re)allocate on a geometry change. */
static rfb_encoder g_enc;
static uint8_t    *g_shadow, *g_scratch;
static rfb_geom    g_geom;
static rfb_u32     g_flags;
static int         g_ready;

static int geom_same(const rfb_geom *a, const rfb_geom *b) {
    return a->width==b->width && a->height==b->height &&
           a->bytes_per_row==b->bytes_per_row && a->depth==b->depth &&
           a->tile_w==b->tile_w && a->tile_h==b->tile_h && a->format==b->format;
}

/* Dispatched by the firmware for opcode HTTPZZ_OP_ENCODE (== 0x8200). */
int httpzz_service_encode(ZZ9KMailboxEntry *entry) {
    const uint8_t *p = entry->payload.inline_data;   /* HttpZzEncodeReq, BE */
    uint32_t surface_h = be32(p +  0), out_h = be32(p +  4);
    uint32_t out_cap   = be32(p +  8), enc_flags = be32(p + 12);
    rfb_geom g;
    uint16_t ty0 = be16(p + 22), ty1 = be16(p + 24);
    uint16_t req_flags = be16(p + 26);               /* HTTPZZ_F_*          */
    const rfb_u8 *planes[1];
    uint8_t *out; uint32_t buf_cap = 0; long n;

    g.width = be16(p + 16); g.height = be16(p + 18);
    g.bytes_per_row = be16(p + 20);
    g.depth = p[30]; g.tile_w = p[31]; g.tile_h = p[32]; g.format = p[33];

    out = zz_shared_base(out_h, &buf_cap);
    planes[0] = zz_surface_base(surface_h);
    if (!out || !planes[0]) { entry->status = ZZ9K_STATUS_BAD_HANDLE; return -1; }
    if (out_cap < buf_cap) buf_cap = out_cap;

    if (!g_ready || g_flags != enc_flags || !geom_same(&g, &g_geom) ||
        (req_flags & HTTPZZ_F_RESET)) {
        rfb_scroll_cfg cfg; rfb_scroll_defaults(&cfg);      /* == the host    */
        /* (re)size g_shadow to rfb_shadow_size(&g), g_scratch to
           rfb_scratch_size(&g, enc_flags, &cfg); then: */
        memset(g_shadow, 0, rfb_shadow_size(&g));           /* zero => full   */
        if (rfb_encoder_init(&g_enc, &g, enc_flags, &cfg,
                             g_shadow, rfb_shadow_size(&g),
                             g_scratch, rfb_scratch_size(&g, enc_flags, &cfg))) {
            entry->status = ZZ9K_STATUS_INTERNAL_ERROR; return -1;
        }
        g_geom = g; g_flags = enc_flags; g_ready = 1;
    }

    n = rfb_encode_band(&g_enc, planes, out, buf_cap, ty0, ty1);
    if (n < 0) { entry->status = ZZ9K_STATUS_NO_MEMORY; return -1; }

    put_be32(entry->payload.inline_data + 0, (uint32_t)n);   /* out_len       */
    put_be16(entry->payload.inline_data + 4, HTTPZZ_CODEC_NONE);
    put_be16(entry->payload.inline_data + 6, ty0 == 0 ? HTTPZZ_RF_KEYFRAME : 0);
    entry->payload_len = 8;                                  /* HttpZzEncodeReply */
    entry->status = ZZ9K_STATUS_OK;
    return 0;
}
```

Manifest: one `ZZ9KModuleServiceDesc` via `zz9k_module_service_init(&d,
0x8200, /*version*/1, /*capability_bits*/0, ZZ9K_SERVICE_FLAG_MODULE,
/*opcode_base*/0x8200, /*opcode_count*/1, /*max_inline_payload*/48,
"consenc")`.  DDR working set at 640x480 BGRA: one shadow frame (~1.2 MB) +
scratch, trivial for the Zynq.  `rfb_encode` is endian-clean portable C; build
it little-endian for the ARM with the same `RFB_*` config the host uses.

## Build / dist

- httpd compiles against `third_party/zz9000-sdk/include`; links `zz9k.library`
  by name at runtime (no hard dependency -- `OpenLibrary` may fail, that's the
  fallback).  The host side is complete and cross-compiles into `tool_httpd`.
- The `0x8200` handler is **firmware**, built with the Zynq toolchain in the
  zz9000-firmware repo and provided by ZZ9000OS the way JEDI `0x8100` is.  There
  is no AmiNetXDuo archive artifact to ship or load -- httpd auto-detects the
  service at runtime and uses it, so the AmiNetXDuo deliverable needs **no
  change** once the firmware carries the service.  (If a future firmware grows a
  public side-load path, the module file could ride in the archive; that is a
  firmware capability, not a host one, and is out of scope here.)

## Firmware build & flash (done 2026-09-18)

The `0x8200` handler is built into ZZ9000OS and staged on the A3000's card.
The remote path -- no case opening, no SD removal -- is `ZZFwUpdate`.

Base: `BlitterStudio/zz9000-firmware` `v2.8.0-rc3` (the A3000 already runs a
v2.8.0-rc build; `zz9k.library 2.29`, SDK ABI 2.0).

Integration (patch: `AmiNetXDuo:zz9000-fw-console-encode.patch`, also on the
A3000):
- `ZZ9000OS/src/sdk_mailbox.c`: `handle_console_encode()` + a persistent
  `rfb_encoder` (shadow/scratch `malloc`'d, seq preserved across resets); the
  `0x8200` row in `sdk_services[]`; the `SDK_OP_CONSOLE_ENCODE` dispatch case;
  `#include <aminetxduo/rfb_encode.h>` + `<stdlib.h>`.  It resolves the
  displayed framebuffer (`get_surface_info`), validates the host geometry
  against the real surface, `prepare_surface_for_arm_read`s it, runs
  `rfb_encode_band` into the shared out buffer, `Xil_DCacheFlushRange`es it,
  and writes the `HttpZzEncodeReply`.
- `ZZ9000OS/src/sdk_mailbox.h`: `SDK_SERVICE_CONSOLE`/`SDK_OP_CONSOLE_ENCODE`
  `0x8200`, `SDK_CAP_CONSOLE_ENCODE (1U << 28)`.
- `ZZ9000OS/src/rfb/` + `src/aminetxduo/`: the portable encoder, byte-identical
  to the 68k fallback (only `<string.h>`); added to the Makefile `C_SRCS`.

Build (ARM-only change, no Vivado -- committed bitstream/FSBL):
```
./build_firmware.sh        # arm-none-eabi-gcc; downloads deps
BOOTGEN=<path>/bootgen ./build_bootimage.sh   # -> bootimage_work/BOOT.bin
```
Sandbox-toolchain notes (Debian's stricter GCC 14, NOT needed with the official
Arm GNU Toolchain the repo expects): demote the GCC-14 permerrors --
`build_libpng.sh` +`-Wno-error=incompatible-pointer-types`, `ZZ9000OS/Makefile`
+`-Wno-int-conversion -Wno-implicit-int`.  `bootgen` was built from
`Xilinx/bootgen` source.

Flash (staged, not yet activated): `ZZFwUpdate AmiNetXDuo:BOOT.bin BOOT.bin`
wrote 3,839,744 bytes to `0:/BOOT.bin` (old kept as `BOOT.bak`).

## Status

- [x] host side: `httpzz` detect + encode + fallback, `httpfb.c` offload,
      geometry-carrying contract, layout assertion (merged to `main`)
- [x] `0x8200` firmware handler built into ZZ9000OS (v2.8.0-rc3 base)
- [x] `BOOT.bin` built and staged on the A3000 SD via `ZZFwUpdate` (`BOOT.bak` kept)
- [ ] **power-cycle the A3000 to activate, then validate `/console` offload**
      (`ZZFwUpdate RESTORE` rolls back if it misbehaves)
- [ ] optional `DEFLATE` wrap (browser-native decode), measured choice -- deferred
- [ ] Zorro II support -- deferred (v1 is Zorro III / A3000)

No AmiNetXDuo archive change is needed: httpd auto-detects the service and uses
it once the firmware provides it.
