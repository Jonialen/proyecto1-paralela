#!/usr/bin/env bash
# Sequential baseline sweep. Each point runs untimed warmup frames first, so the
# initial chunk streaming is not averaged into the steady-state figure.
#
# Run it on an idle machine: frame times here are sensitive to system load, and
# a busy desktop shifts every stage by the same factor.
set -u

BIN=${BIN:-./cubeview}
W=${W:-960}
H=${H:-720}
FRAMES=${FRAMES:-8}

run() { # players view ssaa
  local n=$1 v=$2 s=$3 out
  out=$($BIN -n "$n" --view "$v" --ssaa "$s" --width $W --height $H \
             --warmup 4 --bench "$FRAMES" 2>/dev/null)
  printf "%-4s %-6s %-6s %-9s %-8s %-10s %-7s %-9s %-9s %-7s\n" \
    "$n" "$v" "$s" \
    "$(echo "$out" | rg -o 'Triangles:\s+(\d+)'    -r '$1')" \
    "$(echo "$out" | rg -o 'Chunks:\s+(\d+)'       -r '$1')" \
    "$(echo "$out" | rg -o 'Per frame:\s+([\d.]+)' -r '$1')" \
    "$(echo "$out" | rg -o '\(([\d.]+) FPS\)'      -r '$1')" \
    "$(echo "$out" | rg -o 'geometry:\s+([\d.]+)'  -r '$1')" \
    "$(echo "$out" | rg -o 'raster:\s+([\d.]+)'    -r '$1')" \
    "$(echo "$out" | rg -o 'sky:\s+([\d.]+)'       -r '$1')"
}

printf "%-4s %-6s %-6s %-9s %-8s %-10s %-7s %-9s %-9s %-7s\n" \
  N view ssaa tris chunks frame_ms FPS geom_ms rast_ms sky_ms
echo "-- explorers --"
for n in 1 2 4 8 16; do run "$n" 96 1; done
echo "-- render distance --"
for v in 48 96 160; do run 4 "$v" 1; done
echo "-- supersampling --"
for s in 1 2 4; do run 4 96 "$s"; done
