#include "world.h"
#include "noise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Block indices in the table declared by texture.c, turned into grid ids. */
#define ID_GRASS   block_id_from_index(0)
#define ID_DIRT    block_id_from_index(1)
#define ID_STONE   block_id_from_index(2)
#define ID_COBBLE  block_id_from_index(3)
#define ID_GOLD    block_id_from_index(4)
#define ID_DIAMOND block_id_from_index(5)
#define ID_LOG     block_id_from_index(6)
#define ID_SAND    block_id_from_index(8)
#define ID_SNOW    block_id_from_index(9)
#define ID_LEAVES  block_id_from_index(10)

/* ------------------------------------------------------------------ chunk */

static int chunk_index(int x, int y, int z)
{
    return (y * CHUNK_SIZE_Z + z) * CHUNK_SIZE_X + x;
}

static int chunk_in_bounds(int x, int y, int z)
{
    return x >= 0 && x < CHUNK_SIZE_X &&
           y >= 0 && y < CHUNK_SIZE_Y &&
           z >= 0 && z < CHUNK_SIZE_Z;
}

void chunk_clear(Chunk *chunk)
{
    memset(chunk->blocks, BLOCK_AIR, sizeof(chunk->blocks));
}

uint8_t chunk_get(const Chunk *chunk, int x, int y, int z)
{
    if (!chunk_in_bounds(x, y, z))
        return BLOCK_AIR;
    return chunk->blocks[chunk_index(x, y, z)];
}

void chunk_set(Chunk *chunk, int x, int y, int z, uint8_t id)
{
    if (!chunk_in_bounds(x, y, z))
        return;
    chunk->blocks[chunk_index(x, y, z)] = id;
}

unsigned chunk_face_mask(const Chunk *chunk, int x, int y, int z)
{
    unsigned mask = 0;
    if (chunk_get(chunk, x + 1, y, z) == BLOCK_AIR) mask |= FACE_POS_X;
    if (chunk_get(chunk, x - 1, y, z) == BLOCK_AIR) mask |= FACE_NEG_X;
    if (chunk_get(chunk, x, y + 1, z) == BLOCK_AIR) mask |= FACE_POS_Y;
    if (chunk_get(chunk, x, y - 1, z) == BLOCK_AIR) mask |= FACE_NEG_Y;
    if (chunk_get(chunk, x, y, z + 1) == BLOCK_AIR) mask |= FACE_POS_Z;
    if (chunk_get(chunk, x, y, z - 1) == BLOCK_AIR) mask |= FACE_NEG_Z;
    return mask;
}

/* Shared tail of chunk_emit()/world_emit(): one solid block becomes up to six
 * faces of geometry. */
static size_t emit_block(TriangleBuffer *out, uint8_t id, unsigned mask,
                         Vec3 origin, int x, int y, int z,
                         Mat4 vp, Vec3 light_dir, const Viewport *view)
{
    const Block *block = block_from_id(id);
    if (!block)
        return 0;

    /* Blocks are axis-aligned, so the model matrix is a pure translation to the
     * centre of the voxel cell. */
    Mat4 model = mat4_translate(origin.x + (float)x + 0.5f,
                                origin.y + (float)y + 0.5f,
                                origin.z + (float)z + 0.5f);
    return cube_emit(out, block, model, vp, light_dir, mask, view);
}

size_t chunk_emit(TriangleBuffer *out, const Chunk *chunk, Vec3 origin,
                  Mat4 vp, Vec3 light_dir, const Viewport *view)
{
    size_t emitted = 0;

    for (int y = 0; y < CHUNK_SIZE_Y; y++) {
        for (int z = 0; z < CHUNK_SIZE_Z; z++) {
            for (int x = 0; x < CHUNK_SIZE_X; x++) {
                uint8_t id = chunk->blocks[chunk_index(x, y, z)];
                if (id == BLOCK_AIR)
                    continue;

                unsigned mask = chunk_face_mask(chunk, x, y, z);
                if (mask == 0)
                    continue; /* fully buried, nothing to draw */

                emitted += emit_block(out, id, mask, origin, x, y, z,
                                      vp, light_dir, view);
            }
        }
    }

    return emitted;
}

/* ------------------------------------------------------ terrain generation */

TerrainParams terrain_default(uint32_t seed)
{
    TerrainParams p;
    p.seed = seed;
    p.scale = 26.0f;
    p.octaves = 4;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.base_height = 5.0f;
    p.amplitude = 17.0f;
    p.sand_level = 8;
    p.snow_level = 18;
    p.tree_chance = 0.02f;
    return p;
}

int terrain_height(const TerrainParams *params, int world_x, int world_z)
{
    float n = noise_fbm_2d((float)world_x / params->scale,
                           (float)world_z / params->scale,
                           params->seed, params->octaves,
                           params->lacunarity, params->gain);

    /* Squaring biases the distribution towards low ground, which reads as
     * broad valleys with a few distinct peaks rather than uniform lumpiness. */
    n = n * n * (3.0f - 2.0f * n);

    int height = (int)(params->base_height + n * params->amplitude);
    if (height < 1) height = 1;
    if (height > CHUNK_SIZE_Y - 6) height = CHUNK_SIZE_Y - 6;
    return height;
}

/* Picks the block for one cell of a column, given the column's surface height. */
static uint8_t terrain_block(const TerrainParams *params, int y, int height,
                             int world_x, int world_z)
{
    int depth = height - 1 - y; /* 0 at the surface, growing downwards */

    if (depth == 0) {
        if (height >= params->snow_level) return ID_SNOW;
        if (height <= params->sand_level) return ID_SAND;
        return ID_GRASS;
    }

    if (depth <= 3) {
        if (height >= params->snow_level) return ID_STONE;
        if (height <= params->sand_level) return ID_SAND;
        return ID_DIRT;
    }

    /* Ore pockets scattered deterministically through the stone. */
    float r = noise_hash_3d(world_x, y, world_z, params->seed + 5501u);
    if (y < 6 && r > 0.988f) return ID_GOLD;
    if (y < 4 && r < 0.008f) return ID_DIAMOND;
    if (r > 0.965f && r <= 0.988f) return ID_COBBLE;
    return ID_STONE;
}

/* Small oak: a trunk with a two-layer canopy. Placed only on grass so trees
 * never sprout out of sand or snow. */
static void place_tree(Chunk *chunk, int x, int height, int z)
{
    int trunk = 4 + (height % 2);
    int top = height + trunk;
    if (top + 1 >= CHUNK_SIZE_Y)
        return;

    for (int y = height; y < top; y++)
        chunk_set(chunk, x, y, z, ID_LOG);

    for (int dy = -2; dy <= 0; dy++) {
        int radius = (dy == 0) ? 1 : 2;
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                /* Clip the corners so the canopy is round-ish, not a cube. */
                if (dx * dx + dz * dz > radius * radius + 1)
                    continue;
                if (dx == 0 && dz == 0 && dy < 0)
                    continue; /* leave room for the trunk */
                if (chunk_get(chunk, x + dx, top + dy, z + dz) == BLOCK_AIR)
                    chunk_set(chunk, x + dx, top + dy, z + dz, ID_LEAVES);
            }
        }
    }
    chunk_set(chunk, x, top, z, ID_LEAVES);
}

void chunk_generate(Chunk *chunk, const TerrainParams *params,
                    int chunk_x, int chunk_z)
{
    chunk_clear(chunk);

    for (int z = 0; z < CHUNK_SIZE_Z; z++) {
        for (int x = 0; x < CHUNK_SIZE_X; x++) {
            /* World coordinates, not chunk-local: this is the whole reason
             * adjacent chunks join up instead of each being its own island. */
            int world_x = chunk_x * CHUNK_SIZE_X + x;
            int world_z = chunk_z * CHUNK_SIZE_Z + z;
            int height = terrain_height(params, world_x, world_z);

            for (int y = 0; y < height; y++)
                chunk_set(chunk, x, y, z,
                          terrain_block(params, y, height, world_x, world_z));

            if (height > params->sand_level && height < params->snow_level &&
                noise_hash_3d(world_x, 777, world_z, params->seed + 91u) < params->tree_chance)
                place_tree(chunk, x, height, z);
        }
    }
}

/* ------------------------------------------------------------------- world */

int world_init(World *world, int size, const TerrainParams *params)
{
    if (size < 1) size = 1;
    world->size = size;
    world->params = *params;
    world->chunks = calloc((size_t)size * size, sizeof(Chunk));
    if (!world->chunks)
        return 0;

    /* Centre the world on the origin so the orbit camera needs no offset. */
    world->origin = vec3_make(-0.5f * (float)(size * CHUNK_SIZE_X),
                              -(params->base_height + params->amplitude * 0.45f),
                              -0.5f * (float)(size * CHUNK_SIZE_Z));
    return 1;
}

void world_free(World *world)
{
    free(world->chunks);
    world->chunks = NULL;
    world->size = 0;
}

void world_generate(World *world)
{
    for (int cz = 0; cz < world->size; cz++)
        for (int cx = 0; cx < world->size; cx++)
            chunk_generate(&world->chunks[cz * world->size + cx],
                           &world->params, cx, cz);
}

uint8_t world_get(const World *world, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_SIZE_Y)
        return BLOCK_AIR;

    int cx = x / CHUNK_SIZE_X;
    int cz = z / CHUNK_SIZE_Z;
    if (x < 0 || z < 0 || cx >= world->size || cz >= world->size)
        return BLOCK_AIR;

    const Chunk *chunk = &world->chunks[cz * world->size + cx];
    return chunk->blocks[chunk_index(x % CHUNK_SIZE_X, y, z % CHUNK_SIZE_Z)];
}

static unsigned world_face_mask(const World *world, int x, int y, int z)
{
    unsigned mask = 0;
    if (world_get(world, x + 1, y, z) == BLOCK_AIR) mask |= FACE_POS_X;
    if (world_get(world, x - 1, y, z) == BLOCK_AIR) mask |= FACE_NEG_X;
    if (world_get(world, x, y + 1, z) == BLOCK_AIR) mask |= FACE_POS_Y;
    if (world_get(world, x, y - 1, z) == BLOCK_AIR) mask |= FACE_NEG_Y;
    if (world_get(world, x, y, z + 1) == BLOCK_AIR) mask |= FACE_POS_Z;
    if (world_get(world, x, y, z - 1) == BLOCK_AIR) mask |= FACE_NEG_Z;
    return mask;
}

size_t world_emit(TriangleBuffer *out, const World *world, Mat4 vp,
                  Vec3 light_dir, const Viewport *view)
{
    size_t emitted = 0;
    int span_x = world->size * CHUNK_SIZE_X;
    int span_z = world->size * CHUNK_SIZE_Z;

    for (int y = 0; y < CHUNK_SIZE_Y; y++) {
        for (int z = 0; z < span_z; z++) {
            for (int x = 0; x < span_x; x++) {
                uint8_t id = world_get(world, x, y, z);
                if (id == BLOCK_AIR)
                    continue;

                unsigned mask = world_face_mask(world, x, y, z);
                if (mask == 0)
                    continue;

                emitted += emit_block(out, id, mask, world->origin, x, y, z,
                                      vp, light_dir, view);
            }
        }
    }

    return emitted;
}

size_t world_solid_blocks(const World *world)
{
    size_t solid = 0;
    size_t chunks = (size_t)world->size * world->size;
    for (size_t c = 0; c < chunks; c++)
        for (int i = 0; i < CHUNK_VOLUME; i++)
            if (world->chunks[c].blocks[i] != BLOCK_AIR)
                solid++;
    return solid;
}
