#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stddef.h>
#include "math3d.h"
#include "texture.h"

/* ---------------------------------------------------------- framebuffer */

/* The framebuffer is rendered at width*ssaa x height*ssaa and box-filtered down
 * on resolve. Raising ssaa is the cheapest way to scale the pixel workload for
 * a scalability study without changing the window size. */
typedef struct {
    int width, height;   /* presented resolution */
    int ssaa;            /* supersampling factor, >= 1 */
    int fb_width, fb_height;
    uint32_t *color;     /* fb_width * fb_height, 0x00RRGGBB */
    float *depth;        /* fb_width * fb_height, NDC z in [-1, 1] */
} Framebuffer;

int framebuffer_init(Framebuffer *fb, int width, int height, int ssaa);
void framebuffer_free(Framebuffer *fb);
void framebuffer_clear(Framebuffer *fb, uint32_t color);
/* Box-filters the supersampled buffer into out[width * height]. */
void framebuffer_resolve(const Framebuffer *fb, uint32_t *out);

/* --------------------------------------------------------------- viewport */

/* A rectangle of the framebuffer that one camera renders into, in supersampled
 * pixels. Split-screen is N viewports over one framebuffer; because they are
 * disjoint, two cameras can never touch the same colour or depth slot. */
typedef struct {
    int x, y;
    int width, height;
} Viewport;

Viewport viewport_full(const Framebuffer *fb);
static inline float viewport_aspect(const Viewport *view)
{
    return (float)view->width / (float)view->height;
}


/* ------------------------------------------------------ geometry output */

/* A vertex after projection and the perspective divide. u_w/v_w/inv_w carry the
 * perspective-correct texture interpolants. */
typedef struct {
    float x, y;     /* screen space, y grows downwards */
    float z;        /* NDC depth in [-1, 1] */
    float inv_w;    /* 1/w */
    float u_w, v_w; /* u/w and v/w, which DO interpolate linearly on screen */
} ScreenVertex;

/* One ready-to-rasterize triangle. The screen bounding box is precomputed and
 * clamped so the raster stage can bin triangles into tiles without re-deriving
 * it. */
typedef struct {
    ScreenVertex v[3];
    const Texture *tex;
    float light;
    int min_x, min_y, max_x, max_y;
} ScreenTriangle;

/* Growable list of triangles produced by the geometry stage. */
typedef struct {
    ScreenTriangle *data;
    size_t count;
    size_t capacity;
} TriangleBuffer;

void tribuf_init(TriangleBuffer *buf);
void tribuf_free(TriangleBuffer *buf);
void tribuf_clear(TriangleBuffer *buf); /* keeps the allocation for reuse */
int tribuf_push(TriangleBuffer *buf, const ScreenTriangle *tri);

/* ------------------------------------------------------------ cube stage */

/* Directional light. Bundling direction with ambient and intensity is what lets
 * a day/night cycle darken the whole world: at night the sun is replaced by a
 * dim moon and the ambient floor drops, which a bare direction vector could not
 * express. */
typedef struct {
    Vec3 direction;  /* normalized, points TOWARD the light source */
    float ambient;   /* floor applied to every face, 0..1 */
    float intensity; /* scales the diffuse term, 0..1 */
} Light;

/* Face bits, in the same order as the internal cube_faces[] table:
 * +X, -X, +Y, -Y, +Z, -Z. A chunk clears the bit of every face that touches a
 * solid neighbour, which is what keeps a 4096-block chunk affordable. */
enum {
    FACE_POS_X = 1u << 0,
    FACE_NEG_X = 1u << 1,
    FACE_POS_Y = 1u << 2,
    FACE_NEG_Y = 1u << 3,
    FACE_POS_Z = 1u << 4,
    FACE_NEG_Z = 1u << 5,
    FACE_ALL   = 0x3Fu
};

/* Emits the visible faces of ONE cube into `out`. This is the single entry
 * point every caller uses: the standalone spinning cube passes a rotation as
 * `model` and FACE_ALL, a chunk passes a translation and a neighbour-derived
 * mask. Nothing is drawn here — only transformed, clipped and projected.
 *
 * The cube is a unit cube centred on the model-space origin.
 * `light_dir` must already be normalized. Returns the number of triangles
 * appended. */
size_t cube_emit(TriangleBuffer *out, const Block *block, Mat4 model, Mat4 vp,
                 const Light *light, unsigned face_mask, const Viewport *view);

/* ---------------------------------------------------------- raster stage */

/* Rasterizes one triangle, restricted to the given screen rectangle. The clip
 * rectangle is what makes tile-parallel rasterization possible later: give each
 * worker its own tile and no two workers ever touch the same depth slot. */
void raster_triangle(Framebuffer *fb, const ScreenTriangle *tri,
                     int clip_min_x, int clip_min_y, int clip_max_x, int clip_max_y);

/* Rasterizes every triangle in the buffer over the whole framebuffer. */
void raster_flush(Framebuffer *fb, const TriangleBuffer *tris);

#endif /* RENDER_H */
