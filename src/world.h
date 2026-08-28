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
#define CHUNK_SIZE_Y 64
#define CHUNK_SIZE_Z 16
#define CHUNK_VOLUME (CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z)

typedef struct {
    uint8_t blocks[CHUNK_VOLUME];
    /* One past the highest non-air block. The emission loop stops here instead
     * of walking all CHUNK_SIZE_Y layers, which matters because most of a tall
     * chunk is empty sky. */
    uint16_t height_limit;
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
    float scale;         /* world units per noise unit; larger = broader hills */
    int octaves;
    float lacunarity;
    float gain;
    float base_height;
    float amplitude;          /* vertical range of ordinary rolling terrain */
    float mountain_amplitude; /* extra height where the mountain mask is high */
    float warp_strength;      /* how far the domain warp displaces the sample */
    int sea_level;
    int beach_margin;    /* blocks above sea level that still count as shore */
    int snow_line;       /* height at or above which surfaces are snow */
    float tree_chance;   /* base probability; biomes scale it */
} TerrainParams;

/* Biomes come from two low-frequency fields, temperature and humidity, crossed
 * with the terrain height. Each one selects a different surface palette and a
 * different tree density. */
typedef enum {
    BIOME_OCEAN,
    BIOME_BEACH,
    BIOME_DESERT,
    BIOME_PLAINS,
    BIOME_FOREST,
    BIOME_TUNDRA,
    BIOME_MOUNTAIN
} Biome;

/* Height and biome for one column, sampled together so the noise fields are
 * evaluated once rather than once per query. */
typedef struct {
    int height;
    Biome biome;
} TerrainSample;

TerrainSample terrain_sample(const TerrainParams *params, int world_x, int world_z);

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
/* Camera wedge used to reject chunks the view cannot possibly contain.
 *
 * A point is inside the horizontal extent when |dot(d, right)| <= tan_half * 
 * dot(d, forward), with d measured from the eye. Expressed against the camera's
 * own axes it stays correct however the camera is pitched, which a test on
 * world-space angles would not. */
typedef struct {
    Vec3 eye;
    Vec3 forward;
    Vec3 right;
    float tan_half_h;  /* tangent of the horizontal half field of view */
} ViewFrustum;

/* Rows of chunks a view can span at the largest supported render distance.
 * world_emit_view() parallelizes over rows, one scratch buffer each. */
#define WORLD_MAX_CHUNK_ROWS 320

/* Number of chunk rows a view spans at this render distance. The caller needs
 * it to build a flat task list across several views. */
int world_row_count(float render_radius);

/* Emits one chunk row, addressed by index rather than by chunk coordinate.
 * Rows are independent, so a caller can run any set of (view, row) pairs
 * concurrently as long as each writes its own buffer. */
size_t world_emit_row(TriangleBuffer *out, const World *world, Vec3 camera_pos,
                      float render_radius, Mat4 vp, const Light *light,
                      const Viewport *view, const ViewFrustum *frustum,
                      int row_index);

/* `rows` is optional scratch, one TriangleBuffer per chunk row. When it is
 * supplied and large enough, the geometry stage runs the rows in parallel and
 * stitches them back together IN ROW ORDER, which is exactly the order the
 * serial scan produces -- so the output is identical whatever the thread count.
 * Pass NULL to force the serial path. */
size_t world_emit_view(TriangleBuffer *out, const World *world, Vec3 camera_pos,
                       float render_radius, Mat4 vp, const Light *light,
                       const Viewport *view, const ViewFrustum *frustum,
                       TriangleBuffer *rows, int row_capacity);

/* Solid blocks across every loaded chunk, for reporting culling savings. */
size_t world_solid_blocks(const World *world);

#endif /* WORLD_H */
