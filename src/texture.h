#ifndef TEXTURE_H
#define TEXTURE_H

#include <stdint.h>
#include <stddef.h>

/* Every texture is a 16x16 tile, exactly like a classic Minecraft block face.
 * Nearest-neighbour sampling is what gives the chunky pixel-art look. */
#define TEX_SIZE 16

typedef struct {
    uint32_t px[TEX_SIZE * TEX_SIZE]; /* 0x00RRGGBB */
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

void textures_init(void);
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
    int tx = (int)(u * TEX_SIZE);
    int ty = (int)(v * TEX_SIZE);
    tx &= TEX_SIZE - 1; /* wrap; TEX_SIZE is a power of two */
    ty &= TEX_SIZE - 1;
    return t->px[ty * TEX_SIZE + tx];
}

#endif /* TEXTURE_H */
