#!/usr/bin/env bash
# Runs UI scripts (tests/ui/*.txt) against a scratch copy of a database; screenshots land in <outdir>.
# usage: tests/ui/run.sh <outdir> [script.txt...]    env: OPCAN_BIN (default build/opcan), DB (source db; default: fresh)
# Script headers: "#env K=V ..." sets env vars ($ROOT, $OUT expand; a later OPCAN_DB=$OUT/.db-<script>/opcan.db reuses that run's db); "#needs-db" skips the script without DB.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(realpath -m "${1:?usage: run.sh <outdir> [scripts...]}"); shift
bin=$(realpath -m "${OPCAN_BIN:-$root/build/opcan}")
mkdir -p "$out"
[ $# -gt 0 ] || set -- "$root"/tests/ui/*.txt
fail=0
for s in "$@"; do
  s=$(realpath "$s"); name=$(basename "$s" .txt)
  if grep -q '^#needs-db' "$s" && [ -z "${DB:-}" ]; then echo "skip $name (needs DB=)"; continue; fi
  work="$out/.db-$name"; rm -rf "$work"; mkdir -p "$work"
  [ -n "${DB:-}" ] && cp "$DB" "$work/opcan.db"
  envs=$(sed -n 's/^#env //p' "$s" | sed "s#\\\$ROOT#$root#g; s#\\\$OUT#$out#g")
  echo "run  $name"
  (cd "$root" && env OPCAN_DB="$work/opcan.db" OPCAN_SCRIPT="$s" OPCAN_SHOT_DIR="$out" $envs timeout 300 "$bin") >"$out/$name.log" 2>&1 \
    || { echo "FAIL $name (see $out/$name.log)"; fail=1; }
done
exit $fail
