# Voxel Renderer — Sequential CPU Implementation

Minecraft-style voxel rendering written entirely in C. Every pixel is produced
by our own code; SDL2 is used *only* to open a window and blit the finished
framebuffer. Two scenes ship today: a single textured block, and a 16x16x16
chunk with per-face neighbour culling.

## Why a software rasterizer

An OpenGL version would hand all the work to the GPU, leaving nothing to
parallelize and nothing to measure. Here the whole pipeline is ours.

## Pipeline

The renderer is split into two stages that are timed separately, because they
parallelize in completely different ways.

```
                          GEOMETRY STAGE                    RASTER STAGE
  world/block  ->  cube_emit()               ->  TriangleBuffer  ->  raster_flush()
                   - neighbour face culling                          - z-buffer test
                   - MVP transform                                   - perspective-correct UVs
                   - near-plane clipping                             - texture sample + shading
                   - perspective divide
                   - backface culling
                   - screen bbox
```

Nothing is drawn during the geometry stage. It only appends `ScreenTriangle`
values — already projected, already bounded — to a `TriangleBuffer`. The raster
stage then consumes that list.

## The cube entry point

`cube_emit()` is the single function every caller goes through:

```c
size_t cube_emit(TriangleBuffer *out, const Block *block, Mat4 model, Mat4 vp,
                 Vec3 light_dir, unsigned face_mask, int fb_width, int fb_height);
```

| Caller | `model` | `face_mask` |
|---|---|---|
| Single block scene | identity | `FACE_ALL` |
| `chunk_emit()` | translation to the voxel centre | neighbour-derived |

`face_mask` uses the `FACE_POS_X`..`FACE_NEG_Z` bits, in the same order as the
internal face table and as `block_texture_for_face()`. Adding a new caller —
several chunks, a moving entity, an animated block — means calling `cube_emit()`
with a different model matrix and mask. Nothing else changes.

## Chunks

```c
Chunk chunk;
chunk_fill_demo(&chunk);                       /* placeholder terrain */
chunk_emit(&tris, &chunk, origin, vp, light, fb.fb_width, fb.fb_height);
raster_flush(&fb, &tris);
```

A chunk is a dense `uint8_t blocks[16*16*16]` grid. Id `0` is air; id `N` is
`block_from_id(N)`. Dense storage keeps `chunk_get()` at O(1), which matters
because `chunk_face_mask()` performs six neighbour lookups for every solid
block.

**Neighbour face culling is what makes this affordable.** The demo chunk holds
1686 solid blocks. Drawing all of them naively is 10116 faces / 20232 triangles;
culling every face that touches a solid neighbour leaves ~1100 triangles, a ~18x
reduction. Out-of-range lookups return air on purpose, so border blocks keep
their outward faces.

### Procedural terrain (next step)

`chunk_fill_demo()` in `src/world.c` fills each column from `column_height(x, z)`
— currently two sine waves. That is deliberately the same shape a real generator
has: one height per `(x, z)` column. Replacing that function body with
value/Perlin noise gives real terrain, and nothing downstream needs to change.

## Build and run

```sh
make          # requires SDL2 development headers
./cubeview
./cubeview --scene chunk
```

| Input | Action |
|---|---|
| Left drag / arrow keys | Orbit camera |
| `W`, `S`, mouse wheel | Zoom |
| `Tab` | Switch block scene <-> chunk scene |
| `1`..`8` | Pick block |
| `[`, `]` | Cycle blocks |
| `+`, `-` | Change supersampling (1-8) |
| `Space` | Toggle auto-orbit |
| `R` | Reset camera |
| `Esc` / `Q` | Quit |

## Command line

```
--width N     window width           (default 900)
--height N    window height          (default 700)
--ssaa N      supersampling 1-8      (default 1)
--block N     initial block index    (default 0)
--scene NAME  'block' or 'chunk'     (default block)
--bench N     render N frames headless and report timings
--dump PATH   render one frame headless to a binary PPM
```

## Measuring

`--bench` reports the per-stage split:

```
$ ./cubeview --scene chunk --width 1280 --height 720 --ssaa 4 --bench 60
Triangles:  1142 (last frame)
Per frame:  70.6325 ms  (14.16 FPS)
  geometry:  0.1505 ms  (0.2%)
  raster:   70.4812 ms  (99.8%)
```

Supersampling is the workload knob: it multiplies per-pixel work by `ssaa^2`
without changing the window size. At `--ssaa 1` the scene is too cheap to
measure reliably.

Rendering is deterministic — procedural textures use a fixed hash and the
benchmark path has no time-dependent state. Two runs with identical arguments
produce byte-identical PPM output, so `--dump` is a correctness check for any
future optimized build:

```sh
./cubeview --scene chunk --ssaa 4 --dump reference.ppm
cmp reference.ppm candidate.ppm
```

## Notes for the parallel version

The raster stage is ~99.8% of frame time at high SSAA, so that is where the work
is. It cannot be parallelized over triangles: two triangles can cover the same
pixel, and the depth test is a read-modify-write of one shared slot — that is a
data race, and it is non-deterministic even when the picture looks plausible.

`raster_triangle()` therefore takes a clip rectangle:

```c
void raster_triangle(Framebuffer *fb, const ScreenTriangle *tri,
                     int clip_min_x, int clip_min_y, int clip_max_x, int clip_max_y);
```

Give each worker its own screen tile and iterate the triangle list inside it.
Every worker then owns its depth slots exclusively, no synchronization is needed,
and the output stays byte-identical to the sequential build. Each
`ScreenTriangle` already carries a clamped screen bounding box so a tile can
reject non-overlapping triangles without recomputing anything.

The geometry stage parallelizes over blocks, but it is a rounding error in the
profile — do not start there.

## Layout

| File | Responsibility |
|---|---|
| `src/math3d.{h,c}` | Vectors, 4x4 matrices, projection and view matrices |
| `src/texture.{h,c}` | Procedural 16x16 block textures and block definitions |
| `src/render.{h,c}` | Framebuffer, `cube_emit()`, triangle buffer, rasterizer |
| `src/world.{h,c}` | Chunk storage, face masks, terrain fill, `chunk_emit()` |
| `src/main.c` | SDL2 window, input, scenes, benchmark and dump modes |
