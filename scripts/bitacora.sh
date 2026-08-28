#!/usr/bin/env bash
#
# Test log for the project report. Runs every configuration REPS times on both
# builds, writes one CSV row per individual run, and prints the aggregated
# tables.
#
# The rubric asks for at least ten measurements per test. One run cannot tell a
# real difference from noise: frame times here move by more than half when the
# machine is busy, so the standard deviation is what says whether a number can
# be trusted.
#
#   scripts/bitacora.sh [output.csv]
#
#   REPS=10      repetitions per configuration
#   FRAMES=10    timed frames per repetition
#   W=960 H=720  resolution
#
set -u

CSV=${1:-bitacora.csv}
REPS=${REPS:-10}
FRAMES=${FRAMES:-10}
W=${W:-960}
H=${H:-720}
WARMUP=4

command -v ./cubeview >/dev/null 2>&1 || { echo "run make first"; exit 1; }
[ -x ./cubeview ] && [ -x ./cubeview-seq ] || { echo "run make first: both binaries are needed"; exit 1; }

load=$(uptime | sed 's/.*average: //' | cut -d, -f1)
cores=$(nproc)
echo "Machine: $cores cores, load average $load"
awk -v l="$load" -v c="$cores" 'BEGIN { if (l+0 > c/4) print "WARNING: the machine is busy. Close what you can: a loaded machine has shifted these numbers by 60% before now." }'
echo "Plan: $REPS repetitions x $FRAMES frames per point"
echo

echo "build,test,n,view,ssaa,threads,schedule,rep,frame_ms,fps,geometry_ms,raster_ms,sky_ms,triangles,chunks" > "$CSV"

# one repetition -> one CSV row
measure() { # build test n view ssaa threads schedule rep
  local build=$1 test=$2 n=$3 view=$4 ssaa=$5 threads=$6 sched=$7 rep=$8
  local bin out
  [ "$build" = "seq" ] && bin=./cubeview-seq || bin=./cubeview

  out=$($bin -n "$n" --view "$view" --ssaa "$ssaa" --width $W --height $H \
             --threads "$threads" --schedule "$sched" \
             --warmup $WARMUP --bench "$FRAMES" 2>/dev/null)

  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$build" "$test" "$n" "$view" "$ssaa" "$threads" "$sched" "$rep" \
    "$(echo "$out" | rg -o 'Per frame:\s+([\d.]+)' -r '$1')" \
    "$(echo "$out" | rg -o '\(([\d.]+) FPS\)'      -r '$1')" \
    "$(echo "$out" | rg -o 'geometry:\s+([\d.]+)'  -r '$1')" \
    "$(echo "$out" | rg -o 'raster:\s+([\d.]+)'    -r '$1')" \
    "$(echo "$out" | rg -o 'sky:\s+([\d.]+)'       -r '$1')" \
    "$(echo "$out" | rg -o 'Triangles:\s+(\d+)'    -r '$1')" \
    "$(echo "$out" | rg -o 'Chunks:\s+(\d+)'       -r '$1')" >> "$CSV"
}

sweep() { # label: run every rep of one point on both builds
  local test=$1 n=$2 view=$3 ssaa=$4 threads=$5 sched=$6
  printf '  %-28s ' "$test n=$n th=$threads"
  for rep in $(seq 1 "$REPS"); do
    measure seq "$test" "$n" "$view" "$ssaa" 1 "$sched" "$rep"
    measure par "$test" "$n" "$view" "$ssaa" "$threads" "$sched" "$rep"
    printf '.'
  done
  printf ' done\n'
}

CORES=$(nproc)

echo "Test 1 - explorers"
for n in 1 2 4 8 16; do sweep explorers "$n" 96 1 "$CORES" dynamic; done

echo "Test 2 - thread scaling (n=8)"
for th in 1 2 4 8; do sweep threads 8 96 1 "$th" dynamic; done

echo "Test 3 - scheduling policy (n=4)"
for sch in static dynamic; do sweep "schedule-$sch" 4 96 1 "$CORES" "$sch"; done

echo "Test 4 - pixel workload (n=4)"
for s in 1 2 4; do sweep "ssaa-$s" 4 96 "$s" "$CORES" dynamic; done

echo
echo "Raw runs written to $CSV"
echo
python3 scripts/bitacora_report.py "$CSV"
