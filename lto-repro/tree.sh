#!/usr/bin/env bash
# Build the tree in one arm and report per-artefact sizes as key=value.
#
#   tree.sh <arm-name> <toolchain-prefix> [lto]
set -u
ARM="${1:?arm}"
TC="${2:?toolchain prefix}"
LTO="${3:-}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B="$ROOT/build/$ARM"
cd "$ROOT"

export AMIGA_TOOLCHAIN_ROOT="$TC"

args=(-S . -B "$B"
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
      -DCMAKE_BUILD_TYPE=Release)
if [ "$LTO" = "lto" ]; then
    args+=(-DCMAKE_PROJECT_INCLUDE="$ROOT/.repro/lto.cmake"
           -DCMAKE_AR="$TC/bin/m68k-amigaos-gcc-ar"
           -DCMAKE_RANLIB="$TC/bin/m68k-amigaos-gcc-ranlib"
           -DCMAKE_NM="$TC/bin/m68k-amigaos-gcc-nm")
fi

rm -rf "$B"
mkdir -p "$ROOT/build"
if ! cmake "${args[@]}" > "$ROOT/build/$ARM-configure.log" 2>&1; then
    echo "arm=$ARM configure=fail"
    tail -20 "$ROOT/build/$ARM-configure.log"
    exit 1
fi

if cmake --build "$B" --parallel 24 > "$ROOT/build/$ARM-build.log" 2>&1; then
    echo "arm=$ARM build=ok"
else
    echo "arm=$ARM build=FAIL"
    grep -nE "error:|Error [0-9]|FATAL_ERROR|CMake Error" "$ROOT/build/$ARM-build.log" | head -25
fi

for f in $(cd "$B" && find . -name '*.library' -o -name 'AmiNetXDuo*' -type f -perm -u+x 2>/dev/null | sort); do
    :
done

# Report every artefact the archive ships.
while read -r p; do
    [ -f "$p" ] || continue
    printf 'arm=%s artefact=%s bytes=%s\n' "$ARM" "$(basename "$p")" "$(wc -c < "$p" | tr -d ' ')"
done < <(find "$B" -type f \( -name '*.library' -o -name '*.device' \) ! -name '*.map' | sort
         find "$B/src/tools" -maxdepth 1 -type f ! -name '*.map' ! -name '*.o' \
              ! -name 'Makefile' ! -name '*.cmake' -perm -u+r 2>/dev/null | sort)
