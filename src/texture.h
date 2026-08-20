#ifndef TEXTURE_H
#define TEXTURE_H

#include <stdint.h>
#include <stddef.h>

/* Block faces are square tiles. The built-in procedural set is 16x16, like a
 * classic Minecraft face, but a loaded texture pack may be 32, 64 or 128, so the
 * edge length lives in the struct rather than in a macro.
 *
 * `mask` is `size - 1`. Every real pack uses a power-of-two edge, which keeps
 * texture wrapping a single AND instead of a modulo in the innermost loop of
 * the rasterizer. */
#define TEX_PROCEDURAL_SIZE 32
#define TEX_MAX_SIZE 512

typedef struct {
    uint32_t *px;  /* size * size pixels, 0x00RRGGBB */
    int size;
    int mask;
} Texture;

enum {
    TEX_STONE,
    TEX_COBBLE,
    TEX_DIRT,
    TEX_GRASS_TOP,
    TEX_GRASS_SIDE,
    TEX_GOLD,
    TEX_DIAMOND_ORE,
    TEX_LOG_SIDE,
    TEX_LOG_TOP,
    TEX_BRICK,
    TEX_SAND,
    TEX_SNOW,
    TEX_LEAVES,
    TEX_WATER,
    TEX_GRAVEL,
    TEX_DRY_GRASS,
    TEX_COUNT
};

/* A block picks a different texture per face group, so a grass block can be
 * green on top, dirt underneath and half-and-half on the sides. */
typedef struct {
    const char *name;
    int top;
    int side;
    int bottom;
} Block;

/* Block ids used by the voxel grid: 0 is air, id N is block_get(N - 1). */
#define BLOCK_AIR 0

/* Builds the procedural set. Always call this first: it is the fallback when no
 * pack is given, and it guarantees every slot is valid even if a load fails
 * halfway. */
void textures_init(void);
void textures_free(void);

/* Replaces the procedural set with a pack atlas built by
 * scripts/make_texture_atlas.py. Returns 0 and leaves the procedural textures
 * untouched if the file is missing, malformed or the wrong size, so a bad pack
 * degrades to the built-in look instead of taking the program down. */
int textures_load_atlas(const char *path);

/* Edge length currently in use, for reporting. */
int textures_resolution(void);

const Texture *texture_get(int id);

int block_count(void);
const Block *block_get(int index);
/* Returns NULL for BLOCK_AIR. */
const Block *block_from_id(int id);
static inline int block_id_from_index(int index) { return index + 1; }

/* Face order matches cube_faces[] in render.c: +X, -X, +Y, -Y, +Z, -Z. */
int block_texture_for_face(const Block *block, int face);

static inline uint32_t texture_sample(const Texture *t, float u, float v)
{
    int tx = (int)(u * (float)t->size) & t->mask; /* wrap; size is a power of two */
    int ty = (int)(v * (float)t->size) & t->mask;
    return t->px[ty * t->size + tx];
}

#endif /* TEXTURE_H */
