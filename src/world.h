#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include <stddef.h>
#include "math3d.h"
#include "render.h"

/* A chunk is a dense column of voxels. 0 (BLOCK_AIR) means empty; any other
 * value indexes the block table through block_from_id(). The world is infinite
 * on X and Z and bounded on Y, so a chunk is addressed by (cx, cz) alone. */
#define CHUNK_SIZE_X 16
#define CHUNK_SIZE_Y 32
#define CHUNK_SIZE_Z 16
#define CHUNK_VOLUME (CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z)

typedef struct {
    uint8_t blocks[CHUNK_VOLUME];
} Chunk;

void chunk_clear(Chunk *chunk);
uint8_t chunk_get(const Chunk *chunk, int x, int y, int z);
void chunk_set(Chunk *chunk, int x, int y, int z, uint8_t id);

/* ------------------------------------------------------- terrain generation */

/* Every knob the heightmap exposes. The noise is evaluated in WORLD
 * coordinates, which is what lets chunks be generated independently, in any
 * order, and still line up seamlessly. */
typedef struct {
    uint32_t seed;
    float scale;       /* world units per noise unit; larger = broader hills */
    int octaves;
    float lacunarity;
    float gain;
    float base_height;
    float amplitude;
    int sand_level;
    int snow_level;
    float tree_chance;
} TerrainParams;

TerrainParams terrain_default(uint32_t seed);
int terrain_height(const TerrainParams *params, int world_x, int world_z);
/* Highest ground the generator can produce, in blocks. */
float terrain_peak(const TerrainParams *params);
void chunk_generate(Chunk *chunk, const TerrainParams *params, int chunk_x, int chunk_z);

/* --------------------------------------------------------- streaming world */

/* Chunks are kept in an open-addressed map keyed by (cx, cz), so the world can
 * extend in any direction without a bounding box. A chunk is generated the
 * first time an explorer comes within its generation radius and dropped once no
 * explorer has needed it for a while. */
#define WORLD_EVICT_GRACE_FRAMES 120
#define WORLD_DEFAULT_MAX_CHUNKS 6000

enum { CHUNK_SLOT_EMPTY = 0, CHUNK_SLOT_USED = 1, CHUNK_SLOT_DEAD = 2 };

typedef struct {
    int cx, cz;
    uint8_t state;
    uint32_t touch;   /* frame this chunk was last needed */
    Chunk *chunk;
} ChunkSlot;

typedef struct {
    ChunkSlot *slots;
    size_t capacity;      /* power of two */
    size_t used;
    size_t tombstones;
    size_t max_chunks;
    TerrainParams params;
    uint32_t frame;

    /* Per-frame counters, for the HUD and for the benchmark report. */
    size_t generated_this_frame;
    size_t evicted_this_frame;
} World;

int world_init(World *world, const TerrainParams *params, size_t max_chunks);
void world_free(World *world);

size_t world_chunk_count(const World *world);
const Chunk *world_find_chunk(const World *world, int cx, int cz);
/* Block lookup in world coordinates; anything not loaded reads as air. */
uint8_t world_get(const World *world, int x, int y, int z);

/* --- streaming, called once per frame in this order --- */

void world_begin_frame(World *world);

/* Generates every missing chunk within `generate_radius` world units of
 * `center`, and marks all of them as needed this frame. Returns how many were
 * newly generated. Call once per explorer. */
size_t world_stream_around(World *world, Vec3 center, float generate_radius);

/* Drops chunks no explorer has needed for WORLD_EVICT_GRACE_FRAMES frames, and
 * enforces the max_chunks ceiling by dropping the least recently needed.
 * Returns how many were dropped. */
size_t world_end_frame(World *world);

/* --------------------------------------------------------------- rendering */

/* Emits the visible faces of every loaded chunk within `render_radius` of the
 * camera. Chunks are visited in a sorted box scan rather than in hash order, so
 * the triangle order does not depend on which chunks happened to load first --
 * without that, two runs would produce different output and the byte-identical
 * --dump check would be worthless. */
size_t world_emit_view(TriangleBuffer *out, const World *world, Vec3 camera_pos,
                       float render_radius, Mat4 vp, Vec3 light_dir,
                       const Viewport *view);

/* Solid blocks across every loaded chunk, for reporting culling savings. */
size_t world_solid_blocks(const World *world);

#endif /* WORLD_H */
