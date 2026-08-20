#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include "math3d.h"
#include "render.h"

/* A chunk is a dense grid of block ids. 0 (BLOCK_AIR) means empty; any other
 * value indexes the block table through block_from_id(). Dense storage is the
 * right call here: lookups are O(1) and the neighbour test that drives face
 * culling has to run for all six sides of every solid block. */
#define CHUNK_SIZE_X 16
#define CHUNK_SIZE_Y 32
#define CHUNK_SIZE_Z 16
#define CHUNK_VOLUME (CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z)

typedef struct {
    uint8_t blocks[CHUNK_VOLUME];
} Chunk;

void chunk_clear(Chunk *chunk);

/* Out-of-range coordinates read as BLOCK_AIR, so border blocks correctly keep
 * their outward faces instead of being culled against nothing. */
uint8_t chunk_get(const Chunk *chunk, int x, int y, int z);
void chunk_set(Chunk *chunk, int x, int y, int z, uint8_t id);

/* Returns the FACE_* bits whose neighbour is air. A fully buried block returns
 * 0 and costs nothing beyond the six lookups. */
unsigned chunk_face_mask(const Chunk *chunk, int x, int y, int z);

/* Geometry stage for a single standalone chunk. `origin` is the world position
 * of the chunk's (0,0,0) corner. Border faces are kept, which is correct for a
 * lone chunk; use world_emit() when neighbours exist. */
size_t chunk_emit(TriangleBuffer *out, const Chunk *chunk, Vec3 origin,
                  Mat4 vp, Vec3 light_dir, int fb_width, int fb_height);

/* ------------------------------------------------------- terrain generation */

/* Every knob the heightmap exposes. All of it feeds noise_fbm_2d() evaluated in
 * WORLD coordinates, which is what makes neighbouring chunks line up seamlessly
 * instead of each generating its own disconnected island. */
typedef struct {
    uint32_t seed;
    float scale;       /* world units per noise unit; larger = broader hills */
    int octaves;
    float lacunarity;
    float gain;
    float base_height; /* height of the lowest terrain */
    float amplitude;   /* vertical range added on top of base_height */
    int sand_level;    /* columns at or below this get a sandy shore */
    int snow_level;    /* columns at or above this get snow and bare stone */
    float tree_chance; /* probability per eligible surface column */
} TerrainParams;

TerrainParams terrain_default(uint32_t seed);

/* Surface height of one world column. The only place the noise is sampled. */
int terrain_height(const TerrainParams *params, int world_x, int world_z);

/* Fills one chunk of a world laid out on a chunk grid. */
void chunk_generate(Chunk *chunk, const TerrainParams *params,
                    int chunk_x, int chunk_z);

/* ------------------------------------------------------------------- world */

/* A square grid of chunks, centred on the origin so the orbit camera works
 * unchanged. Raising `size` is the scene-side workload knob, the counterpart to
 * --ssaa on the pixel side. */
typedef struct {
    int size;              /* size x size chunks */
    Chunk *chunks;         /* size * size, indexed [cz * size + cx] */
    TerrainParams params;
    Vec3 origin;           /* world position of block (0,0,0) of chunk (0,0) */
} World;

int world_init(World *world, int size, const TerrainParams *params);
void world_free(World *world);
void world_generate(World *world);

/* World-space block lookup across chunk boundaries; out of range is air. */
uint8_t world_get(const World *world, int x, int y, int z);

/* Geometry stage for the whole world. Unlike chunk_emit() this resolves
 * neighbours ACROSS chunk borders, so the seam faces between two touching
 * chunks are culled instead of being rasterized and then hidden by the depth
 * test. */
size_t world_emit(TriangleBuffer *out, const World *world, Mat4 vp,
                  Vec3 light_dir, int fb_width, int fb_height);

/* Total solid blocks, for reporting how much the face culling actually saves. */
size_t world_solid_blocks(const World *world);

#endif /* WORLD_H */
