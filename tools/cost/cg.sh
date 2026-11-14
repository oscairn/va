#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Instruction cost of the DSP core with callgrind (the regression gate; docs/cost.md).
# Runs on aarch64 Linux with g++ and valgrind, e.g. in the device-like container:
#   docker run --rm -v "$PWD":/src:ro -v /tmp/cg:/out IMAGE sh /src/tools/cost/cg.sh /src /out [check]
# Builds tools/vcbench.cpp at -O3 -mcpu=cortex-a53, runs every case with one callgrind dump per
# measured 128-frame block, and prints instructions per block (mean, max). With "check" it fails
# (exit 1) when a case's mean is more than 10 % over tools/cost/baseline.txt.
set -e
SRC=$1; OUT=$2; MODE=$3
F="-std=c++17 -O3 -DNDEBUG -mcpu=cortex-a53"
mkdir -p "$OUT/runs"
g++ $F "$SRC/core/core.cpp" "$SRC/core/laws.cpp" "$SRC/core/patch.cpp" "$SRC/tools/vcbench.cpp" -o "$OUT/vcbench"
# the plugin as a host loads it (VA_FLAGS: extra compile flags), and the hosted driver (the shell's cost: hosted-* cases)
I="-I$SRC/third_party/clap/include -I$SRC/third_party/oscairn-clap/include"
g++ $F $VA_FLAGS -shared -fPIC -fvisibility=hidden $I "$SRC/src/plugin.cpp" "$SRC/src/core_engine.cpp" "$SRC/core/core.cpp" "$SRC/core/laws.cpp" "$SRC/core/patch.cpp" -o "$OUT/va.clap"
g++ $F $I "$SRC/tools/hostbench.cpp" -o "$OUT/hostbench" -ldl
run() {  # name args...
  n=$1; shift; d="$OUT/runs/$n"; rm -rf "$d"; mkdir -p "$d"
  case "$n" in
  hosted*) valgrind --tool=callgrind --dump-line=no --callgrind-out-file="$d/cg.out" "$OUT/hostbench" "$OUT/va.clap" "$@" > "$d/out.txt" 2> "$d/err.txt";;
  *) valgrind --tool=callgrind --dump-line=no --callgrind-out-file="$d/cg.out" "$OUT/vcbench" cg "$@" "$d/v.tsv" > "$d/out.txt" 2> "$d/err.txt";;
  esac
  # per-block totals (dumps 1..N); the first measured block is dropped (warm caches of the tool itself)
  ls "$d"/cg.out.* | sort -t. -k3 -n | while read -r f; do sed -n "s/^totals: \([0-9]*\).*/\1/p" "$f"; done > "$d/blocks.txt"
  awk -v n="$n" 'NR>1 {s+=$1; c++; if ($1>m) m=$1} END {printf "%-10s blocks %5d  mean %8.0f  max %8.0f\n", n, c, s/c, m}' "$d/blocks.txt"
  case "$n" in shed*) echo "  $n first blocks (the shed block first): $(head -4 "$d/blocks.txt" | tr "\n" " ")";; esac
  rm -f "$d"/cg.out.*
}
{
run rest rest 0.5 0.5
for v in 1 4 8 16; do run held$v held $v 0.5 0.5; done
run held8-L1 held 8 0.5 0.5 lanes1
run pattern pattern 2 2
run shed8 shed 8 0.5 0.02
run shed16 shed 16 0.5 0.02
run hosted-rest rest 0.5 0.5
run hosted-held8 held 8 0.5 0.5
run hosted-pattern pattern 2 2
if [ -d "$SRC/tools/cost/plans" ] && [ "$MODE" != check ]; then
  for p in "$SRC"/tools/cost/plans/*.patch; do VC_PATCH="$p" run "plan-$(basename "$p" .patch)" held 8 0.5 0.5; done
fi
} | tee "$OUT/summary.txt"
if [ "$MODE" = check ]; then
  fail=0
  while read -r name base; do
    case "$name" in ''|'#'*) continue;; esac
    got=$(awk -v n="$name" '$1==n {print $5}' "$OUT/summary.txt")
    if [ -z "$got" ] || [ "$(awk -v g="$got" -v b="$base" 'BEGIN {print (g > b * 1.10)}')" = 1 ]; then
      echo "REGRESSION $name: $got instructions a block, baseline $base (+10 % allowed)"; fail=1
    else echo "ok $name: $got (baseline $base)"; fi
  done < "$SRC/tools/cost/baseline.txt"
  exit $fail
fi
