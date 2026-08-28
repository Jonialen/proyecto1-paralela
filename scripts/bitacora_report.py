#!/usr/bin/env python3
"""Aggregate the raw runs from bitacora.sh into the report tables.

Kept separate from the collection script so the tables can be regenerated, or
re-cut differently, without measuring again -- measuring is the slow part and
the numbers move if the machine is not in the same state.

Standard deviation is printed beside every mean on purpose. A speedup quoted
without it cannot be told apart from noise, and on this project system load has
moved the same measurement by more than half.
"""

import csv
import math
import sys
from collections import defaultdict


def stats(values):
    """Mean, sample standard deviation, and the deviation as a percentage."""
    n = len(values)
    if n == 0:
        return 0.0, 0.0, 0.0
    mean = sum(values) / n
    if n < 2:
        return mean, 0.0, 0.0
    var = sum((v - mean) ** 2 for v in values) / (n - 1)
    sd = math.sqrt(var)
    return mean, sd, (100.0 * sd / mean if mean else 0.0)


def load(path):
    runs = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if not row["frame_ms"]:
                continue
            key = (row["test"], row["build"], int(row["n"]), int(row["threads"]))
            runs[key].append(float(row["frame_ms"]))
    return runs


def line(width=94):
    print("-" * width)


def report_speedup(runs, tests, title, label, keys):
    print(f"\n{title}")
    line()
    print(f"{label:<10} {'sequential (ms)':<22} {'parallel (ms)':<22} "
          f"{'speedup':<9} {'efficiency':<11} FPS")
    line()

    for key, threads in keys:
        seq = [v for (t, b, n, th), vs in runs.items()
               if t in tests and b == "seq" and n == key for v in vs]
        par = [v for (t, b, n, th), vs in runs.items()
               if t in tests and b == "par" and n == key and th == threads for v in vs]
        if not seq or not par:
            continue

        sm, ss, sp = stats(seq)
        pm, ps, pp = stats(par)
        speedup = sm / pm if pm else 0.0
        efficiency = f"{100 * speedup / threads:.1f}%"
        print(f"{key:<10} {sm:8.2f} +/- {ss:5.2f} ({sp:4.1f}%)  "
              f"{pm:8.2f} +/- {ps:5.2f} ({pp:4.1f}%)  "
              f"{speedup:<9.2f} {efficiency:<11} {1000 / pm:.1f}")
    line()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "bitacora.csv"
    runs = load(path)
    if not runs:
        sys.exit(f"no usable rows in {path}")

    reps = max(len(v) for v in runs.values())
    print(f"Test log from {path} -- {reps} repetitions per point")
    print("Speedup is sequential mean over parallel mean. Efficiency is speedup "
          "over thread count.")

    threads_used = sorted({th for (_, b, _, th) in runs if b == "par"})
    top = threads_used[-1] if threads_used else 1

    ns = sorted({n for (t, _, n, _) in runs if t == "explorers"})
    if ns:
        report_speedup(runs, {"explorers"}, "TEST 1 - explorers (--view 96 --ssaa 1)",
                       "N", [(n, top) for n in ns])

    ths = sorted({th for (t, b, _, th) in runs if t == "threads" and b == "par"})
    if ths:
        print("\nTEST 2 - thread scaling (n=8)")
        line()
        print(f"{'threads':<10} {'parallel (ms)':<22} {'speedup':<9} "
              f"{'efficiency':<11} FPS")
        line()
        seq = [v for (t, b, _, _), vs in runs.items()
               if t == "threads" and b == "seq" for v in vs]
        sm, _, _ = stats(seq)
        for th in ths:
            par = [v for (t, b, _, x), vs in runs.items()
                   if t == "threads" and b == "par" and x == th for v in vs]
            pm, ps, pp = stats(par)
            speedup = sm / pm if pm else 0.0
            efficiency = f"{100 * speedup / th:.1f}%"
            print(f"{th:<10} {pm:8.2f} +/- {ps:5.2f} ({pp:4.1f}%)  "
                  f"{speedup:<9.2f} {efficiency:<11} {1000 / pm:.1f}")
        line()

    scheds = sorted({t for (t, _, _, _) in runs if t.startswith("schedule-")})
    if scheds:
        print("\nTEST 3 - scheduling policy (n=4)")
        line()
        print(f"{'policy':<10} {'parallel (ms)':<22} vs the other")
        line()
        means = {}
        for t in scheds:
            par = [v for (x, b, _, _), vs in runs.items()
                   if x == t and b == "par" for v in vs]
            means[t], sd, pct = stats(par)[0], stats(par)[1], stats(par)[2]
            print(f"{t.replace('schedule-',''):<10} {means[t]:8.2f} +/- {sd:5.2f} ({pct:4.1f}%)")
        if len(means) == 2:
            a, b = list(means)
            diff = 100.0 * (means[a] - means[b]) / means[a]
            faster = b if diff > 0 else a
            print(f"\n{faster.replace('schedule-','')} is faster by {abs(diff):.1f}%")
        line()

    ssaa = sorted({t for (t, _, _, _) in runs if t.startswith("ssaa-")})
    if ssaa:
        print("\nTEST 4 - pixel workload (n=4)")
        line()
        print(f"{'ssaa':<10} {'sequential (ms)':<22} {'parallel (ms)':<22} "
              f"{'speedup':<9} efficiency")
        line()
        for t in ssaa:
            seq = [v for (x, b, _, _), vs in runs.items()
                   if x == t and b == "seq" for v in vs]
            par = [v for (x, b, _, th), vs in runs.items()
                   if x == t and b == "par" and th == top for v in vs]
            if not seq or not par:
                continue
            sm, ss, sp = stats(seq)
            pm, ps, pp = stats(par)
            speedup = sm / pm if pm else 0.0
            print(f"{t.replace('ssaa-',''):<10} {sm:8.2f} +/- {ss:5.2f} ({sp:4.1f}%)  "
                  f"{pm:8.2f} +/- {ps:5.2f} ({pp:4.1f}%)  "
                  f"{speedup:<9.2f} {100 * speedup / top:.1f}%")
        line()

    worst = max((pct for vs in runs.values() for pct in [stats(vs)[2]]), default=0.0)
    print(f"\nLargest relative standard deviation across all points: {worst:.1f}%")
    if worst > 5.0:
        print("Above 5% the machine was not quiet enough for the smaller "
              "differences in these tables to be meaningful.")


if __name__ == "__main__":
    main()
