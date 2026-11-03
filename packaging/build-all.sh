#!/usr/bin/env bash
# Builds the release packages this machine can make and collects them in dist/, named by platform:
#   dist/opcan-<version>-windows-x64-setup.exe   (Linux host: MinGW cross-build, see build-windows.sh)
#   dist/opcan-<version>-linux-x86_64.tar.gz     (Linux host: built in an Ubuntu 22.04 container with podman or
#                                                 docker, so it runs on any distro with glibc >= 2.35; needs only
#                                                 libGL, libdbus and libwayland-client, which every desktop has)
#   dist/opcan-<version>-macos-<arch>.dmg        (macOS host only: needs Apple's SDK, which only exists on a Mac)
# usage: packaging/build-all.sh [windows] [linux] [macos]    (default: everything this host can build)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n 's/^set(_opcan_numeric \([0-9.]*\)).*/\1/p' CMakeLists.txt)
host=$(uname -s)
targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
  if [ "$host" = Darwin ]; then targets=(macos); else targets=(windows linux); fi
fi
mkdir -p dist
built=() failed=()

build_windows() {
  [ "$host" = Linux ] || { echo "windows: build it from Linux (MinGW cross-build)" >&2; return 1; }
  packaging/build-windows.sh
}

build_linux() {
  [ "$host" = Linux ] || { echo "linux: build it from Linux" >&2; return 1; }
  local ctr
  ctr=$(command -v podman || command -v docker) || { echo "linux: needs podman or docker" >&2; return 1; }
  # toolchain image (cached after the first run): gcc 12 + CMake >= 3.24 + GLFW/portal build deps
  "$ctr" build -q -t opcan-linux-builder - >/dev/null <<'EOF'
FROM docker.io/library/ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends g++-12 ninja-build pkg-config git curl \
    ca-certificates python3-pip file libgl-dev libdbus-1-dev libwayland-dev wayland-protocols libxkbcommon-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev && pip3 install cmake && rm -rf /var/lib/apt/lists/*
EOF
  # reuse already-downloaded dependency sources from the dev build (slow networks)
  local reuse=""
  for d in build/_deps/*-src; do
    [ -d "$d" ] || continue
    local n; n=$(basename "$d" -src | tr '[:lower:]-' '[:upper:]_')
    reuse+=" -DFETCHCONTENT_SOURCE_DIR_$n=/src/$d"
  done
  "$ctr" run --rm -v "$root:/src" -w /src opcan-linux-builder bash -euc "
    [ -x packaging/stockfish/stockfish ] || packaging/fetch-stockfish.sh packaging/stockfish linux-x86-64
    cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 \
      -DOPCAN_VULKAN=OFF -DGGML_OPENMP=OFF -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' \
      -DOPCAN_STOCKFISH_PATH=/src/packaging/stockfish/stockfish $reuse
    cmake --build build-linux ${JOBS:+-j $JOBS}
    build-linux/opcan_check | tail -1
    cd build-linux && rm -f opcan-*.tar.gz && cpack -G TGZ >/dev/null"
  cp build-linux/opcan-"$version"-linux-x86_64.tar.gz dist/
  echo "dist/opcan-$version-linux-x86_64.tar.gz"
}

build_macos() {
  [ "$host" = Darwin ] || { echo "macos: needs a Mac (Apple's SDK can't be used from Linux); run this script there" >&2; return 1; }
  local arch; arch=$(uname -m)
  [ -x packaging/stockfish/stockfish ] || packaging/fetch-stockfish.sh packaging/stockfish macos
  cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DOPCAN_STOCKFISH_PATH="$root/packaging/stockfish/stockfish"
  cmake --build build-mac ${JOBS:+-j "$JOBS"}
  build-mac/opcan_check | tail -1
  (cd build-mac && rm -f opcan-*.dmg && cpack -G DragNDrop >/dev/null)
  cp build-mac/opcan-"$version"-darwin-"$arch".dmg "dist/opcan-$version-macos-$arch.dmg"
  echo "dist/opcan-$version-macos-$arch.dmg"
}

for t in "${targets[@]}"; do
  echo "== $t"
  if "build_$t"; then built+=("$t"); else failed+=("$t"); fi
done
echo
echo "built: ${built[*]:-none}${failed:+   failed/skipped: ${failed[*]}}"
ls -1 dist/opcan-"$version"-* 2>/dev/null || true
[ ${#failed[@]} -eq 0 ]
