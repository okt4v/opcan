#!/usr/bin/env bash
# Downloads the pinned official Stockfish release for this platform, checks its sha256 and puts the
# binary at <out>/stockfish (+ its GPL license). Use with: cmake -DOPCAN_STOCKFISH_PATH=<out>/stockfish
# usage: packaging/fetch-stockfish.sh [out-dir] [linux-x86-64|linux-arm64|macos]
set -euo pipefail
TAG=sf_19
out=${1:-packaging/stockfish}
plat=${2:-}
if [ -z "$plat" ]; then
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) plat=linux-x86-64 ;;
    Linux-aarch64) plat=linux-arm64 ;;
    Darwin-*) plat=macos ;;
    *) echo "unsupported platform $(uname -sm); on Windows use fetch-stockfish.ps1" >&2; exit 1 ;;
  esac
fi
case "$plat" in
  linux-x86-64) sha=9defc0d4e55d49c65a6d042f3e571a39fcea499ade6dbe741b53b8c65e03611f ;;
  linux-arm64)  sha=fe26cfd1d9db4c8af3d21e24d9ff34cacb31c1f940085a7583da11796f2bac01 ;;
  macos)        sha=a1f0e3bcc5a6927a11fe6fc8e54a779754645f3c2bae2cf13420fd1957adaa77 ;;
  *) echo "unknown platform $plat" >&2; exit 1 ;;
esac
name=stockfish-$plat-universal.tar.gz
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 5 --retry-all-errors -C - -o "$tmp/$name" "https://github.com/official-stockfish/Stockfish/releases/download/$TAG/$name"
if command -v sha256sum >/dev/null; then got=$(sha256sum "$tmp/$name" | cut -d' ' -f1)
else got=$(shasum -a 256 "$tmp/$name" | cut -d' ' -f1); fi
[ "$got" = "$sha" ] || { echo "sha256 mismatch for $name: $got" >&2; exit 1; }
mkdir -p "$tmp/x" "$out"
tar -xzf "$tmp/$name" -C "$tmp/x"
# the executable is the stockfish-* file that isn't source/docs
bin=$(find "$tmp/x" -type f -name 'stockfish*' ! -name '*.*' | head -n1)
[ -n "$bin" ] || { echo "no stockfish binary in $name" >&2; exit 1; }
install -m 755 "$bin" "$out/stockfish"
lic=$(find "$tmp/x" -type f -name 'Copying.txt' | head -n1)
[ -n "$lic" ] && cp "$lic" "$out/stockfish-COPYING.txt"
echo "stockfish $TAG -> $out/stockfish"
