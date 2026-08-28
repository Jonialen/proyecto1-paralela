# Working on this project

Sequential CPU voxel screensaver for Proyecto #1, Computación Paralela y
Distribuida (UVG). `N` first-person explorers fly over an endless procedurally
generated world in split-screen, rendered entirely by our own software
rasterizer. SDL2 only opens the window and blits the finished framebuffer.

`CLAUDE.md` is a symlink to this file: the two conventions name the same
document so there is only one set of instructions to keep current.

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

Header dependencies are tracked with `-MMD -MP`. If you ever see an impossible
crash right after editing a header, check that first: object files compiled
against a stale struct layout corrupt memory in ways that look like a bug in
whatever you just wrote. This happened here, and touching `world.h` used to
rebuild exactly zero files.

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
- **A dumped frame contains scene content only.** Nothing that varies with the
  build, the clock or the configuration may reach those pixels. The frame rate
  shows as dashes and the build label as `REFERENCE` for this reason. This has
  already broken the comparison twice; when a diff fails, check *which rows*
  differ before suspecting the renderer.
- **Keep procedural generation seeded and pure.** Terrain and textures are
  functions of coordinates and a seed. No global RNG state, no time input.

If the check fails, find *which bytes* differ before auditing the algorithm. Last
time it was a HUD text line, not the renderer.

## Measurement discipline

- **Measure, do not estimate.** An analytical model predicted 68 % vs 83 % wrong
  once already. Use `--bench`.
- **Benchmark on an idle machine.** Frame times here are very sensitive to system
  load. A busy desktop shifted every stage by the same 1.6x factor and looked
  exactly like a regression in the change under test. If all stages move
  together, suspect the machine before the code.
- **Isolate one variable per A/B.** A fast-path comparison here reported a
  mismatch that turned out to be a second, unrelated parameter changed between
  the two captures.
- **Tune terrain with `--survey`, not with impressions.** It prints a height
  histogram and a biome census. Every terrain parameter in this project was set
  from that output.
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
- **Light is a per-vertex attribute.** Ambient occlusion varies across a face, so
  the rasterizer interpolates it perspective-correct like the texture
  coordinates. Do not collapse it back to a per-triangle constant.
- **`neighbourhood_mask()` has a fast path for interior blocks.** Any change to
  either path must be verified byte-for-byte against the other, not just
  eyeballed.
- **Texture packs are never committed.** `*.vxtx`, `*.zip`, `textures/` and
  `packs/` are gitignored. The repository is made public for grading and
  third-party art must not be redistributed. Packs are converted offline by
  `scripts/make_texture_atlas.py`; the program has no PNG decoder on purpose.
- **Procedural textures are the fallback and must keep working.** A missing or
  malformed pack is reported and ignored, never fatal.

## Optimizations that were tried and did not pay

Recorded so they are not attempted again. Both were measured, not guessed.

- **Reordering the early-out in `raster_triangle()`.** Testing band overlap
  before computing the signed area looks like it should skip work on the ~97% of
  calls that reject. Neutral: reading `min_x` already pulls in the cache line the
  vertices share, so the arithmetic saved was free. The ordering was kept because
  it is the honest one, not because it helped.
- **Binning triangles into bands** so each band walks only the triangles it
  touches. Rasterization did drop 9-12%, but the counting sort cost 13-23% more
  in the geometry stage and the frame got *worse* at four and eight explorers.
  The redundant scan is cheap because it walks the triangle array linearly and
  stays in cache after the first band; the sort does integer divisions and
  scattered writes, which is real work. Reverted.

- **SIMD lanes in the rasterizer inner loop.** Coverage and the depth test are
  data-dependent branches no auto-vectorizer will cross, so they were resolved
  into a mask and the interpolation run unconditionally over eight-pixel lanes.
  Rasterization got **26% slower** at one explorer. Divergence is why: block
  faces are small, so most lanes hold two or three covered pixels out of eight,
  and every uncovered lane still pays three barycentric multiplies and a depth
  interpolation. With the edge functions already stepped, rejecting a pixel
  costs three adds and a compare -- there was nothing left to save. Reverted.

The lesson all three share: a rejection that looks expensive by instruction
count can be nearly free, while the machinery built to avoid it is not. Measure
the rejection before optimizing it away.

Note also that these were tried *after* the edge functions were stepped
incrementally, which is the one rasterizer optimization that did pay: 26% off
rasterization and 19% off the frame. Cheapening the common path beat every
attempt to skip it.

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
