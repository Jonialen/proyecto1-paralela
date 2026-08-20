#!/usr/bin/env bash
# Sequential baseline sweep. Each point runs an untimed warmup first, so the
# initial chunk streaming is not averaged into the steady-state figure.
set -u

BIN=${BIN:-./cubeview}
W=${W:-960}
H=${H:-720}

run() { # players view ssaa frames
  local n=$1 v=$2 s=$3 f=$4
  local out
  out=$($BIN -n "$n" --view "$v" --ssaa "$s" --width $W --height $H \
             --warmup 4 --bench "$f" 2>/dev/null)
  printf "%-4s %-6s %-6s %-10s %-8s %-11s %-8s\n" \
    "$n" "$v" "$s" \
    "$(echo "$out" | rg -o 'Triangles:\s+(\d+)'    -r '$1')" \
    "$(echo "$out" | rg -o 'Chunks:\s+(\d+)'       -r '$1')" \
    "$(echo "$out" | rg -o 'Per frame:\s+([\d.]+)' -r '$1')" \
    "$(echo "$out" | rg -o '\(([\d.]+) FPS\)'      -r '$1')"
}

printf "%-4s %-6s %-6s %-10s %-8s %-11s %-8s\n" N view ssaa tris chunks frame_ms FPS
echo "-- explorers (N) --"
for n in 1 2 4 8 16; do run "$n" 96 1 12; done
echo "-- render distance --"
for v in 48 96 160; do run 4 "$v" 1 12; done
echo "-- supersampling --"
for s in 1 2 4; do run 4 96 "$s" 8; done
