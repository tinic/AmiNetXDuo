# Build spike (throwaway)

Minimal `tx_port.h` / `nx_port.h` / `nx_user.h` used to prove that ThreadX and NetX Duo
compile for m68k-amigaos. **Not a working port** — `TX_DISABLE`/`TX_RESTORE` are
placeholders and no scheduler, timer or driver exists yet.

Provenance: `tx_port.h` and `nx_user.h` are written from scratch; `nx_port.h` is
`netxduo/ports/linux/gnu/inc/nx_port.h` (MIT, © Microsoft / Eclipse ThreadX
contributors) with `NX_LITTLE_ENDIAN` removed for big-endian m68k.

Reproduce (with `netxduo` and `threadx` checked out beside this repo):

```sh
G=m68k-amigaos-gcc
for f in threadx/common/src/*.c netxduo/common/src/*.c; do
  $G -c -O2 -m68020 -fomit-frame-pointer \
     -Ispike/amiport -Ithreadx/common/inc -Inetxduo/common/inc "$f" -o /dev/null
done
```

Result on 2026-07-24 with GCC 15.2.0: 185/185 ThreadX and 511/511 NetX Duo files compile
with zero failures. See `docs/RESEARCH.md` §5.4.
