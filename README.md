# Voxel Screensaver — Sequential Version

Proyecto #1, Computación Paralela y Distribuida (UVG).

`N` first-person explorers fly over an endless, procedurally generated voxel
world in split-screen. Terrain is streamed in around each explorer as it
advances. Everything is rendered on the CPU by our own software rasterizer;
SDL2 is used only to open a window and blit the finished framebuffer.

This is the **sequential** version — the baseline the parallel version is
measured against.

## Documentation

| File | Contents |
|---|---|
| `docs/matematica.md` | The maths behind each pipeline stage and why it is that way |
| `docs/decisiones.md` | Design decision record: what, why, alternatives, consequences |
| `AGENTS.md` | Working practices, invariants and the determinism contract |

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

Layering: snow above `snow_level`, sand at or below `sand_level`, otherwise grass
over dirt over stone; ores scattered with a 3D hash; trees are a log trunk with a
two-layer canopy. All knobs live in `TerrainParams`.

## Texture packs

Textures are procedural by default, so the program runs with no assets at all.
A Minecraft-style pack can be used instead:

```sh
scripts/make_texture_atlas.py /path/to/pack build/pack.vxtx --size 32
./cubeview -n 4 --textures build/pack.vxtx
```

**Packs are converted offline, not decoded at runtime.** The renderer has no PNG
decoder on purpose: adding one would mean either a new build dependency or
several thousand lines of third-party code in a project whose brief asks for our
own. The script does the decoding with ImageMagick and writes a raw block of
pixels the program reads with one `fread`.

Two details the converter handles, without which a pack looks wrong:

- Grass and leaf textures ship **greyscale** in Minecraft packs; the game tints
  them per biome at runtime. Loaded as-is they come out white, so the script
  applies a fixed tint.
- Water and fire are **animation strips**: a tall image holding N square frames.
  Only the first frame is taken.

Texture size is per-texture rather than a compile-time constant, so 16, 32, 64
and 128 packs all work. Every pack uses a power-of-two edge, which keeps
wrapping a single AND in the rasterizer's innermost loop.

A pack that is missing, malformed or the wrong size is reported and **ignored**:
the procedural set stays in place and the program keeps running.

> Packs are not committed. `*.vxtx`, `textures/` and `packs/` are in
> `.gitignore` — the repository is made public for grading, and third-party art
> must not be redistributed. Note in your report which pack you used and where
> to get it.

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

### Explorers (`--view 96 --ssaa 1`)

| N | triangles | chunks | ms/frame | FPS |
|---|---|---|---|---|
| 1  |  25948 |  720 |  19.03 | 52.6 |
| 2  |  36148 | 1843 |  27.89 | 35.9 |
| 4  | 140870 | 2898 |  55.00 | 18.2 |
| 8  | 288753 | 4841 | 100.47 |  9.9 |
| 16 | 604685 | 5533 | 191.83 |  5.2 |

**The sequential build falls below 30 FPS from N = 4 onwards.** Recovering that
floor is what the parallel version is for.

### Render distance (`-n 4 --ssaa 1`)

| view | triangles | chunks | ms/frame | FPS |
|---|---|---|---|---|
| 48  |  33002 |  866 |  19.24 | 52.0 |
| 96  | 140870 | 2898 |  54.37 | 18.4 |
| 160 | 389265 | 7282 | 128.33 |  7.8 |

Triangles grow with the disk area, so doubling the render distance roughly
quadruples the geometry.

### Supersampling (`-n 4 --view 96`)

| ssaa | triangles | ms/frame | FPS |
|---|---|---|---|
| 1 | 140933 |  53.53 | 18.7 |
| 2 | 140844 |  84.85 | 11.8 |
| 4 | 140795 | 190.72 |  5.2 |

Triangle counts are identical: `--ssaa` changes only the raster workload.

### Where the time goes

Measured per stage with `--bench`, not derived:

| configuration | stream | geometry | raster | sky |
|---|---|---|---|---|
| `-n 4  --view 96 --ssaa 1` | 0.4% | **56.6%** | 25.6% | 16.6% |

Earlier measurements, before the textured sky existed:

| configuration | stream | geometry | raster |
|---|---|---|---|
| `-n 4  --view 96 --ssaa 1` | 0.2% | **68.1%** | 30.6% |
| `-n 4  --view 96 --ssaa 4` | 0.1% | 19.1% | **76.8%** |
| `-n 16 --view 96 --ssaa 1` | 0.2% | **83.1%** | 16.3% |

Three things this says:

1. **Streaming is not a bottleneck.** In steady state only a handful of chunks
   are generated per frame; the cost is concentrated in the first frames, which
   is why the benchmark has a warmup.
2. **Geometry dominates as N grows**, because every explorer traverses its own
   region of the world independently.
3. **Raster dominates as `--ssaa` grows**, with the triangle count untouched.

There is no single bottleneck: which stage matters depends on the operating
point. Pick the operating point before deciding what to optimize.

## Determinism

Textures, terrain, the flight paths and the benchmark time step all use fixed
values, so two runs with identical arguments produce byte-identical output. That
makes `--dump` a correctness check for the parallel build:

```sh
./cubeview -n 4 --view 96 --ssaa 2 --dump reference.ppm
cmp reference.ppm candidate.ppm
```

If a single byte differs, there is a race.

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

- The first frames stream thousands of chunks at once and stall visibly. A
  per-frame generation budget would smooth it.
- No frustum culling of chunks: the full disk around each camera is walked, even
  the part behind it.
- No distance fog, so the edge of the render distance is a hard horizon.
- Trees on a chunk seam lose part of their canopy, the usual cost of generating
  chunks independently.

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
