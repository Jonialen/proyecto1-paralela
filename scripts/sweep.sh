#!/usr/bin/env bash
# Sequential baseline sweep: geometry load (--chunks) x pixel load (--ssaa).
set -u
printf "%-7s %-6s %-10s %-12s %-12s %-12s %-9s\n" chunks ssaa tris frame_ms geom_ms raster_ms raster%
for c in 1 2 4 8; do
  for s in 1 2 4; do
    frames=$(( 120 / (s*s) )); [ $frames -lt 8 ] && frames=8
    out=$(./cubeview --scene chunk --chunks $c --width 1280 --height 720 \
                     --ssaa $s --bench $frames 2>/dev/null)
    tris=$(echo   "$out" | rg -o 'Triangles:\s+(\d+)'        -r '$1')
    frame=$(echo  "$out" | rg -o 'Per frame:\s+([\d.]+)'     -r '$1')
    geom=$(echo   "$out" | rg -o 'geometry:\s+([\d.]+)'      -r '$1')
    rast=$(echo   "$out" | rg -o 'raster:\s+([\d.]+)'        -r '$1')
    pct=$(echo    "$out" | rg -o 'raster:.*\(([\d.]+)%\)'    -r '$1')
    printf "%-7s %-6s %-10s %-12s %-12s %-12s %-9s\n" "$c" "$s" "$tris" "$frame" "$geom" "$rast" "$pct"
  done
done
