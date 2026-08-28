#include "world.h"
#include "noise.h"

#ifdef _OPENMP
#include <omp.h>
#endif

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
#define ID_WATER   block_id_from_index(11)
#define ID_GRAVEL  block_id_from_index(12)
#define ID_DRYGRAS block_id_from_index(13)

/* Integer floor division and non-negative modulo. Plain / and % truncate
 * towards zero, which puts world x = -1 in chunk 0 instead of chunk -1 and
 * tears the terrain along both negative axes. */
static int floor_div(int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static int mod_floor(int a, int b)
{
    int m = a % b;
    if (m < 0)
        m += b;
    return m;
}

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
    if (!chunk || !chunk_in_bounds(x, y, z))
        return BLOCK_AIR;
    return chunk->blocks[chunk_index(x, y, z)];
}

void chunk_set(Chunk *chunk, int x, int y, int z, uint8_t id)
{
    if (!chunk_in_bounds(x, y, z))
        return;
    chunk->blocks[chunk_index(x, y, z)] = id;
}

/* ------------------------------------------------------ terrain generation */

TerrainParams terrain_default(uint32_t seed)
{
    TerrainParams p;
    p.seed = seed;
    p.scale = 40.0f;
    p.octaves = 4;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.base_height = 4.0f;
    p.amplitude = 20.0f;
    p.mountain_amplitude = 26.0f;
    p.warp_strength = 0.55f;
    p.sea_level = 10;
    p.beach_margin = 1;
    p.snow_line = 32;
    p.tree_chance = 0.05f;
    return p;
}

float terrain_peak(const TerrainParams *params)
{
    return params->base_height + params->amplitude + params->mountain_amplitude;
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float smoothstep01(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

TerrainSample terrain_sample(const TerrainParams *params, int world_x, int world_z)
{
    float nx = (float)world_x / params->scale;
    float nz = (float)world_z / params->scale;

    /* DOMAIN WARPING: displace the sample point using more noise before reading
     * the height field. Plain fBm produces round, obviously procedural blobs;
     * warping the input bends those contours into ridges, inlets and valleys
     * that look eroded. It costs two extra noise lookups and changes the whole
     * character of the terrain. */
    float warp_x = noise_fbm_2d(nx * 0.7f + 4.3f, nz * 0.7f - 2.1f,
                                params->seed + 7001u, 2, 2.0f, 0.5f) - 0.5f;
    float warp_z = noise_fbm_2d(nx * 0.7f - 1.9f, nz * 0.7f + 6.7f,
                                params->seed + 7002u, 2, 2.0f, 0.5f) - 0.5f;
    nx += warp_x * params->warp_strength;
    nz += warp_z * params->warp_strength;

    /* Base relief: ordinary rolling terrain. Smoothstep is symmetric -- it
     * flattens both extremes and steepens the middle where fBm clusters, which
     * increases contrast between valleys and ridges. */
    float base = smoothstep01(noise_fbm_2d(nx, nz, params->seed,
                                           params->octaves, params->lacunarity,
                                           params->gain));

    /* Mountain mask: a separate low-frequency field decides WHERE mountains are
     * allowed at all, so ranges are localized instead of spread evenly. Only
     * the top of its range counts, remapped so the transition is gradual. */
    float mask = noise_fbm_2d(nx * 0.32f + 17.0f, nz * 0.32f - 11.0f,
                              params->seed + 3301u, 2, 2.0f, 0.5f);
    mask = smoothstep01(clamp01((mask - 0.52f) * 2.6f));

    /* Ridged noise gives crests rather than domes. */
    float ridge = noise_ridged_2d(nx * 1.15f, nz * 1.15f,
                                  params->seed + 4401u, 4, 2.0f, 0.5f);

    /* The mountain term is gated by the mask but NOT purely multiplied by the
     * ridge. A plain mask*ridge product is high only when both fields happen to
     * peak together, which is rare, so mountains ended up existing on paper and
     * never on screen. Keeping a floor under the ridge means that wherever the
     * mask says "mountains here", there is real elevation; the ridge then
     * decides whether it is a crest or a shoulder. */
    float relief = 0.35f + 0.65f * ridge;
    float h = params->base_height
            + base * params->amplitude
            + mask * relief * params->mountain_amplitude;

    TerrainSample sample;
    sample.height = (int)h;
    if (sample.height < 1) sample.height = 1;
    if (sample.height > CHUNK_SIZE_Y - 10) sample.height = CHUNK_SIZE_Y - 10;

    /* Climate fields, at a much lower frequency than the relief so a biome
     * spans many chunks instead of flickering block to block. */
    float temperature = noise_fbm_2d(nx * 0.16f + 31.0f, nz * 0.16f + 13.0f,
                                     params->seed + 8801u, 2, 2.0f, 0.5f);
    float humidity = noise_fbm_2d(nx * 0.19f - 23.0f, nz * 0.19f + 29.0f,
                                  params->seed + 8802u, 2, 2.0f, 0.5f);

    /* Lapse rate: temperature falls with altitude. Climate fields sampled on
     * their own are independent of the relief, which put snow at sea level
     * right beside a desert. Coupling them is both physically right and the fix
     * -- cold ends up on the heights, deserts in the warm lowlands. */
    temperature -= (float)(sample.height - params->sea_level) * 0.008f;

    /* Height wins over climate: an ocean is an ocean whatever the weather. */
    if (sample.height <= params->sea_level)
        sample.biome = BIOME_OCEAN;
    else if (sample.height <= params->sea_level + params->beach_margin)
        sample.biome = BIOME_BEACH;
    /* Mountain needs actual altitude, not just a high mask. Deciding by mask
     * alone painted bare stone at sea level wherever the mask happened to be
     * high but the terrain was not. */
    else if (sample.height >= params->snow_line ||
             (mask > 0.5f && sample.height > params->sea_level + 12))
        sample.biome = BIOME_MOUNTAIN;
    else if (temperature > 0.60f && humidity < 0.42f)
        sample.biome = BIOME_DESERT;
    else if (temperature < 0.34f)
        sample.biome = BIOME_TUNDRA;
    else if (humidity > 0.54f)
        sample.biome = BIOME_FOREST;
    else
        sample.biome = BIOME_PLAINS;

    return sample;
}

int terrain_height(const TerrainParams *params, int world_x, int world_z)
{
    return terrain_sample(params, world_x, world_z).height;
}

/* Ore scattering, shared by every biome: only the surface layers differ. */
static uint8_t stone_or_ore(const TerrainParams *params, int y,
                            int world_x, int world_z)
{
    float r = noise_hash_3d(world_x, y, world_z, params->seed + 5501u);
    if (y < 8 && r > 0.990f) return ID_GOLD;
    if (y < 5 && r < 0.007f) return ID_DIAMOND;
    if (r > 0.968f && r <= 0.990f) return ID_COBBLE;
    return ID_STONE;
}

/* Surface palette for one column, by biome. `depth` is 0 at the surface and
 * grows downwards. */
static uint8_t terrain_block(const TerrainParams *params, const TerrainSample *sample,
                             int y, int world_x, int world_z)
{
    int depth = sample->height - 1 - y;

    switch (sample->biome) {
    case BIOME_OCEAN:
        if (depth <= 2) return (y % 3 == 0) ? ID_GRAVEL : ID_SAND;
        break;
    case BIOME_BEACH:
        if (depth <= 3) return ID_SAND;
        break;
    case BIOME_DESERT:
        if (depth <= 4) return ID_SAND;
        break;
    case BIOME_TUNDRA:
        if (depth == 0) return ID_SNOW;
        if (depth <= 3) return ID_DIRT;
        break;
    case BIOME_MOUNTAIN:
        if (sample->height >= params->snow_line) {
            if (depth <= 1) return ID_SNOW;
            if (depth <= 4) return ID_STONE;
        } else {
            if (depth == 0) return (y % 5 == 0) ? ID_GRAVEL : ID_STONE;
            if (depth <= 3) return ID_STONE;
        }
        break;
    case BIOME_PLAINS:
        if (depth == 0) return ID_DRYGRAS;
        if (depth <= 3) return ID_DIRT;
        break;
    case BIOME_FOREST:
    default:
        if (depth == 0) return ID_GRASS;
        if (depth <= 3) return ID_DIRT;
        break;
    }

    return stone_or_ore(params, y, world_x, world_z);
}

/* How often a column of this biome grows a tree. */
static float biome_tree_chance(const TerrainParams *params, Biome biome)
{
    switch (biome) {
    case BIOME_FOREST: return params->tree_chance * 3.0f;
    case BIOME_PLAINS: return params->tree_chance * 0.5f;
    case BIOME_TUNDRA: return params->tree_chance * 0.25f;
    default:           return 0.0f; /* no trees on sand, rock, snow or water */
    }
}

/* Small oak: a trunk with a two-layer canopy. Canopy blocks that fall outside
 * the chunk are clipped by chunk_set, so trees on a chunk seam lose part of
 * their crown -- the usual cost of generating chunks independently. */
static void place_tree(Chunk *chunk, int x, int height, int z)
{
    int trunk = 4 + (height % 3);
    int top = height + trunk;
    if (top + 1 >= CHUNK_SIZE_Y)
        return;

    for (int y = height; y < top; y++)
        chunk_set(chunk, x, y, z, ID_LOG);

    for (int dy = -2; dy <= 0; dy++) {
        int radius = (dy == 0) ? 1 : 2;
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                if (dx * dx + dz * dz > radius * radius + 1)
                    continue;
                if (dx == 0 && dz == 0 && dy < 0)
                    continue;
                if (chunk_get(chunk, x + dx, top + dy, z + dz) == BLOCK_AIR)
                    chunk_set(chunk, x + dx, top + dy, z + dz, ID_LEAVES);
            }
        }
    }
    chunk_set(chunk, x, top, z, ID_LEAVES);
}

/* Scans down for the highest non-air layer, so emission can skip empty sky. */
static void chunk_update_height_limit(Chunk *chunk)
{
    for (int y = CHUNK_SIZE_Y - 1; y >= 0; y--) {
        for (int i = 0; i < CHUNK_SIZE_Z * CHUNK_SIZE_X; i++) {
            if (chunk->blocks[y * CHUNK_SIZE_Z * CHUNK_SIZE_X + i] != BLOCK_AIR) {
                chunk->height_limit = (uint16_t)(y + 1);
                return;
            }
        }
    }
    chunk->height_limit = 0;
}

void chunk_generate(Chunk *chunk, const TerrainParams *params,
                    int chunk_x, int chunk_z)
{
    chunk_clear(chunk);

    for (int z = 0; z < CHUNK_SIZE_Z; z++) {
        for (int x = 0; x < CHUNK_SIZE_X; x++) {
            /* World coordinates, not chunk-local: this is what makes a chunk
             * generated on its own line up with neighbours generated later. */
            int world_x = chunk_x * CHUNK_SIZE_X + x;
            int world_z = chunk_z * CHUNK_SIZE_Z + z;
            TerrainSample sample = terrain_sample(params, world_x, world_z);

            for (int y = 0; y < sample.height; y++)
                chunk_set(chunk, x, y, z,
                          terrain_block(params, &sample, y, world_x, world_z));

            /* Flood everything below sea level that the ground did not fill. */
            for (int y = sample.height; y <= params->sea_level; y++)
                chunk_set(chunk, x, y, z, ID_WATER);

            float chance = biome_tree_chance(params, sample.biome);
            if (chance > 0.0f &&
                noise_hash_3d(world_x, 777, world_z, params->seed + 91u) < chance)
                place_tree(chunk, x, sample.height, z);
        }
    }

    chunk_update_height_limit(chunk);
}

/* --------------------------------------------------------------- chunk map */

static size_t chunk_hash(int cx, int cz)
{
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u;
    h = (h ^ (h >> 15)) * 2246822519u;
    return (size_t)(h ^ (h >> 13));
}

int world_init(World *world, const TerrainParams *params, size_t max_chunks)
{
    memset(world, 0, sizeof(*world));
    world->capacity = 1024; /* power of two, grows as needed */
    world->slots = calloc(world->capacity, sizeof(ChunkSlot));
    if (!world->slots)
        return 0;

    world->params = *params;
    world->max_chunks = max_chunks ? max_chunks : WORLD_DEFAULT_MAX_CHUNKS;
    world->frame = 1;
    return 1;
}

void world_free(World *world)
{
    if (world->slots) {
        for (size_t i = 0; i < world->capacity; i++)
            if (world->slots[i].state == CHUNK_SLOT_USED)
                free(world->slots[i].chunk);
        free(world->slots);
    }
    memset(world, 0, sizeof(*world));
}

size_t world_chunk_count(const World *world)
{
    return world->used;
}

/* Returns the slot holding (cx, cz), or NULL. */
static ChunkSlot *world_lookup(const World *world, int cx, int cz)
{
    size_t mask = world->capacity - 1;
    size_t i = chunk_hash(cx, cz) & mask;

    for (size_t probe = 0; probe < world->capacity; probe++) {
        ChunkSlot *slot = &world->slots[i];
        if (slot->state == CHUNK_SLOT_EMPTY)
            return NULL; /* a never-used slot ends the probe chain */
        if (slot->state == CHUNK_SLOT_USED && slot->cx == cx && slot->cz == cz)
            return slot;
        i = (i + 1) & mask; /* linear probing */
    }
    return NULL;
}

const Chunk *world_find_chunk(const World *world, int cx, int cz)
{
    ChunkSlot *slot = world_lookup(world, cx, cz);
    return slot ? slot->chunk : NULL;
}

/* Reinserts every live chunk into a fresh table, clearing tombstones. */
static int world_rehash(World *world, size_t new_capacity)
{
    ChunkSlot *fresh = calloc(new_capacity, sizeof(ChunkSlot));
    if (!fresh)
        return 0;

    size_t mask = new_capacity - 1;
    for (size_t i = 0; i < world->capacity; i++) {
        ChunkSlot *old = &world->slots[i];
        if (old->state != CHUNK_SLOT_USED)
            continue;
        size_t j = chunk_hash(old->cx, old->cz) & mask;
        while (fresh[j].state == CHUNK_SLOT_USED)
            j = (j + 1) & mask;
        fresh[j] = *old;
    }

    free(world->slots);
    world->slots = fresh;
    world->capacity = new_capacity;
    world->tombstones = 0;
    return 1;
}

/* Inserts a freshly generated chunk. Takes ownership of `chunk`. */
static int world_insert(World *world, int cx, int cz, Chunk *chunk)
{
    /* Keep the load factor under 0.7; tombstones count because they lengthen
     * probe chains just as much as live entries do. */
    if ((world->used + world->tombstones + 1) * 10 >= world->capacity * 7) {
        if (!world_rehash(world, world->capacity * 2))
            return 0;
    }

    size_t mask = world->capacity - 1;
    size_t i = chunk_hash(cx, cz) & mask;
    while (world->slots[i].state == CHUNK_SLOT_USED)
        i = (i + 1) & mask;

    if (world->slots[i].state == CHUNK_SLOT_DEAD)
        world->tombstones--;

    world->slots[i].cx = cx;
    world->slots[i].cz = cz;
    world->slots[i].state = CHUNK_SLOT_USED;
    world->slots[i].touch = world->frame;
    world->slots[i].chunk = chunk;
    world->used++;
    return 1;
}

static void world_drop_slot(World *world, ChunkSlot *slot)
{
    free(slot->chunk);
    slot->chunk = NULL;
    slot->state = CHUNK_SLOT_DEAD;
    world->used--;
    world->tombstones++;
}

uint8_t world_get(const World *world, int x, int y, int z)
{
    if (y < 0 || y >= CHUNK_SIZE_Y)
        return BLOCK_AIR;

    const Chunk *chunk = world_find_chunk(world,
                                          floor_div(x, CHUNK_SIZE_X),
                                          floor_div(z, CHUNK_SIZE_Z));
    if (!chunk)
        return BLOCK_AIR;

    return chunk->blocks[chunk_index(mod_floor(x, CHUNK_SIZE_X), y,
                                     mod_floor(z, CHUNK_SIZE_Z))];
}

/* --------------------------------------------------------------- streaming */

void world_begin_frame(World *world)
{
    world->frame++;
    world->generated_this_frame = 0;
    world->evicted_this_frame = 0;
}

size_t world_stream_around(World *world, Vec3 center, float generate_radius)
{
    if (generate_radius <= 0.0f)
        return 0;

    int center_cx = floor_div((int)floorf(center.x), CHUNK_SIZE_X);
    int center_cz = floor_div((int)floorf(center.z), CHUNK_SIZE_Z);
    int reach = (int)ceilf(generate_radius / (float)CHUNK_SIZE_X);
    float radius_sq = generate_radius * generate_radius;

    size_t generated = 0;
    for (int dz = -reach; dz <= reach; dz++) {
        for (int dx = -reach; dx <= reach; dx++) {
            int cx = center_cx + dx;
            int cz = center_cz + dz;

            /* Disk, not square: the corners of the square are up to 41% further
             * out and would cost 27% more chunks for terrain nobody reaches. */
            float ox = ((float)cx + 0.5f) * CHUNK_SIZE_X - center.x;
            float oz = ((float)cz + 0.5f) * CHUNK_SIZE_Z - center.z;
            if (ox * ox + oz * oz > radius_sq)
                continue;

            ChunkSlot *slot = world_lookup(world, cx, cz);
            if (slot) {
                slot->touch = world->frame; /* still needed, keep it alive */
                continue;
            }

            Chunk *chunk = malloc(sizeof(Chunk));
            if (!chunk)
                return generated; /* out of memory: stop growing, keep running */

            chunk_generate(chunk, &world->params, cx, cz);
            if (!world_insert(world, cx, cz, chunk)) {
                free(chunk);
                return generated;
            }
            generated++;
        }
    }

    world->generated_this_frame += generated;
    return generated;
}

size_t world_end_frame(World *world)
{
    size_t evicted = 0;

    /* Grace period: a chunk right at the generation boundary would otherwise
     * be dropped and regenerated on alternating frames as an explorer skims
     * past it. */
    for (size_t i = 0; i < world->capacity; i++) {
        ChunkSlot *slot = &world->slots[i];
        if (slot->state != CHUNK_SLOT_USED)
            continue;
        if (slot->touch + WORLD_EVICT_GRACE_FRAMES < world->frame) {
            world_drop_slot(world, slot);
            evicted++;
        }
    }

    /* Hard ceiling, so a fast explorer cannot grow the resident set without
     * bound before the grace period expires. Drops the least recently needed. */
    while (world->used > world->max_chunks) {
        ChunkSlot *oldest = NULL;
        for (size_t i = 0; i < world->capacity; i++) {
            ChunkSlot *slot = &world->slots[i];
            if (slot->state != CHUNK_SLOT_USED)
                continue;
            if (slot->touch == world->frame)
                continue; /* needed right now, never evict */
            if (!oldest || slot->touch < oldest->touch)
                oldest = slot;
        }
        if (!oldest)
            break; /* everything resident is in use this frame */
        world_drop_slot(world, oldest);
        evicted++;
    }

    world->evicted_this_frame = evicted;
    return evicted;
}

/* --------------------------------------------------------------- rendering */

/* The four horizontal neighbours of one chunk, resolved once so face masking
 * does not pay a hash lookup per block per side. */
typedef struct {
    const Chunk *neg_x, *pos_x, *neg_z, *pos_z;
} ChunkNeighbors;

/* Faces whose neighbour is air. A missing neighbour chunk counts as SOLID: it
 * only happens outside the generation radius, and treating it as air would
 * draw a wall of faces along the edge of the loaded region. */
static unsigned face_mask_at(const Chunk *chunk, const ChunkNeighbors *n,
                             int x, int y, int z)
{
    unsigned mask = 0;

    if (x + 1 < CHUNK_SIZE_X) {
        if (chunk->blocks[chunk_index(x + 1, y, z)] == BLOCK_AIR) mask |= FACE_POS_X;
    } else if (n->pos_x && chunk_get(n->pos_x, 0, y, z) == BLOCK_AIR) {
        mask |= FACE_POS_X;
    }

    if (x - 1 >= 0) {
        if (chunk->blocks[chunk_index(x - 1, y, z)] == BLOCK_AIR) mask |= FACE_NEG_X;
    } else if (n->neg_x && chunk_get(n->neg_x, CHUNK_SIZE_X - 1, y, z) == BLOCK_AIR) {
        mask |= FACE_NEG_X;
    }

    if (z + 1 < CHUNK_SIZE_Z) {
        if (chunk->blocks[chunk_index(x, y, z + 1)] == BLOCK_AIR) mask |= FACE_POS_Z;
    } else if (n->pos_z && chunk_get(n->pos_z, x, y, 0) == BLOCK_AIR) {
        mask |= FACE_POS_Z;
    }

    if (z - 1 >= 0) {
        if (chunk->blocks[chunk_index(x, y, z - 1)] == BLOCK_AIR) mask |= FACE_NEG_Z;
    } else if (n->neg_z && chunk_get(n->neg_z, x, y, CHUNK_SIZE_Z - 1) == BLOCK_AIR) {
        mask |= FACE_NEG_Z;
    }

    /* Y has no neighbouring chunk: above the top and below the bottom is air. */
    if (y + 1 >= CHUNK_SIZE_Y || chunk->blocks[chunk_index(x, y + 1, z)] == BLOCK_AIR)
        mask |= FACE_POS_Y;
    if (y - 1 < 0 || chunk->blocks[chunk_index(x, y - 1, z)] == BLOCK_AIR)
        mask |= FACE_NEG_Y;

    return mask;
}

/* Packs the 3x3x3 neighbourhood of one block into 27 bits, indexed
 * (dx+1)*9 + (dy+1)*3 + (dz+1). Ambient occlusion needs the diagonals, which
 * the six-way face mask never looks at. Only built for blocks that actually
 * show a face, so fully buried blocks still cost nothing. */
static uint32_t neighbourhood_mask(const Chunk *chunk, const ChunkNeighbors *n,
                                   int x, int y, int z)
{
    uint32_t mask = 0;

    /* Fast path for a block whose whole neighbourhood lies inside this chunk,
     * which is the overwhelming majority. Straight indexing with no bounds
     * checks and no cross-chunk lookups; the generic path below only runs on
     * the shell. This matters because the mask is built for every visible
     * block and the branch-heavy version doubled the geometry stage. */
    if (x > 0 && x < CHUNK_SIZE_X - 1 &&
        y > 0 && y < CHUNK_SIZE_Y - 1 &&
        z > 0 && z < CHUNK_SIZE_Z - 1) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                const uint8_t *row = &chunk->blocks[chunk_index(x - 1, y + dy, z + dz)];
                for (int dx = 0; dx < 3; dx++)
                    if (row[dx] != BLOCK_AIR)
                        mask |= 1u << (dx * 9 + (dy + 1) * 3 + (dz + 1));
            }
        }
        return mask;
    }

    for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                int bx = x + dx, by = y + dy, bz = z + dz;
                uint8_t id;

                if (by < 0 || by >= CHUNK_SIZE_Y) {
                    id = BLOCK_AIR;
                } else if (bx >= 0 && bx < CHUNK_SIZE_X &&
                           bz >= 0 && bz < CHUNK_SIZE_Z) {
                    id = chunk->blocks[chunk_index(bx, by, bz)];
                } else if (bx < 0) {
                    id = chunk_get(n->neg_x, CHUNK_SIZE_X - 1, by,
                                   bz < 0 ? CHUNK_SIZE_Z - 1 : (bz >= CHUNK_SIZE_Z ? 0 : bz));
                } else if (bx >= CHUNK_SIZE_X) {
                    id = chunk_get(n->pos_x, 0, by,
                                   bz < 0 ? CHUNK_SIZE_Z - 1 : (bz >= CHUNK_SIZE_Z ? 0 : bz));
                } else if (bz < 0) {
                    id = chunk_get(n->neg_z, bx, by, CHUNK_SIZE_Z - 1);
                } else {
                    id = chunk_get(n->pos_z, bx, by, 0);
                }

                if (id != BLOCK_AIR)
                    mask |= 1u << ((dx + 1) * 9 + (dy + 1) * 3 + (dz + 1));
            }
        }
    }
    return mask;
}

/* True when the chunk cannot appear in the view at all.
 *
 * The chunk is bounded by a sphere sized from its own occupied height rather
 * than the full 64 blocks, which keeps the test tight enough to be worth
 * running: a chunk holding only 20 blocks of terrain gets a far smaller sphere
 * than the chunk struct's nominal extent would suggest.
 *
 * Conservative on purpose. A chunk is dropped only when the whole sphere lies
 * beyond one side plane, so the test never removes anything visible. */
static int chunk_outside_frustum(const ViewFrustum *frustum, const Chunk *chunk,
                                 float base_x, float base_z)
{
    float half_y = (float)chunk->height_limit * 0.5f;
    float dx = base_x + CHUNK_SIZE_X * 0.5f - frustum->eye.x;
    float dy = half_y - frustum->eye.y;
    float dz = base_z + CHUNK_SIZE_Z * 0.5f - frustum->eye.z;

    float radius = sqrtf((float)(CHUNK_SIZE_X * CHUNK_SIZE_X) * 0.25f +
                         half_y * half_y +
                         (float)(CHUNK_SIZE_Z * CHUNK_SIZE_Z) * 0.25f);

    float f = dx * frustum->forward.x + dy * frustum->forward.y + dz * frustum->forward.z;
    float r = dx * frustum->right.x + dy * frustum->right.y + dz * frustum->right.z;

    float tan_half = frustum->tan_half_h;
    float scale = 1.0f / sqrtf(1.0f + tan_half * tan_half);

    /* Signed distance to each side plane, positive on the outside. */
    if ((r - tan_half * f) * scale > radius)
        return 1;
    if ((-r - tan_half * f) * scale > radius)
        return 1;
    return 0;
}

/* Emits one row of chunks. Split out so the row loop can run either serially or
 * in parallel without duplicating the body. */
static size_t emit_chunk_row(TriangleBuffer *out, const World *world, int cz,
                             int center_cx, int reach, Vec3 camera_pos,
                             float radius_sq, Mat4 vp, const Light *light,
                             const Viewport *view, const ViewFrustum *frustum)
{
    size_t emitted = 0;

    for (int cx = center_cx - reach; cx <= center_cx + reach; cx++) {
        float ox = ((float)cx + 0.5f) * CHUNK_SIZE_X - camera_pos.x;
        float oz = ((float)cz + 0.5f) * CHUNK_SIZE_Z - camera_pos.z;
        if (ox * ox + oz * oz > radius_sq)
            continue;

        const Chunk *chunk = world_find_chunk(world, cx, cz);
        if (!chunk)
            continue;

        float base_x = (float)cx * CHUNK_SIZE_X;
        float base_z = (float)cz * CHUNK_SIZE_Z;

        if (frustum && chunk_outside_frustum(frustum, chunk, base_x, base_z))
            continue;

        ChunkNeighbors neighbors = {
            world_find_chunk(world, cx - 1, cz),
            world_find_chunk(world, cx + 1, cz),
            world_find_chunk(world, cx, cz - 1),
            world_find_chunk(world, cx, cz + 1)
        };

        int y_limit = (int)chunk->height_limit;
        for (int y = 0; y < y_limit; y++) {
            for (int z = 0; z < CHUNK_SIZE_Z; z++) {
                for (int x = 0; x < CHUNK_SIZE_X; x++) {
                    uint8_t id = chunk->blocks[chunk_index(x, y, z)];
                    if (id == BLOCK_AIR)
                        continue;

                    unsigned mask = face_mask_at(chunk, &neighbors, x, y, z);
                    if (mask == 0)
                        continue; /* fully buried */

                    const Block *block = block_from_id(id);
                    if (!block)
                        continue;

                    /* Ambient occlusion needs the diagonals, which the six-way
                     * face mask never looks at. Only built for blocks that
                     * actually show a face. */
                    uint32_t occlusion = neighbourhood_mask(chunk, &neighbors, x, y, z);

                    Mat4 model = mat4_translate(base_x + (float)x + 0.5f,
                                                (float)y + 0.5f,
                                                base_z + (float)z + 0.5f);
                    emitted += cube_emit(out, block, model, vp, light,
                                         mask, occlusion, view);
                }
            }
        }
    }

    return emitted;
}

int world_row_count(float render_radius)
{
    int reach = (int)ceilf(render_radius / (float)CHUNK_SIZE_X);
    return 2 * reach + 1;
}

size_t world_emit_row(TriangleBuffer *out, const World *world, Vec3 camera_pos,
                      float render_radius, Mat4 vp, const Light *light,
                      const Viewport *view, const ViewFrustum *frustum,
                      int row_index)
{
    int center_cx = floor_div((int)floorf(camera_pos.x), CHUNK_SIZE_X);
    int center_cz = floor_div((int)floorf(camera_pos.z), CHUNK_SIZE_Z);
    int reach = (int)ceilf(render_radius / (float)CHUNK_SIZE_X);

    return emit_chunk_row(out, world, center_cz - reach + row_index,
                          center_cx, reach, camera_pos,
                          render_radius * render_radius,
                          vp, light, view, frustum);
}

size_t world_emit_view(TriangleBuffer *out, const World *world, Vec3 camera_pos,
                       float render_radius, Mat4 vp, const Light *light,
                       const Viewport *view, const ViewFrustum *frustum,
                       TriangleBuffer *rows, int row_capacity)
{
    int center_cx = floor_div((int)floorf(camera_pos.x), CHUNK_SIZE_X);
    int center_cz = floor_div((int)floorf(camera_pos.z), CHUNK_SIZE_Z);
    int reach = (int)ceilf(render_radius / (float)CHUNK_SIZE_X);
    float radius_sq = render_radius * render_radius;
    int row_count = 2 * reach + 1;

    /* Serial path: one buffer, rows visited in order. */
    if (!rows || row_capacity < row_count) {
        size_t emitted = 0;
        for (int r = 0; r < row_count; r++)
            emitted += emit_chunk_row(out, world, center_cz - reach + r,
                                      center_cx, reach, camera_pos, radius_sq,
                                      vp, light, view, frustum);
        return emitted;
    }

    /* Parallel path. Each row fills its OWN buffer, so no two workers touch the
     * same memory. Rows are then stitched back in row order, which is exactly
     * the order the serial scan produces -- that is what keeps the output
     * identical whatever the thread count, and it is why the split is by row
     * rather than by an arbitrary partition of the chunks. */
    size_t emitted = 0;
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) reduction(+:emitted)
#endif
    for (int r = 0; r < row_count; r++) {
        tribuf_clear(&rows[r]);
        emitted += emit_chunk_row(&rows[r], world, center_cz - reach + r,
                                  center_cx, reach, camera_pos, radius_sq,
                                  vp, light, view, frustum);
    }

    for (int r = 0; r < row_count; r++)
        tribuf_append(out, &rows[r]);

    return emitted;
}

size_t world_solid_blocks(const World *world)
{
    size_t solid = 0;
    for (size_t i = 0; i < world->capacity; i++) {
        if (world->slots[i].state != CHUNK_SLOT_USED)
            continue;
        const Chunk *chunk = world->slots[i].chunk;
        for (int j = 0; j < CHUNK_VOLUME; j++)
            if (chunk->blocks[j] != BLOCK_AIR)
                solid++;
    }
    return solid;
}
