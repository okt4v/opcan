#!/usr/bin/env bash
# Cross-compiles opcan for 64-bit Windows on Linux and builds the installer:
#   dist/opcan-<version>-windows-x64-setup.exe
# Runs itself inside `nix develop .#windows` (MinGW-w64 GCC, CMake, NSIS); everything is linked statically, so the
# installer needs nothing else on the target PC. Stockfish (official sf_19 Windows build, sha256-checked) is bundled.
# usage: packaging/build-windows.sh [extra cmake args...]   env: BUILD_DIR (default build-win), JOBS
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
if [ -z "${OPCAN_WIN_SHELL:-}" ]; then
  exec nix develop "$root#windows" -c env OPCAN_WIN_SHELL=1 "$0" "$@"
fi

build=${BUILD_DIR:-build-win}
version=$(sed -n 's/^project(opcan VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || { echo "no version in CMakeLists.txt" >&2; exit 1; }

# Stockfish: packaging/stockfish-win/{stockfish.exe,stockfish-COPYING.txt,stockfish-AUTHORS.txt}
sfdir=packaging/stockfish-win
if [ ! -f $sfdir/stockfish.exe ]; then
  sha=3c8bf1f9ea66a09350a40df4f632288285ac206d99f33ab5842c408fc30b48a7  # same pin as fetch-stockfish.ps1
  name=stockfish-windows-x86-64-universal.zip
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  curl -fsSL --retry 5 --retry-all-errors -C - -o "$tmp/$name" \
    "https://github.com/official-stockfish/Stockfish/releases/download/sf_19/$name"
  got=$(sha256sum "$tmp/$name" | cut -d' ' -f1)
  [ "$got" = "$sha" ] || { echo "sha256 mismatch for $name: $got" >&2; exit 1; }
  unzip -q "$tmp/$name" -d "$tmp/x"
  bin=$(find "$tmp/x" -type f -iname 'stockfish*.exe' | head -n1)
  [ -n "$bin" ] || { echo "no stockfish exe in $name" >&2; exit 1; }
  mkdir -p $sfdir
  cp "$bin" $sfdir/stockfish.exe
  lic=$(find "$tmp/x" -type f -name 'Copying.txt' | head -n1)
  [ -n "$lic" ] && cp "$lic" $sfdir/stockfish-COPYING.txt
  au=$(find "$tmp/x" -type f -name 'AUTHORS' | head -n1)
  [ -n "$au" ] && cp "$au" $sfdir/stockfish-AUTHORS.txt
fi

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$root/packaging/mingw-w64.cmake" \
  -DOPCAN_VULKAN=OFF -DOPCAN_STOCKFISH_PATH="$root/$sfdir/stockfish.exe" "$@"
cmake --build "$build" --target opcan ${JOBS:+-j "$JOBS"}

# only Windows system DLLs may be imported (no libstdc++/libgcc/winpthread/mcfgthread DLLs)
bad=$(x86_64-w64-mingw32-objdump -p "$build/opcan.exe" | sed -n 's/.*DLL Name: //p' \
  | grep -viE '^(kernel32|user32|gdi32|shell32|ole32|oleaut32|opengl32|comdlg32|ws2_32|advapi32|msvcrt|ucrtbase|api-ms-win-[a-z0-9-]*|shlwapi|uuid|winmm|imm32|dwmapi|version|bcrypt|ntdll|uxtheme|comctl32|setupapi|crypt32|secur32|psapi|dbghelp|propsys|avrt|ksuser)\.dll$' || true)
[ -z "$bad" ] || { echo "opcan.exe imports non-system DLLs: $bad" >&2; exit 1; }

stage="$build/stage"
rm -rf "$stage"
cmake --install "$build" --component opcan --prefix "$stage"
[ -f $sfdir/stockfish-AUTHORS.txt ] && cp $sfdir/stockfish-AUTHORS.txt "$stage/engines/"
x86_64-w64-mingw32-strip "$stage/opcan.exe"

mkdir -p dist
out="dist/opcan-$version-windows-x64-setup.exe"
makensis -V2 -DVERSION="$version" -DSTAGE="$root/$stage" -DOUTFILE="$root/$out" packaging/opcan.nsi
echo "$out ($(du -h "$out" | cut -f1))"
