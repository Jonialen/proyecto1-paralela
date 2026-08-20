# Voxel Screensaver — Sequential Version

Proyecto #1, Computación Paralela y Distribuida (UVG).

`N` first-person explorers fly over procedurally generated voxel terrain in
split-screen. Everything is rendered on the CPU by our own software rasterizer;
SDL2 is used only to open a window and blit the finished framebuffer.

This is the **sequential** version — the baseline the parallel version is
measured against.

## Build

```sh
make            # requires SDL2 development headers
./cubeview -n 4
```

## Usage

```
-n, --players N   explorers rendered in split-screen, 1-16   (default 1)
    --chunks N    world size in chunks per side, 1-64        (default 4)
    --seed N      terrain seed                               (default 1337)
    --width N     window width, minimum 640                  (default 960)
    --height N    window height, minimum 480                 (default 720)
    --ssaa N      supersampling factor, 1-8                  (default 1)
    --scene NAME  'chunk' or 'block'                         (default chunk)
    --block N     block index for the block scene
    --bench N     render N frames headless and report timings
    --dump PATH   render one frame headless into a binary PPM
    --help        show this message
```

`Esc` or `Q` closes the window.

Every argument is validated: non-numeric values, out-of-range values and missing
values are rejected with an explicit message and a non-zero exit code. Nothing
is hard-coded.

```
$ ./cubeview -n 99
Error: -n must be between 1 and 16, got 99
```

## What is on screen

The HUD is drawn into the resolved image with an embedded 5x7 bitmap font
(`src/overlay.c`), so it costs the same at any `--ssaa` setting and needs no
font file.

- **FPS**, updated live. It is drawn **red below 30** and green at or above it,
  so a frame rate under the project's floor is visible at a glance.
- Total triangles, view count, supersampling factor, and which build is running.
- Per-view label with that explorer's triangle count — the numbers that show the
  load imbalance between workers.

## How the explorers move

Each explorer flies a closed Lissajous path, which is pure trigonometry and, by
being closed, never leaves the world:

```
x(t) = cx + radius_x * sin(rate_x * t * speed + phase_x)
z(t) = cz + radius_z * sin(rate_z * t * speed + phase_z)
y(t) = height + bob_amplitude * sin(bob_rate * t * speed + phase_y)
```

Different `rate_x` and `rate_z` turn the circle into a figure-eight, so the
explorer sweeps the map instead of orbiting one ring. Each explorer gets its own
phase, speed and view distance.

The heading is taken from the **horizontal** travel direction only. Using the
full path derivative is wrong: near the extremes of the vertical sine the bob
velocity dominates the horizontal one and the camera ends up staring at the sky.
Pitch is its own gentle oscillation, biased downwards to keep terrain in frame.

## Architecture

```
                     GEOMETRY STAGE                      RASTER STAGE
  world  ->  world_emit() / cube_emit()  ->  TriangleBuffer  ->  raster_flush()
             - neighbour face culling                           - z-buffer test
             - MVP transform                                    - perspective-correct UVs
             - near-plane clipping                              - texture sample + shading
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
buffers, read-only access to the world. The whole parallel decomposition is one
loop in `render_frame()`:

```c
for (int i = 0; i < scene->view_count; i++)
    render_view(fb, &scene->views[i], scene, t);
```

No locks, no atomics, no reduction. `world` is a pointer rather than a shared
global on purpose: today every view points at the same world, and pointing them
at different worlds is all that separates split-screen from independent
sub-worlds.

`to_screen()` maps NDC into the **viewport rectangle** rather than the whole
framebuffer, and triangle bounding boxes are clamped to the viewport, so one
camera's geometry can never bleed into a neighbouring pane.

### Terrain

`terrain_height()` is the only place the noise is sampled: four octaves of value
noise (`src/noise.c`), lattice hash plus a Hermite smoothstep.

**The noise is evaluated in world coordinates**, `chunk_x * CHUNK_SIZE_X + x`,
not chunk-local ones. That is why adjacent chunks join seamlessly instead of each
generating a disconnected island with a step at every border.

Layering: snow above `snow_level`, sand at or below `sand_level`, otherwise grass
over dirt over stone; ores scattered with a 3D hash; trees are a log trunk with a
two-layer canopy, placed only on grass. All knobs live in `TerrainParams`.

### Face culling

A chunk is a dense `uint8_t blocks[16*32*16]` grid; id 0 is air. `world_emit()`
resolves neighbours across chunk borders and drops every face that touches a
solid block. Without it a 4x4 world would emit hundreds of thousands of
triangles that are never visible.

## The workload knobs

| Flag | Scales | Which stage |
|---|---|---|
| `-n, --players N` | explorers, one full world traversal each | **geometry**, ~linear in N |
| `--chunks N` | voxels in the world, as N² | **geometry** |
| `--ssaa N` | per-pixel raster work, as N² | **raster** |

They are deliberately independent, because the two stages respond to different
ones.

## Sequential baseline

960x720, `--chunks 4 --ssaa 2`, 40 frames per point, gcc `-O2`.

| N (players) | triangles | ms/frame | FPS |
|---|---|---|---|
| 1  |  2041 | 32.65 | 30.6 |
| 2  |  2591 | 33.16 | 30.2 |
| 4  |  8852 | 40.17 | 24.9 |
| 8  | 17223 | 48.94 | 20.4 |
| 16 | 33762 | 74.84 | 13.4 |

Raster time barely moves with `N` — the window is a fixed size, so N views each
cover 1/N of it and the total pixel count is constant. Almost all of the growth
from 32.6 ms to 74.8 ms is the geometry stage: every view traverses the whole
world independently.

**The sequential build drops below 30 FPS from N = 4 onwards.** Recovering that
floor is what the parallel version is for.

Per-view triangle counts from one 4-explorer frame:

```
P0 1600    P1 3828    P2 854    P3 3525
```

A 4.5x spread between the lightest and the heaviest view. The explorers fly at
different speeds with different view distances on purpose, so the workers get
unequal work — which is what makes OpenMP scheduling policy worth measuring
rather than assuming.

## Determinism

Textures, terrain and the benchmark time step all use fixed values, so two runs
with identical arguments produce byte-identical output. That makes `--dump` a
correctness check for the parallel build:

```sh
./cubeview -n 4 --ssaa 2 --dump reference.ppm
cmp reference.ppm candidate.ppm
```

If a single byte differs, there is a race.

## Notes for the parallel version

**Views.** The loop over `ViewTask`s is the primary decomposition and it is
already race-free by construction. Unequal per-view cost is the reason to
compare `schedule(static)` against `schedule(dynamic)`.

**Raster, if a single view has to be split.** Partition by screen tile, never by
triangle: two triangles can cover the same pixel and the depth test is a
read-modify-write of one shared slot. `raster_triangle()` already takes a clip
rectangle for exactly this, and every `ScreenTriangle` carries a clamped bounding
box so a tile can reject non-overlapping triangles cheaply.

**Geometry, if a single view has to be split.** Partition by block with a private
`TriangleBuffer` per worker, then concatenate. `tribuf_push()` appends to a
shared buffer and would race on `count`. Keep the concatenation order fixed or
the byte-identical `--dump` check stops working.

`framebuffer_clear()` and `framebuffer_resolve()` are trivially parallel over
rows.

## Layout

| File | Responsibility |
|---|---|
| `src/math3d.{h,c}` | Vectors, 4x4 matrices, projection and view matrices |
| `src/noise.{h,c}` | Value noise, fBm, hash scattering |
| `src/texture.{h,c}` | Procedural 16x16 block textures and block definitions |
| `src/camera.{h,c}` | Explorer flight path and view-projection matrix |
| `src/render.{h,c}` | Framebuffer, viewports, `cube_emit()`, rasterizer |
| `src/world.{h,c}` | Chunk storage, face masks, terrain generation, `world_emit()` |
| `src/overlay.{h,c}` | Embedded 5x7 bitmap font and HUD primitives |
| `src/main.c` | CLI, scene setup, split-screen layout, render loop, modes |
| `scripts/sweep.sh` | Benchmark sweep used to produce the tables above |
