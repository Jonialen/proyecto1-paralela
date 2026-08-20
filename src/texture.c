#include "texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The procedural generators all draw on a 16x16 grid. A loaded pack may be
 * larger, but nothing in here needs to know that. */
#define TEX_SIZE TEX_PROCEDURAL_SIZE

static Texture g_textures[TEX_COUNT];
static uint32_t *g_pixels;   /* one allocation backing every texture */
static int g_resolution;

/* Deterministic per-pixel hash so every run produces the exact same block art
 * (important when comparing sequential vs parallel output pixel by pixel). */
static uint32_t hash2(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int clamp_channel(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)clamp_channel(r) << 16) |
           ((uint32_t)clamp_channel(g) << 8) |
            (uint32_t)clamp_channel(b);
}

/* Base colour plus symmetric per-pixel noise: the bread and butter of every
 * Minecraft-ish stone/dirt tile. */
static void fill_noise(Texture *t, int r, int g, int b, int amplitude, uint32_t seed)
{
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int n = (int)(hash2(x, y, seed) % (uint32_t)(2 * amplitude + 1)) - amplitude;
            t->px[y * TEX_SIZE + x] = rgb(r + n, g + n, b + n);
        }
    }
}

static void make_cobble(Texture *t)
{
    fill_noise(t, 122, 122, 122, 22, 11);
    /* Dark mortar lines carve the tile into irregular stones. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int cell_y = y / 5;
            int offset = (cell_y & 1) ? 3 : 0;
            int edge = ((x + offset) % 7 == 0) || (y % 5 == 0);
            if (edge) {
                int n = (int)(hash2(x, y, 12) % 12);
                t->px[y * TEX_SIZE + x] = rgb(66 + n, 66 + n, 66 + n);
            }
        }
    }
}

static void make_grass_side(Texture *t)
{
    fill_noise(t, 134, 96, 67, 18, 21); /* dirt base */
    for (int x = 0; x < TEX_SIZE; x++) {
        /* Ragged grass fringe so the green does not end on a straight line. */
        int depth = 3 + (int)(hash2(x, 0, 31) % 3);
        for (int y = 0; y < depth; y++) {
            int n = (int)(hash2(x, y, 32) % 25) - 12;
            t->px[y * TEX_SIZE + x] = rgb(93 + n, 156 + n, 59 + n);
        }
    }
}

static void make_gold(Texture *t)
{
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            /* 4x4 nugget grid: bright top-left bevel, dark bottom-right. */
            int cx = x & 3, cy = y & 3;
            int shade = 0;
            if (cx == 0 || cy == 0) shade = 26;
            if (cx == 3 || cy == 3) shade = -34;
            int n = (int)(hash2(x, y, 41) % 13) - 6;
            t->px[y * TEX_SIZE + x] = rgb(246 + shade + n, 206 + shade + n, 68 + shade + n);
        }
    }
}

static void make_diamond_ore(Texture *t)
{
    fill_noise(t, 122, 122, 122, 20, 11); /* same stone base as TEX_STONE */
    /* Hand-placed diamond clusters, drawn as a tiny bitmap. */
    static const int spots[][2] = {
        { 3, 4 }, { 4, 4 }, { 3, 5 }, { 4, 5 },
        { 9, 3 }, { 10, 3 }, { 9, 4 },
        { 6, 10 }, { 7, 10 }, { 6, 11 }, { 7, 11 }, { 8, 11 },
        { 12, 9 }, { 12, 10 }, { 13, 10 }
    };
    int count = (int)(sizeof(spots) / sizeof(spots[0]));
    for (int i = 0; i < count; i++) {
        int x = spots[i][0], y = spots[i][1];
        int n = (int)(hash2(x, y, 51) % 30) - 15;
        t->px[y * TEX_SIZE + x] = rgb(94 + n, 219 + n, 219 + n);
    }
}

static void make_log_side(Texture *t)
{
    fill_noise(t, 106, 84, 50, 12, 61);
    /* Vertical bark grooves. */
    for (int x = 0; x < TEX_SIZE; x++) {
        if (hash2(x, 7, 62) % 3 != 0)
            continue;
        for (int y = 0; y < TEX_SIZE; y++) {
            int n = (int)(hash2(x, y, 63) % 10);
            t->px[y * TEX_SIZE + x] = rgb(78 + n, 60 + n, 34 + n);
        }
    }
}

static void make_log_top(Texture *t)
{
    fill_noise(t, 176, 143, 86, 10, 71);
    /* Concentric growth rings measured with a cheap Chebyshev distance. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int dx = x - 8, dy = y - 8;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int ring = (adx > ady ? adx : ady);
            if (ring % 3 == 0) {
                int n = (int)(hash2(x, y, 72) % 10);
                t->px[y * TEX_SIZE + x] = rgb(133 + n, 104 + n, 58 + n);
            }
        }
    }
}

static void make_brick(Texture *t)
{
    fill_noise(t, 150, 84, 66, 12, 81);
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int row = y / 4;
            int offset = (row & 1) ? 4 : 0;
            int mortar = (y % 4 == 0) || ((x + offset) % 8 == 0);
            if (mortar)
                t->px[y * TEX_SIZE + x] = rgb(174, 170, 166);
        }
    }
}

static void make_leaves(Texture *t)
{
    fill_noise(t, 62, 124, 40, 26, 91);
    /* Scattered dark gaps read as depth between individual leaves. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            if (hash2(x, y, 92) % 5 == 0) {
                int n = (int)(hash2(x, y, 93) % 14);
                t->px[y * TEX_SIZE + x] = rgb(38 + n, 78 + n, 26 + n);
            }
        }
    }
}

static void make_water(Texture *t)
{
    /* Faint horizontal banding reads as a rippled surface from above. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int band = ((x + (y / 3) * 2) % 8 < 4) ? 8 : -8;
            int n = (int)(hash2(x, y, 121) % 11) - 5;
            t->px[y * TEX_SIZE + x] = rgb(46 + band + n, 96 + band + n, 178 + band + n);
        }
    }
}

/* Points every texture at its slice of one contiguous allocation. Keeping them
 * in a single block means one malloc, one free, and neighbouring textures share
 * cache lines during rasterization. */
static int textures_alloc(int size)
{
    size_t pixels = (size_t)TEX_COUNT * (size_t)size * (size_t)size;
    uint32_t *block = calloc(pixels, sizeof(uint32_t));
    if (!block)
        return 0;

    free(g_pixels);
    g_pixels = block;
    g_resolution = size;

    for (int i = 0; i < TEX_COUNT; i++) {
        g_textures[i].px = g_pixels + (size_t)i * size * size;
        g_textures[i].size = size;
        g_textures[i].mask = size - 1;
    }
    return 1;
}

void textures_free(void)
{
    free(g_pixels);
    g_pixels = NULL;
    g_resolution = 0;
    for (int i = 0; i < TEX_COUNT; i++)
        g_textures[i].px = NULL;
}

int textures_resolution(void)
{
    return g_resolution;
}

void textures_init(void)
{
    if (!textures_alloc(TEX_PROCEDURAL_SIZE)) {
        fprintf(stderr, "Error: out of memory allocating the texture set\n");
        exit(1);
    }

    fill_noise(&g_textures[TEX_STONE], 122, 122, 122, 20, 11);
    make_cobble(&g_textures[TEX_COBBLE]);
    fill_noise(&g_textures[TEX_DIRT], 134, 96, 67, 18, 21);
    fill_noise(&g_textures[TEX_GRASS_TOP], 93, 156, 59, 22, 33);
    make_grass_side(&g_textures[TEX_GRASS_SIDE]);
    make_gold(&g_textures[TEX_GOLD]);
    make_diamond_ore(&g_textures[TEX_DIAMOND_ORE]);
    make_log_side(&g_textures[TEX_LOG_SIDE]);
    make_log_top(&g_textures[TEX_LOG_TOP]);
    make_brick(&g_textures[TEX_BRICK]);
    fill_noise(&g_textures[TEX_SAND], 217, 205, 152, 14, 101);
    fill_noise(&g_textures[TEX_SNOW], 238, 244, 247, 9, 111);
    make_leaves(&g_textures[TEX_LEAVES]);
    make_water(&g_textures[TEX_WATER]);
    fill_noise(&g_textures[TEX_GRAVEL], 136, 132, 128, 30, 131);
    fill_noise(&g_textures[TEX_DRY_GRASS], 150, 156, 84, 20, 141);
}

/* Atlas file written by scripts/make_texture_atlas.py:
 *
 *   char     magic[4]  "VXTX"
 *   uint32_t version   1
 *   uint32_t count     number of textures, must match TEX_COUNT
 *   uint32_t size      edge length, power of two
 *   uint32_t pixels[count * size * size]   0x00RRGGBB, in TEX_* enum order
 *
 * A raw dump rather than PNG on purpose: decoding PNG would mean either a new
 * dependency or several thousand lines of third-party code in a project whose
 * brief asks for our own. The conversion happens offline instead. */
int textures_load_atlas(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Error: cannot open texture atlas '%s'\n", path);
        return 0;
    }

    char magic[4];
    uint32_t header[3];
    if (fread(magic, 1, 4, file) != 4 || fread(header, sizeof(uint32_t), 3, file) != 3) {
        fprintf(stderr, "Error: '%s' is too short to be a texture atlas\n", path);
        fclose(file);
        return 0;
    }
    if (memcmp(magic, "VXTX", 4) != 0 || header[0] != 1u) {
        fprintf(stderr, "Error: '%s' is not a version 1 texture atlas\n", path);
        fclose(file);
        return 0;
    }

    uint32_t count = header[1];
    uint32_t size = header[2];
    if (count != (uint32_t)TEX_COUNT) {
        fprintf(stderr, "Error: '%s' holds %u textures, this build expects %d\n",
                path, count, TEX_COUNT);
        fclose(file);
        return 0;
    }
    if (size < 8 || size > TEX_MAX_SIZE || (size & (size - 1)) != 0) {
        fprintf(stderr, "Error: '%s' has edge %u; expected a power of two from 8 to %d\n",
                path, size, TEX_MAX_SIZE);
        fclose(file);
        return 0;
    }

    /* Read into a scratch block first. Only once every pixel has arrived do the
     * live textures get replaced, so a truncated file leaves the procedural set
     * intact rather than half-overwritten. */
    size_t pixel_count = (size_t)count * size * size;
    uint32_t *scratch = malloc(pixel_count * sizeof(uint32_t));
    if (!scratch) {
        fprintf(stderr, "Error: out of memory reading '%s'\n", path);
        fclose(file);
        return 0;
    }
    if (fread(scratch, sizeof(uint32_t), pixel_count, file) != pixel_count) {
        fprintf(stderr, "Error: '%s' is truncated\n", path);
        free(scratch);
        fclose(file);
        return 0;
    }
    fclose(file);

    if (!textures_alloc((int)size)) {
        fprintf(stderr, "Error: out of memory resizing the texture set\n");
        free(scratch);
        return 0;
    }
    memcpy(g_pixels, scratch, pixel_count * sizeof(uint32_t));
    free(scratch);

    printf("Loaded texture pack: %s (%ux%u, %u textures)\n", path, size, size, count);
    return 1;
}

const Texture *texture_get(int id)
{
    if (id < 0 || id >= TEX_COUNT)
        id = TEX_STONE;
    return &g_textures[id];
}

static const Block g_blocks[] = {
    { "Grass Block", TEX_GRASS_TOP,   TEX_GRASS_SIDE,   TEX_DIRT     },
    { "Dirt",        TEX_DIRT,        TEX_DIRT,         TEX_DIRT     },
    { "Stone",       TEX_STONE,       TEX_STONE,        TEX_STONE    },
    { "Cobblestone", TEX_COBBLE,      TEX_COBBLE,       TEX_COBBLE   },
    { "Gold Block",  TEX_GOLD,        TEX_GOLD,         TEX_GOLD     },
    { "Diamond Ore", TEX_DIAMOND_ORE, TEX_DIAMOND_ORE,  TEX_DIAMOND_ORE },
    { "Oak Log",     TEX_LOG_TOP,     TEX_LOG_SIDE,     TEX_LOG_TOP  },
    { "Bricks",      TEX_BRICK,       TEX_BRICK,        TEX_BRICK    },
    { "Sand",        TEX_SAND,        TEX_SAND,         TEX_SAND     },
    { "Snow",        TEX_SNOW,        TEX_SNOW,         TEX_SNOW     },
    { "Leaves",      TEX_LEAVES,      TEX_LEAVES,       TEX_LEAVES   },
    { "Water",       TEX_WATER,       TEX_WATER,        TEX_WATER    },
    { "Gravel",      TEX_GRAVEL,      TEX_GRAVEL,       TEX_GRAVEL   },
    { "Dry Grass",   TEX_DRY_GRASS,   TEX_GRASS_SIDE,   TEX_DIRT     }
};

int block_count(void)
{
    return (int)(sizeof(g_blocks) / sizeof(g_blocks[0]));
}

const Block *block_get(int index)
{
    int n = block_count();
    if (index < 0) index = 0;
    if (index >= n) index = n - 1;
    return &g_blocks[index];
}

const Block *block_from_id(int id)
{
    if (id <= BLOCK_AIR || id > block_count())
        return NULL;
    return &g_blocks[id - 1];
}

int block_texture_for_face(const Block *block, int face)
{
    if (face == 2) return block->top;    /* +Y */
    if (face == 3) return block->bottom; /* -Y */
    return block->side;
}
