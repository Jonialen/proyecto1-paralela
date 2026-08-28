# Voxel Screensaver — Sequential Version

Proyecto #1, Computación Paralela y Distribuida (UVG).

`N` first-person explorers fly over an endless, procedurally generated voxel
world in split-screen. Terrain is streamed in around each explorer as it
advances. Everything is rendered on the CPU by our own software rasterizer;
SDL2 is used only to open a window and blit the finished framebuffer.

This is the **sequential** version — the baseline the parallel version is
measured against.

## Licensing at a glance

This repository ships **source only**: no executables, no object files, no
third-party artwork. Textures are generated procedurally at startup and the HUD
font is embedded in source, so nothing but SDL2 is needed to build and run it.

Texture packs are optional, user-supplied and never committed. See
[`THIRD_PARTY.md`](THIRD_PARTY.md).

## Documentation

| File | Contents |
|---|---|
| `docs/matematica.md` | The maths behind each pipeline stage and why it is that way |
| `docs/decisiones.md` | Design decision record: what, why, alternatives, consequences |
| `AGENTS.md` | Working practices, invariants and the determinism contract |
| `THIRD_PARTY.md` | Dependencies, and why no third-party asset ships here |

## Build and run

```sh
make            # requires SDL2 development headers
./cubeview -n 4
```

| Key | Action |
|---|---|
| `H` or `F1` | Hide the HUD and just watch |
| `F11` | Fullscreen |
| `Esc` or `Q` | Quit |

The window is resizable. Framebuffer, resolve buffer, streaming texture and
viewport layout are rebuilt on resize, and each pane recomputes its own aspect
ratio so views are never stretched. New surfaces are allocated before the old
ones are released, so a resize that runs out of memory keeps the previous size
instead of tearing down buffers it cannot replace.

## Usage

```
-n, --players N     explorers rendered in split-screen, 1-16   (default 1)
    --view N        render distance per explorer, world units  (default 96)
    --roam N        radius of the flight path, world units     (default 320)
    --max-chunks N  ceiling on resident chunks                 (default 6000)
    --textures P    texture atlas built from a pack (see below)
    --daylen N      seconds for a full day/night cycle          (default 180)
    --seed N        terrain seed                               (default 1337)
    --width N       window width, minimum 640                  (default 960)
    --height N      window height, minimum 480                 (default 720)
    --ssaa N        supersampling factor, 1-8                  (default 1)
    --scene NAME    'chunk' or 'block'                         (default chunk)
    --block N       block index for the block scene
    --bench N       render N frames headless and report timings
    --warmup N      untimed frames before the benchmark        (default 3)
    --dump PATH     render one frame headless into a binary PPM
    --survey N      sample the generator over an NxN area and report
    --help          show this message
```

Every argument is validated. Non-numeric values, out-of-range values and
missing values are rejected with an explicit message and a non-zero exit code.
Nothing is hard-coded.

```
$ ./cubeview -n 99
Error: -n must be between 1 and 16, got 99
```

## What is on screen

The HUD is drawn into the resolved image with an embedded 5x7 bitmap font
(`src/overlay.c`), so it costs the same at any `--ssaa` setting and needs no
font file.

- **FPS**, drawn **red below 30** and green at or above it, so a frame rate
  under the project's floor is visible at a glance.
- Triangle total, view count, supersampling factor, build.
- Resident chunks, plus how many were generated and dropped this frame.
- Per-view label with that explorer's triangle count.

Frame rate is averaged over a fixed 0.35 s **window of time**, not with a
per-frame smoothing factor. A per-frame factor is frame-rate dependent: at 1 FPS
a 0.9 weight remembers several seconds, so one bad sample survives for ages and
readings are not comparable between configurations. Until the first window
closes the HUD shows dashes rather than inventing a number.

## How the explorers move

Each explorer flies a closed Lissajous path — pure trigonometry, and closed so
it never escapes its own streaming region:

```
x(t) = cx + radius_x * sin(rate_x * t * speed + phase_x)
z(t) = cz + radius_z * sin(rate_z * t * speed + phase_z)
y(t) = height + bob_amplitude * sin(bob_rate * t * speed + phase_y)
```

Different `rate_x` and `rate_z` turn the circle into a figure-eight, so the
explorer sweeps new ground instead of orbiting one ring. Explorers are spread
around a ring so their paths do not overlap: with overlapping paths the terrain
would be generated once and shared, hiding the real cost of streaming for N
explorers.

The heading comes from the **horizontal** travel direction only. Using the full
path derivative is wrong: near the extremes of the vertical sine the bob
velocity dominates the horizontal one and the camera ends up staring at the sky.

## Streaming terrain

The world is infinite on X and Z. Chunks live in an open-addressed map keyed by
`(cx, cz)`, are generated the first time an explorer comes within its generation
radius, and are dropped once no explorer has needed them for a while.

**Terrain is generated out to 2.5x each explorer's render distance**
(`CAMERA_STREAM_FACTOR`). The margin is not decoration:

1. Face culling at the edge of the rendered region needs the neighbouring chunk
   to already exist. Without it, the boundary renders as a wall of faces.
2. A fast explorer would otherwise fly into chunks that do not exist yet.

Each frame runs in this order, and the order matters:

```
world_begin_frame()
  for each explorer: world_stream_around(eye, generate_radius)   <- claims chunks
world_end_frame()                                                <- evicts
  for each explorer: render_view()                               <- world read-only
```

Eviction runs only after **every** explorer has claimed what it needs. Dropping
after each explorer would throw away chunks the next one still requires.
Eviction also has a grace period, because a chunk right at the boundary would
otherwise be generated and dropped on alternating frames as an explorer skims
past it. The `--max-chunks` ceiling never evicts a chunk needed on the current
frame, so with many explorers the resident set can legitimately exceed it.

Chunk lookup uses **floor division**, not plain integer division. `/` truncates
towards zero, which puts world `x = -1` in chunk 0 instead of chunk -1 and tears
the terrain along both negative axes.

Face masking resolves the four horizontal neighbour chunks once per chunk rather
than hashing per block per side. A missing neighbour counts as solid.

### Terrain generation

`terrain_height()` is the only place the noise is sampled: four octaves of value
noise (`src/noise.c`), lattice hash plus a Hermite smoothstep. **The noise is
evaluated in world coordinates**, which is what lets chunks be generated
independently, in any order, and still line up seamlessly.

On top of the base field, **domain warping** displaces the sample point with a
second noise field, which bends contours into ridges and inlets that read as
eroded rather than as round fBm blobs. **Ridged noise** (`1 - |2n-1|`, squared)
supplies crests instead of domes, gated by a separate low-frequency mask so
ranges are localized.

Seven biomes come from two low-frequency climate fields, temperature and
humidity, crossed with the height. **Temperature falls with altitude**: sampled
independently of the relief, the climate put snow at sea level next to a desert.

All knobs live in `TerrainParams`.

### Tuning terrain with `--survey`

```sh
./cubeview --survey 2048
```

Samples the generator over an NxN block area and prints a height histogram and a
biome census. "The terrain looks flat" is not actionable; a histogram showing
78% of the world inside an 11-block band is. Every terrain parameter in this
project was set from that output rather than from impressions.

## Texture packs

Textures are procedural by default -- 32x32, generated at startup -- so the
program runs with no assets at all. The generators use seamless tile noise: the
lattice indices wrap, so neighbouring blocks show no seam. Unwrapped noise draws
a visible grid across the terrain.

A pack can be used instead:
```sh
scripts/make_texture_atlas.py /path/to/pack build/pack.vxtx --size 32
./cubeview -n 4 --textures build/pack.vxtx
```

**Packs are converted offline, not decoded at runtime.** The renderer has no PNG
decoder on purpose: adding one would mean either a new build dependency or
several thousand lines of third-party code in a project whose brief asks for our
own. The script does the decoding with ImageMagick and writes a raw block of
pixels the program reads with one `fread`.

Four details the converter handles, without which a pack looks wrong:

- **Biome-tinted textures ship desaturated.** Grass, leaves *and water* are grey
  in the files; the game tints them per biome at runtime. Loaded as-is, water
  comes out grey. The script applies a fixed tint to each.
- **Animation strips.** Water and fire are tall images holding N square frames;
  only the first is taken. Grove's `water_still.png` is 64x4096, 64 frames.
- **The grass side is an overlay.** In modern packs `grass_block_side.png` is
  plain dirt, and the green fringe is a separate greyscale image the game tints
  and composites on top. Without compositing it, grass blocks have bare dirt
  sides.
- **Variant folders.** CTM packs replace `stone.png` with `stone/1.png` through
  `stone/8.png`. Candidates may name a directory, and a bare candidate must sit
  directly in `block/` -- otherwise `stone.png` resolves to `block/button/stone.png`,
  the stone *button*.

Texture size is per-texture rather than a compile-time constant, so 16, 32, 64
and 128 packs all work. Every pack uses a power-of-two edge, which keeps
wrapping a single AND in the rasterizer's innermost loop.

A pack that is missing, malformed or the wrong size is reported and **ignored**:
the procedural set stays in place and the program keeps running.

> **Packs are never committed, and neither is the atlas built from one.** The
> `.vxtx` file is a derivative work containing the pack's pixels and carries the
> pack's licence exactly as the original files do. `*.zip`, `*.vxtx`,
> `docs/*.zip`, `textures/` and `packs/` are all gitignored.
>
> Most resource packs forbid redistribution. Read the licence of any pack before
> using it, and see [`THIRD_PARTY.md`](THIRD_PARTY.md) for what this project does
> and does not include.

## Lighting

Faces are shaded with a flat Lambert term plus **per-vertex ambient occlusion**.
Each corner of a face is darkened by its two edge neighbours and the diagonal,
read from a 27-bit map of the block's 3x3x3 neighbourhood.

Without it, every face of a given orientation renders at exactly the same
brightness whatever surrounds it, and voxel terrain looks like flat plastic. It
is the single largest visual difference, and it costs about 20% of the frame.

Light is therefore a per-vertex attribute, interpolated perspective-correct by
the rasterizer like the texture coordinates.

## Sky and the day/night cycle

`src/sky.c` derives everything from one phase value: 0 is midnight, 0.25
sunrise, 0.5 noon, 0.75 sunset. The same state drives the background **and** the
terrain shading, so the world darkens with the sky instead of staying lit under
a black one. At night the sun is replaced by a dim moon from the opposite
direction and the ambient floor drops, which is why the light is a `Light`
struct (direction, ambient, intensity) rather than a bare vector.

- **Gradient** blended between three palettes. Twilight is not an interpolation
  of day and night: it has its own warm horizon, which is what makes a sunrise
  read as a sunrise.
- **Clouds** live on a horizontal plane above the camera. Intersecting the view
  ray with that plane is what gives them perspective — they spread overhead and
  compress towards the horizon instead of being pasted flat on the sky.
- **Stars** are hashed from a quantized view direction, so they cost no memory
  and stay fixed as the camera turns.
- **Sun and moon** are a disc plus a `dot^32` glow.

The sky is drawn **after** the terrain and writes only where the depth buffer is
still at the far plane. Sky pixels hidden by terrain are never computed, which
on a typical frame is most of the pane.

## Architecture

```
   STREAMING            GEOMETRY STAGE                    RASTER STAGE
  world_stream    ->  world_emit_view()   ->  TriangleBuffer  ->  raster_flush()
  world_evict         - neighbour face culling                    - z-buffer test
                      - MVP transform                             - perspective-correct UVs
                      - near-plane clipping                       - texture + shading
                      - perspective divide
                      - backface culling
                      - viewport-clamped bbox
```

### ViewTask — the unit of parallel work

```c
typedef struct {
    Camera camera;
    const World *world;
    Viewport viewport;
    TriangleBuffer triangles;
    size_t triangle_count;
} ViewTask;
```

Two views share **nothing**: disjoint framebuffer rectangles, private triangle
buffers, read-only access to the world during rendering. The whole decomposition
is one loop in `render_frame()`:

```c
for (int i = 0; i < scene->view_count; i++)
    render_view(fb, &scene->views[i], scene, t, timings);
```

No locks, no atomics, no reduction. `world` is a pointer rather than a shared
global on purpose: pointing views at different worlds is all that separates
split-screen from independent sub-worlds.

`to_screen()` maps NDC into the **viewport rectangle** rather than the whole
framebuffer, and triangle bounding boxes are clamped to the viewport, so one
camera's geometry can never bleed into a neighbouring pane.

## The workload knobs

| Flag | Scales | Dominant stage |
|---|---|---|
| `-n, --players N` | explorers, one world traversal each | geometry, ~linear in N |
| `--view N` | terrain in range, as the disk area | geometry, ~N² |
| `--ssaa N` | per-pixel raster work, as N² | raster, ~N² |

## Sequential baseline

960x720, gcc `-O2`, 4 untimed warmup frames per point. Reproduce with
`scripts/sweep.sh`.

**Run benchmarks on an idle machine.** Frame times here are sensitive to system
load: a busy desktop shifts every stage by the same factor, which looks exactly
like a regression in whatever you changed last.

### Explorers (`--view 96 --ssaa 1`)

| N | triangles | chunks | ms/frame | FPS | geometry | raster | sky |
|---|---|---|---|---|---|---|---|
| 1  |  16700 |  713 |  49.1 | 20.4 |  13.5 | 14.7 | 20.1 |
| 2  |  28604 | 1833 |  79.2 | 12.6 |  43.8 | 16.0 | 18.4 |
| 4  | 138501 | 2887 | 157.4 |  6.4 | 109.6 | 28.8 | 17.9 |
| 8  | 328678 | 4806 | 299.1 |  3.3 | 240.8 | 40.6 | 16.2 |
| 16 | 699218 | 5505 | 570.2 |  1.8 | 489.3 | 60.2 | 18.6 |

### Render distance (`-n 4 --ssaa 1`)

| view | triangles | chunks | ms/frame | FPS |
|---|---|---|---|---|
| 48  |  36934 |  859 |  65.7 | 15.2 |
| 96  | 138501 | 2887 | 157.2 |  6.4 |
| 160 | 372092 | 7268 | 360.4 |  2.8 |

Triangles grow with the disk area, so doubling the render distance roughly
quadruples the geometry.

### Supersampling (`-n 4 --view 96`)

| ssaa | ms/frame | FPS | geometry | raster | sky |
|---|---|---|---|---|---|
| 1 | 157.5 | 6.4 | 109.5 |  28.9 |  18.0 |
| 2 | 266.3 | 3.8 | 109.8 |  81.8 |  71.4 |
| 4 | 672.6 | 1.5 | 109.5 | 266.7 | 284.9 |

Triangle counts are identical across the three: `--ssaa` changes only per-pixel
work. Geometry is flat, as expected.

### What the split says

- **Geometry dominates as N grows** — 28% of the frame at one explorer, **86% at
  sixteen** — because every explorer traverses its own region of the world
  independently. Ambient occlusion made this stage heavier: it costs about 20%
  of the frame overall.
- **The sky is constant in N and quadratic in `--ssaa`.** The window is a fixed
  size, so N views each cover 1/N of it. At `--ssaa 4` the sky costs *more than
  the rasterizer* (284.9 ms against 266.7): the per-pixel cloud lookup is
  three octaves of noise, and supersampling multiplies it by 16.
- **Streaming stays under half a percent** in steady state and is not a target.

There is no single bottleneck: which stage matters depends on the operating
point. Pick the operating point before deciding what to optimize.

### The 30 FPS floor

**The sequential build does not reach 30 FPS at any configuration measured
above** — 20.4 FPS at one explorer, 1.8 at sixteen. Recovering that floor is
what the parallel version is for, but the arithmetic has to be faced: 8 cores
mean 1.5x is comfortable at N = 1 and 17x is impossible at N = 16. Either the
demo runs at a modest N, or the quality knobs come down, or both.

## Parallel version

Two binaries are built from the same sources. `_OPENMP` guards every pragma and
every `omp_*` call, so `cubeview-seq` is a genuine single-threaded program, not
the parallel one with its thread count pinned to 1.

```sh
make                    # builds cubeview and cubeview-seq
./cubeview -n 8 --threads 8 --schedule dynamic
./cubeview-seq -n 8
```

`--threads` and `--schedule` are accepted and ignored by the sequential build, so
the same command line drives both and comparisons are easy to script.

### One flat task list per stage

Each frame runs four phases. The first mutates the shared chunk map and stays
serial; the other three are each **one flat list of (view, item) tasks**:

| phase | task | why it is safe |
|---|---|---|
| streaming | — | serial: it mutates the chunk map |
| geometry | one chunk **row** of one view | each row fills its own buffer |
| rasterization | one screen **band** of one view | a band owns its rows exclusively |
| sky | one **slice** of rows of one view | slices write disjoint pixels |

Flattening is what keeps efficiency flat. Splitting by view alone gives coarse
tasks — sixteen views with a 6x cost spread means the heaviest view sets the
frame time — while splitting inside one view at a time pays for entering a
parallel region once per view per stage. One flat list of fine tasks avoids
both, and how finely each view is cut is derived from the thread count so the
total lands near four tasks per thread whatever the explorer count.

Rasterization is split by **screen band and never by triangle**: two triangles
can cover one pixel, and the depth test is a read-modify-write of that shared
slot. `raster_triangle()` has taken a clip rectangle since the sequential
version was written, which is why the band split was a few lines rather than a
rewrite.

Geometry rows are stitched back **in row order**, exactly the order the serial
scan produces. That is what makes the output independent of how rows were spread
across threads, and it is why the split is by row rather than by an arbitrary
partition of the chunks.

With a single thread the flattening buys nothing and costs the per-row buffers
and the stitching, so that case takes the direct path instead.

#### Speedup fell while the program got faster

Two and four explorers used to show 3.79x and 3.47x; they now show 2.96x and
2.70x. Nothing regressed. Frustum culling and the cheaper cloud sampling made
the *sequential* baseline much faster, so the fixed cost of the parallel
machinery is a larger share of a smaller number.

Speedup measures how well work divides, not how fast the program is. Optimizing
the algorithm lowers it and is still the right thing to do — which is why the
FPS column is there beside it.

#### One word was worth 1.6x

The sky loop first used `schedule(static)`, and the sky then scaled at only 2.9x
while geometry and rasterization reached 5-6x. The reason is visible in the
scene: rows near the top of a pane are open sky and pay the full three-octave
cloud lookup, rows near the bottom are covered by terrain and the depth test
rejects them immediately. A static split hands one thread every expensive row
and another every cheap one.

With `schedule(dynamic, 8)` the sky went from 6.91 ms to 2.69 ms, the frame from
13.0 ms to 8.1 ms, and one explorer from 77 to 121 FPS.

The lesson generalizes: **an even split of iterations is not an even split of
work.** Look at what each iteration actually does before choosing a policy.

### Thread scaling (`-n 8`)

| threads | ms | speedup | efficiency |
|---|---|---|---|
| 1 | 202.6 | 0.96 | 96.3% |
| 2 | 116.7 | 1.67 | 83.6% |
| 4 |  68.1 | 2.87 | 71.7% |
| 8 |  47.9 | 4.07 | 50.9% |

Efficiency decays the way it always does. One thread at 0.96 rather than 1.00 is
the cost of the parallel machinery itself, which is small and real.

### Amdahl, and why the sky mattered

At one explorer the sequential split was geometry 28%, rasterization 30%, sky
41%. Parallelizing geometry alone caps the speedup at

    1 / (0.72 + 0.28/8) = 1.32x

and the measured figure was 1.24x — 94% of a ceiling that was simply too low.
The gain came from raising the parallel fraction, not from optimizing what was
already parallel: with all three stages split, the same configuration reaches
2.56x.

### Scheduling policy

`--schedule` selects the policy for the three measured stages, which are
compiled with `schedule(runtime)`. Default is `dynamic`.

| N | `static` | `dynamic` | dynamic wins by |
|---|---|---|---|
| 1  | 11.35 ms |  5.82 ms | **48.7%** |
| 2  | 13.02 ms |  7.26 ms | 44.3% |
| 4  | 19.08 ms | 13.95 ms | 26.9% |
| 8  | 27.10 ms | 23.09 ms | 14.8% |
| 16 | 44.45 ms | 41.41 ms | 6.8% |

Dynamic wins everywhere, and its advantage **shrinks** as the explorer count
grows. That is the opposite of what the same comparison showed before the tasks
were flattened, and the reversal is instructive: the right policy depends on the
tasks, not on the program.

When a task was a whole view, four views on eight threads gave every thread at
most one task. There was nothing to rebalance and dynamic only charged its own
overhead, so static won. Now a task is a chunk row, a screen band or a slice of
sky, and those vary enormously — a row through the middle of the render disk
holds a dozen chunks while a row at its edge holds one; a band across the
horizon covers far more filled pixels than a band of open sky.

The advantage narrows at high N because each view is then cut into fewer pieces
(the split count is derived from threads over views), so the tasks are coarser
and more uniform again.

### A warning about measurement

These numbers move a great deal with system load, and the parallel build is hurt
worse than the sequential one because it competes for the same cores. The same
configuration measured 0.87x on a machine at load 8.5 and 1.24x on the same
machine at load 2.0.

A design decision was very nearly made on the strength of the polluted figure.
**Check the load average before trusting a speedup.**

## Determinism

Textures, terrain, the flight paths and the benchmark time step all use fixed
values, so two runs with identical arguments produce byte-identical output. That
makes `--dump` a correctness check for the parallel build:

```sh
./cubeview -n 4 --view 96 --ssaa 2 --dump reference.ppm
cmp reference.ppm candidate.ppm
```

If a single byte differs, there is a race. Verified across 1, 2, 4, 8 and 16
threads under both schedules: byte-identical to `cubeview-seq` in every case.

Two things this required:

- `world_emit_view()` visits chunks in a **sorted box scan**, not in hash order.
  Otherwise the triangle order would depend on which chunks happened to load
  first, and two runs would differ.
- Headless dumps do not draw a frame rate. A wall-clock reading differs on every
  run, and a single-frame reading is meaningless anyway.

## Notes for the parallel version

**Views.** The loop over `ViewTask`s is the primary decomposition and is
race-free by construction. Per-view cost is deliberately uneven — explorers fly
at different speeds with different view distances — so `schedule(static)` and
`schedule(dynamic)` are worth measuring rather than assuming.

**Streaming is a separate phase.** It mutates the shared chunk map, so it cannot
simply be folded into the render loop. It is also only ~0.2% of a steady-state
frame, so it is not where the speedup is.

**Raster, if a single view must be split.** Partition by screen tile, never by
triangle: two triangles can cover the same pixel and the depth test is a
read-modify-write of one shared slot. `raster_triangle()` already takes a clip
rectangle for exactly this, and every `ScreenTriangle` carries a clamped bounding
box so a tile can reject non-overlapping triangles cheaply.

**Geometry, if a single view must be split.** Partition by chunk with a private
`TriangleBuffer` per worker, then concatenate. `tribuf_push()` appends to a
shared buffer and would race on `count`. Keep the concatenation order fixed or
the byte-identical `--dump` check stops working.

## Known gaps

- **The 30 FPS floor is not met sequentially** at any measured setting. See
  above.
- **The sky is disproportionately expensive**, and at high supersampling it
  outweighs the rasterizer. The cloud lookup is three octaves of noise per sky
  pixel; evaluating it at display resolution and upsampling, or caching it per
  frame, would cut it sharply.
- The first frames stream thousands of chunks at once and stall visibly. A
  per-frame generation budget would smooth it.
- No frustum culling of chunks: the full disk around each camera is walked, even
  the part behind it.
- No distance fog, so the edge of the render distance is a hard horizon.
- Trees on a chunk seam lose part of their canopy, the usual cost of generating
  chunks independently.
- Terrain still reads as gentle from the air: the explorers fly high enough that
  the relief is foreshortened.

## Layout

| File | Responsibility |
|---|---|
| `src/math3d.{h,c}` | Vectors, 4x4 matrices, projection and view matrices |
| `src/noise.{h,c}` | Value noise, fBm, hash scattering |
| `src/texture.{h,c}` | Procedural 16x16 block textures and block definitions |
| `src/camera.{h,c}` | Explorer flight path, view-projection, stream radius |
| `src/render.{h,c}` | Framebuffer, viewports, `cube_emit()`, rasterizer |
| `src/sky.{h,c}` | Day/night cycle, sky gradient, clouds, stars, sun and moon |
| `src/world.{h,c}` | Chunk map, streaming, eviction, terrain, `world_emit_view()` |
| `src/overlay.{h,c}` | Embedded 5x7 bitmap font and HUD primitives |
| `src/main.c` | CLI, scene setup, split-screen layout, render loop, modes |
| `scripts/sweep.sh` | Benchmark sweep used to produce the tables above |
| `scripts/make_texture_atlas.py` | Converts a texture pack into the atlas format |
