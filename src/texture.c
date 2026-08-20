#include "texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

static void put(Texture *t, int x, int y, int r, int g, int b)
{
    t->px[y * TEX_SIZE + x] = rgb(r, g, b);
}

static float smooth_t(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

/* Value noise over a lattice of `period` cells across the tile.
 *
 * The lattice indices wrap modulo `period`, which is what makes the result
 * SEAMLESS: the left edge of the tile interpolates towards the same lattice
 * values as the right edge, so blocks placed side by side show no seam. Plain
 * unwrapped noise would draw a visible grid across the terrain. */
static float tile_noise(int x, int y, int period, uint32_t seed)
{
    float fx = (float)x * (float)period / (float)TEX_SIZE;
    float fy = (float)y * (float)period / (float)TEX_SIZE;
    int x0 = (int)fx, y0 = (int)fy;
    float tx = smooth_t(fx - (float)x0);
    float ty = smooth_t(fy - (float)y0);

    int xa = x0 % period, xb = (x0 + 1) % period;
    int ya = y0 % period, yb = (y0 + 1) % period;

    float v00 = (float)(hash2(xa, ya, seed) & 0xFFFF) / 65535.0f;
    float v10 = (float)(hash2(xb, ya, seed) & 0xFFFF) / 65535.0f;
    float v01 = (float)(hash2(xa, yb, seed) & 0xFFFF) / 65535.0f;
    float v11 = (float)(hash2(xb, yb, seed) & 0xFFFF) / 65535.0f;

    float top = v00 + (v10 - v00) * tx;
    float bottom = v01 + (v11 - v01) * tx;
    return top + (bottom - top) * ty;
}

/* Grain plus broad blotches. Fine noise alone reads as television static; the
 * low-frequency layer is what makes a surface look like a material. */
static void fill_material(Texture *t, int r, int g, int b,
                          int grain, int blotch, int period, uint32_t seed)
{
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int fine = (int)(hash2(x, y, seed) % (uint32_t)(2 * grain + 1)) - grain;
            int broad = (int)((tile_noise(x, y, period, seed + 77u) - 0.5f) * 2.0f * (float)blotch);
            int d = fine + broad;
            put(t, x, y, r + d, g + d, b + d);
        }
    }
}

static void make_stone(Texture *t)
{
    fill_material(t, 126, 126, 128, 9, 16, 4, 11);
    /* A few darker cracks, drawn as short noise-guided streaks. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            float n = tile_noise(x, y, 6, 12u);
            if (n > 0.80f) {
                int d = (int)((n - 0.80f) * 300.0f);
                put(t, x, y, 126 - d, 126 - d, 128 - d);
            }
        }
    }
}

static void make_cobble(Texture *t)
{
    /* Irregular stones. The noise field is quantized into bands, and a pixel is
     * mortar when its band differs from a neighbour's. Comparing the gradient
     * instead marks far too much of the tile as edge, which buries the stones
     * under mortar and leaves the block almost black. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int here  = (int)(tile_noise(x, y, 5, 21u) * 9.0f);
            int right = (int)(tile_noise((x + 1) % TEX_SIZE, y, 5, 21u) * 9.0f);
            int below = (int)(tile_noise(x, (y + 1) % TEX_SIZE, 5, 21u) * 9.0f);

            int grain = (int)(hash2(x, y, 22) % 17) - 8;
            if (here != right || here != below) {
                put(t, x, y, 74 + grain, 74 + grain, 76 + grain);   /* mortar */
            } else {
                /* Each stone gets its own tone from its band index. */
                int shade = (int)(hash2(here, here * 3 + 1, 23) % 40) - 20;
                put(t, x, y, 140 + shade + grain, 140 + shade + grain, 142 + shade + grain);
            }
        }
    }
}

static void make_dirt(Texture *t)
{
    fill_material(t, 128, 90, 62, 10, 18, 4, 31);
    /* Pebbles and root flecks. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint32_t h = hash2(x, y, 32);
            if (h % 29 == 0)
                put(t, x, y, 152, 116, 84);
            else if (h % 37 == 0)
                put(t, x, y, 92, 62, 40);
        }
    }
}

static void make_grass_top(Texture *t)
{
    fill_material(t, 92, 148, 60, 11, 22, 5, 41);
    /* Vertical blade hints: brighten a pixel whose neighbour above is darker. */
    for (int y = 1; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            if (hash2(x, y, 42) % 6 == 0) {
                uint32_t c = t->px[y * TEX_SIZE + x];
                int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
                put(t, x, y, r + 16, g + 22, b + 10);
            }
        }
    }
}

static void make_grass_side(Texture *t)
{
    make_dirt(t);
    for (int x = 0; x < TEX_SIZE; x++) {
        /* Ragged fringe with an overhang, so the grass does not end on a line. */
        int depth = 5 + (int)(tile_noise(x, 0, 6, 51u) * 6.0f);
        for (int y = 0; y < depth; y++) {
            int d = (int)((tile_noise(x, y, 5, 52u) - 0.5f) * 40.0f);
            int fade = (y > depth - 3) ? -18 : 0; /* darker at the boundary */
            put(t, x, y, 92 + d + fade, 148 + d + fade, 60 + d + fade);
        }
    }
}

static void make_gold(Texture *t)
{
    /* 8x8 nuggets with a lit top-left bevel and a shadowed bottom-right. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int cx = x & 7, cy = y & 7;
            int shade = 0;
            if (cx <= 1 || cy <= 1) shade = 30;
            if (cx >= 6 || cy >= 6) shade = -40;
            if (cx == 0 && cy == 0) shade = 52;   /* specular corner */
            int n = (int)(hash2(x, y, 61) % 11) - 5;
            put(t, x, y, 244 + shade + n, 202 + shade + n, 66 + shade + n);
        }
    }
}

static void make_diamond_ore(Texture *t)
{
    make_stone(t);
    /* Crystals as small diamonds with a bright core and a dark rim. */
    static const int spots[][2] = {
        { 6, 8 }, { 20, 6 }, { 13, 20 }, { 25, 23 }, { 8, 26 }
    };
    for (int i = 0; i < 5; i++) {
        int ox = spots[i][0], oy = spots[i][1];
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int manhattan = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                if (manhattan > 2)
                    continue;
                int x = (ox + dx + TEX_SIZE) % TEX_SIZE;
                int y = (oy + dy + TEX_SIZE) % TEX_SIZE;
                if (manhattan == 2)
                    put(t, x, y, 46, 122, 130);
                else if (manhattan == 1)
                    put(t, x, y, 94, 214, 214);
                else
                    put(t, x, y, 172, 246, 244);
            }
        }
    }
}

static void make_log_side(Texture *t)
{
    fill_material(t, 104, 80, 48, 7, 12, 3, 71);
    /* Bark grooves: continuous vertical channels of varying width. */
    for (int x = 0; x < TEX_SIZE; x++) {
        float n = tile_noise(x, 4, 8, 72u);
        if (n < 0.42f) {
            int depth = (int)((0.42f - n) * 120.0f);
            for (int y = 0; y < TEX_SIZE; y++) {
                int wobble = (int)(hash2(x, y, 73) % 7);
                put(t, x, y, 104 - depth + wobble, 80 - depth + wobble, 48 - depth + wobble);
            }
        }
    }
}

static void make_log_top(Texture *t)
{
    fill_material(t, 174, 140, 86, 6, 10, 3, 81);
    /* Concentric growth rings, measured with a real radius so they are round. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            float dx = (float)x - (TEX_SIZE / 2) + 0.5f;
            float dy = (float)y - (TEX_SIZE / 2) + 0.5f;
            float r = sqrtf(dx * dx + dy * dy);
            /* Wobble the radius so the rings are not perfect circles. */
            r += (tile_noise(x, y, 4, 82u) - 0.5f) * 2.4f;
            float ring = sinf(r * 1.9f);
            if (ring > 0.55f) {
                int d = (int)((ring - 0.55f) * 90.0f);
                put(t, x, y, 174 - d, 140 - d, 86 - d);
            }
        }
    }
}

static void make_brick(Texture *t)
{
    const int course = 8, brick_w = 16;
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int row = y / course;
            int offset = (row & 1) ? brick_w / 2 : 0;
            int lx = (x + offset) % brick_w;
            int ly = y % course;

            if (ly < 2 || lx < 2) {
                int n = (int)(hash2(x, y, 91) % 9);
                put(t, x, y, 168 + n, 164 + n, 158 + n);   /* mortar */
            } else {
                /* Each brick gets its own tint, so the wall is not uniform. */
                int id = (x + offset) / brick_w + row * 7;
                int tint = (int)(hash2(id, row, 92) % 22) - 11;
                int n = (int)(hash2(x, y, 93) % 11) - 5;
                put(t, x, y, 150 + tint + n, 78 + tint + n, 60 + tint + n);
            }
        }
    }
}

static void make_sand(Texture *t)
{
    fill_material(t, 219, 205, 152, 7, 10, 5, 101);
    /* Fine wind ripples. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            float ripple = sinf((float)(x + y) * 0.9f + tile_noise(x, y, 4, 102u) * 5.0f);
            if (ripple > 0.7f) {
                uint32_t c = t->px[y * TEX_SIZE + x];
                int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
                put(t, x, y, r + 9, g + 9, b + 7);
            }
        }
    }
}

static void make_snow(Texture *t)
{
    fill_material(t, 240, 245, 249, 4, 7, 5, 111);
    /* Occasional sparkle, the one thing that stops snow reading as flat white. */
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++)
            if (hash2(x, y, 112) % 61 == 0)
                put(t, x, y, 255, 255, 255);
}

static void make_leaves(Texture *t)
{
    fill_material(t, 66, 128, 42, 14, 26, 6, 121);
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            float n = tile_noise(x, y, 7, 122u);
            if (n < 0.30f) {
                /* Gaps between leaves: dark, but not black, since the renderer
                 * is opaque and a true hole would read as a bug. */
                int d = (int)((0.30f - n) * 130.0f);
                put(t, x, y, 34 - d / 3, 62 - d / 3, 24 - d / 4);
            } else if (n > 0.74f) {
                int d = (int)((n - 0.74f) * 110.0f);
                put(t, x, y, 92 + d, 158 + d, 60 + d);   /* lit leaf */
            }
        }
    }
}

static void make_water(Texture *t)
{
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            /* Two crossing wave trains, the classic cheap water look. */
            float w = sinf((float)x * 0.55f + (float)y * 0.22f)
                    + 0.6f * sinf((float)y * 0.75f - (float)x * 0.18f);
            int d = (int)(w * 11.0f);
            int n = (int)(hash2(x, y, 131) % 7) - 3;
            put(t, x, y, 44 + d + n, 100 + d + n, 182 + d + n);
        }
    }
}

static void make_gravel(Texture *t)
{
    /* Pebbles of mixed size. The coarse field picks the pebble and gives it a
     * tone; a much finer field breaks up its interior. Both frequencies have to
     * be high or the result reads as poured concrete rather than loose stone. */
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int pebble = (int)(tile_noise(x, y, 9, 141u) * 14.0f);
            float fine = tile_noise(x, y, 16, 142u);

            int tone = (int)(hash2(pebble, pebble * 5 + 3, 143) % 70) - 35;
            int d = (int)((fine - 0.5f) * 34.0f);
            int n = (int)(hash2(x, y, 144) % 19) - 9;

            int base = 124 + tone + d + n;
            put(t, x, y, base, base - 3, base - 8);
        }
    }
}

static void make_dry_grass(Texture *t)
{
    fill_material(t, 158, 162, 88, 10, 20, 5, 151);
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            if (hash2(x, y, 152) % 8 == 0) {
                uint32_t c = t->px[y * TEX_SIZE + x];
                int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
                put(t, x, y, r + 14, g + 12, b - 4);
            }
        }
    }
}

/* Points every texture at its slice of one contiguous allocation. One malloc,
 * one free, and neighbouring textures share cache lines during rasterization. */
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

    make_stone(&g_textures[TEX_STONE]);
    make_cobble(&g_textures[TEX_COBBLE]);
    make_dirt(&g_textures[TEX_DIRT]);
    make_grass_top(&g_textures[TEX_GRASS_TOP]);
    make_grass_side(&g_textures[TEX_GRASS_SIDE]);
    make_gold(&g_textures[TEX_GOLD]);
    make_diamond_ore(&g_textures[TEX_DIAMOND_ORE]);
    make_log_side(&g_textures[TEX_LOG_SIDE]);
    make_log_top(&g_textures[TEX_LOG_TOP]);
    make_brick(&g_textures[TEX_BRICK]);
    make_sand(&g_textures[TEX_SAND]);
    make_snow(&g_textures[TEX_SNOW]);
    make_leaves(&g_textures[TEX_LEAVES]);
    make_water(&g_textures[TEX_WATER]);
    make_gravel(&g_textures[TEX_GRAVEL]);
    make_dry_grass(&g_textures[TEX_DRY_GRASS]);
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
