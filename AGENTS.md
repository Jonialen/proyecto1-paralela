# Working on this project

Sequential CPU voxel screensaver for Proyecto #1, Computación Paralela y
Distribuida (UVG). `N` first-person explorers fly over an endless procedurally
generated world in split-screen, rendered entirely by our own software
rasterizer. SDL2 only opens the window and blits the finished framebuffer.

Read `README.md` for what the program does, `docs/matematica.md` for the maths
behind each pipeline stage, and `docs/decisiones.md` for why the design is the
way it is. **Check `docs/decisiones.md` before changing anything structural** —
most surprising choices are deliberate and the reason is recorded there.

## Language policy

| Where | Language |
|---|---|
| Code, identifiers, comments, commit messages | English |
| `README.md`, `AGENTS.md` | English |
| `docs/` | Spanish (neutral register) |

`docs/` is Spanish because the team and the assignment brief are. Do not mix
registers inside a file, and never put regional slang in a technical artifact.

## Build and verify

```sh
make                 # must finish with zero warnings
./cubeview -n 4
./scripts/sweep.sh   # sequential baseline tables
```

The build **must stay clean under `-Wall -Wextra`**. Warnings here have caught
real bugs, including signed integer overflow in the texture hash. Do not silence
one without understanding it.

There is no unit test framework. Verification is:

1. `make` with no warnings.
2. The determinism check below.
3. `--bench` numbers that are not absurd.
4. `--dump` plus looking at the image.

## The determinism contract

**This is the most important invariant in the repository.** Rendering is
deterministic: two runs with identical arguments produce byte-identical output.

```sh
./cubeview -n 4 --view 96 --ssaa 2 --dump reference.ppm
# make your change
./cubeview -n 4 --view 96 --ssaa 2 --dump candidate.ppm
cmp reference.ppm candidate.ppm
```

It is the only objective correctness test for the parallel version: if a single
byte differs, there is a race. Protecting it constrains what you may do:

- **Triangle emission order must not depend on load history.** `world_emit_view()`
  visits chunks in a sorted box scan, never in hash order.
- **When splitting work across threads, fix the merge order.** Per-thread
  `TriangleBuffer`s must be concatenated in a deterministic sequence.
- **Never draw wall-clock values into a dumped frame.** Headless dumps show
  `FPS ---` on purpose.
- **Keep procedural generation seeded and pure.** Terrain and textures are
  functions of coordinates and a seed. No global RNG state, no time input.

If the check fails, find *which bytes* differ before auditing the algorithm. Last
time it was a HUD text line, not the renderer.

## Measurement discipline

- **Measure, do not estimate.** An analytical model predicted 68 % vs 83 % wrong
  once already. Use `--bench`.
- **Always warm up.** The first frames stream thousands of chunks; `--warmup`
  keeps that out of the steady-state average.
- **Never use per-frame smoothing for a rate.** It is frame-rate dependent and
  therefore not comparable between configurations. Average over a time window.
- **Report the stage split**, not just the frame time. Which stage dominates
  depends on the operating point — state the operating point with every number.

## Hard requirements from the rubric

Do not regress these.

- A CLI parameter `N` for the number of rendered elements (`-n, --players`).
- Frame rate drawn **on screen**, not in the window title, and the target floor
  is 30 FPS.
- Minimum canvas 640x480; the CLI rejects anything smaller.
- Pseudo-random colours (procedural textures).
- Constant motion with trigonometry in the calculation (the Lissajous paths).
- Defensive programming: every argument validated, no silent clamping, non-zero
  exit on bad input.
- No hard-coded values that should be arguments.
- Source only in the repository — no executables, no object files.

## Architecture invariants

Breaking these is possible but must be deliberate and recorded in
`docs/decisiones.md`.

- **`ViewTask` is the unit of parallel work.** Views share nothing: disjoint
  framebuffer rectangles, private triangle buffers, read-only world access during
  rendering. Keep it that way.
- **`ViewTask.world` stays a pointer.** Pointing views at different worlds is the
  entire change needed for independent sub-worlds.
- **The geometry stage draws nothing.** It only fills a `TriangleBuffer`. The
  raster stage only consumes one.
- **`raster_triangle()` keeps its clip rectangle.** It is what makes tile-parallel
  rasterization possible without touching another worker's depth slots.
- **Streaming is its own phase.** It mutates the shared chunk map, so it cannot be
  folded into the render loop. Eviction runs only after every explorer has
  claimed what it needs.
- **Chunk coordinates use `floor_div`/`mod_floor`.** C's `/` truncates towards
  zero and tears the terrain along the negative axes.
- **Noise is sampled in world coordinates.** Chunk-local sampling makes every
  chunk a disconnected island.

## Parallelization notes

Not implemented yet — the sequential baseline comes first, by decision.

- The loop over `ViewTask`s in `render_frame()` is the primary decomposition and
  is race-free by construction.
- Per-view cost is deliberately uneven, so `schedule(static)` versus
  `schedule(dynamic)` is worth measuring.
- If a single view must be split: rasterize by **screen tile, never by triangle**
  (two triangles can cover one pixel and the depth test is a read-modify-write of
  a shared slot). Split geometry **by chunk with per-worker buffers**, since
  `tribuf_push()` races on `count`.
- Both sequential and parallel versions must remain buildable and comparable.

## Commits

- Conventional Commits. **No AI attribution and no `Co-Authored-By` trailers.**
- One work unit per commit: a deliverable behaviour, fix or docs unit — not
  "add files", not "update code".
- **Every commit must compile.** Group changes that break a signature with the
  callers they break.
- Documentation ships with the change it explains.
- The message explains the outcome and the reason, not the file list. If a bug
  is fixed, say what the wrong behaviour was and why it happened.
- Commit often. The rubric grades a history that reflects continuous work.

## Style

- Match the surrounding code: same brace style, naming and comment density.
- **Comments explain why, not what.** `i++ /* increment i */` is noise;
  `/* heading from horizontal travel only: the vertical bob dominates near the
  sine extremes */` is the reason someone will not undo your fix.
- Keep functions doing one thing. The rasterizer inner loop is the exception and
  is deliberately flat for speed.
- No new dependencies. Everything is C11, libm and SDL2. The bitmap font is
  embedded rather than loaded for this reason.
- Free what you allocate; every failed allocation path already cleans up.
