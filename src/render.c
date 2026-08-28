#include "render.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* ---------------------------------------------------------------- geometry */

typedef struct {
    Vec3 pos;
    float u, v;
} CubeVertex;

typedef struct {
    CubeVertex v[4]; /* counter-clockwise seen from outside the cube */
    Vec3 normal;
} CubeFace;

/* Face order: +X, -X, +Y, -Y, +Z, -Z. The FACE_* bits and
 * block_texture_for_face() both rely on it. */
static const CubeFace cube_faces[6] = {
    { { { {  0.5f, -0.5f,  0.5f }, 0.0f, 1.0f },
        { {  0.5f, -0.5f, -0.5f }, 1.0f, 1.0f },
        { {  0.5f,  0.5f, -0.5f }, 1.0f, 0.0f },
        { {  0.5f,  0.5f,  0.5f }, 0.0f, 0.0f } }, {  1.0f,  0.0f,  0.0f } },

    { { { { -0.5f, -0.5f, -0.5f }, 0.0f, 1.0f },
        { { -0.5f, -0.5f,  0.5f }, 1.0f, 1.0f },
        { { -0.5f,  0.5f,  0.5f }, 1.0f, 0.0f },
        { { -0.5f,  0.5f, -0.5f }, 0.0f, 0.0f } }, { -1.0f,  0.0f,  0.0f } },

    { { { { -0.5f,  0.5f,  0.5f }, 0.0f, 1.0f },
        { {  0.5f,  0.5f,  0.5f }, 1.0f, 1.0f },
        { {  0.5f,  0.5f, -0.5f }, 1.0f, 0.0f },
        { { -0.5f,  0.5f, -0.5f }, 0.0f, 0.0f } }, {  0.0f,  1.0f,  0.0f } },

    { { { { -0.5f, -0.5f, -0.5f }, 0.0f, 1.0f },
        { {  0.5f, -0.5f, -0.5f }, 1.0f, 1.0f },
        { {  0.5f, -0.5f,  0.5f }, 1.0f, 0.0f },
        { { -0.5f, -0.5f,  0.5f }, 0.0f, 0.0f } }, {  0.0f, -1.0f,  0.0f } },

    { { { { -0.5f, -0.5f,  0.5f }, 0.0f, 1.0f },
        { {  0.5f, -0.5f,  0.5f }, 1.0f, 1.0f },
        { {  0.5f,  0.5f,  0.5f }, 1.0f, 0.0f },
        { { -0.5f,  0.5f,  0.5f }, 0.0f, 0.0f } }, {  0.0f,  0.0f,  1.0f } },

    { { { {  0.5f, -0.5f, -0.5f }, 0.0f, 1.0f },
        { { -0.5f, -0.5f, -0.5f }, 1.0f, 1.0f },
        { { -0.5f,  0.5f, -0.5f }, 1.0f, 0.0f },
        { {  0.5f,  0.5f, -0.5f }, 0.0f, 0.0f } }, {  0.0f,  0.0f, -1.0f } }
};

/* ------------------------------------------------------------- framebuffer */

int framebuffer_init(Framebuffer *fb, int width, int height, int ssaa)
{
    if (ssaa < 1) ssaa = 1;
    fb->width = width;
    fb->height = height;
    fb->ssaa = ssaa;
    fb->fb_width = width * ssaa;
    fb->fb_height = height * ssaa;

    size_t pixels = (size_t)fb->fb_width * (size_t)fb->fb_height;
    fb->color = malloc(pixels * sizeof(uint32_t));
    fb->depth = malloc(pixels * sizeof(float));
    if (!fb->color || !fb->depth) {
        framebuffer_free(fb);
        return 0;
    }
    return 1;
}

void framebuffer_free(Framebuffer *fb)
{
    free(fb->color);
    free(fb->depth);
    fb->color = NULL;
    fb->depth = NULL;
}

void framebuffer_clear(Framebuffer *fb, uint32_t color)
{
    size_t pixels = (size_t)fb->fb_width * (size_t)fb->fb_height;
    for (size_t i = 0; i < pixels; i++) {
        fb->color[i] = color;
        fb->depth[i] = 1.0f; /* far plane in NDC */
    }
}

void framebuffer_resolve(const Framebuffer *fb, uint32_t *out)
{
    int s = fb->ssaa;
    if (s == 1) {
        memcpy(out, fb->color, (size_t)fb->width * (size_t)fb->height * sizeof(uint32_t));
        return;
    }

    int samples = s * s;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = 0; sy < s; sy++) {
                const uint32_t *row = &fb->color[(size_t)(y * s + sy) * fb->fb_width + (size_t)(x * s)];
                for (int sx = 0; sx < s; sx++) {
                    uint32_t c = row[sx];
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                }
            }
            out[(size_t)y * fb->width + x] =
                ((r / samples) << 16) | ((g / samples) << 8) | (b / samples);
        }
    }
}

/* -------------------------------------------------------- triangle buffer */

void tribuf_init(TriangleBuffer *buf)
{
    buf->data = NULL;
    buf->count = 0;
    buf->capacity = 0;
}

void tribuf_free(TriangleBuffer *buf)
{
    free(buf->data);
    tribuf_init(buf);
}

void tribuf_clear(TriangleBuffer *buf)
{
    buf->count = 0;
}

int tribuf_push(TriangleBuffer *buf, const ScreenTriangle *tri)
{
    if (buf->count == buf->capacity) {
        size_t next = buf->capacity ? buf->capacity * 2 : 1024;
        ScreenTriangle *grown = realloc(buf->data, next * sizeof(ScreenTriangle));
        if (!grown)
            return 0;
        buf->data = grown;
        buf->capacity = next;
    }
    buf->data[buf->count++] = *tri;
    return 1;
}

int tribuf_append(TriangleBuffer *dst, const TriangleBuffer *src)
{
    if (src->count == 0)
        return 1;

    size_t needed = dst->count + src->count;
    if (needed > dst->capacity) {
        size_t next = dst->capacity ? dst->capacity : 1024;
        while (next < needed)
            next *= 2;
        ScreenTriangle *grown = realloc(dst->data, next * sizeof(ScreenTriangle));
        if (!grown)
            return 0;
        dst->data = grown;
        dst->capacity = next;
    }

    memcpy(dst->data + dst->count, src->data, src->count * sizeof(ScreenTriangle));
    dst->count = needed;
    return 1;
}

/* ------------------------------------------------------------ cube stage */

typedef struct {
    Vec4 clip;
    float u, v;
    float light;   /* per-vertex, so ambient occlusion survives clipping */
} ClipVertex;

static ClipVertex clip_lerp(ClipVertex a, ClipVertex b, float t)
{
    ClipVertex r;
    r.clip.x = a.clip.x + (b.clip.x - a.clip.x) * t;
    r.clip.y = a.clip.y + (b.clip.y - a.clip.y) * t;
    r.clip.z = a.clip.z + (b.clip.z - a.clip.z) * t;
    r.clip.w = a.clip.w + (b.clip.w - a.clip.w) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    r.light = a.light + (b.light - a.light) * t;
    return r;
}

/* Sutherland-Hodgman against the near plane only (z + w > 0). Without it a
 * vertex behind the camera divides by a negative w and the triangle explodes. */
static int clip_near_plane(const ClipVertex *in, int in_count, ClipVertex *out)
{
    int out_count = 0;
    for (int i = 0; i < in_count; i++) {
        ClipVertex cur = in[i];
        ClipVertex nxt = in[(i + 1) % in_count];
        float d_cur = cur.clip.z + cur.clip.w;
        float d_nxt = nxt.clip.z + nxt.clip.w;

        if (d_cur > 0.0f)
            out[out_count++] = cur;
        if ((d_cur > 0.0f) != (d_nxt > 0.0f)) {
            float t = d_cur / (d_cur - d_nxt);
            out[out_count++] = clip_lerp(cur, nxt, t);
        }
    }
    return out_count;
}

Viewport viewport_full(const Framebuffer *fb)
{
    Viewport view = { 0, 0, fb->fb_width, fb->fb_height };
    return view;
}

/* NDC maps into the VIEWPORT rectangle, not the whole framebuffer. This one
 * line is what turns the renderer from single-view into split-screen. */
static ScreenVertex to_screen(ClipVertex c, const Viewport *view)
{
    ScreenVertex s;
    float inv_w = 1.0f / c.clip.w;
    s.x = (float)view->x + (c.clip.x * inv_w * 0.5f + 0.5f) * (float)view->width;
    s.y = (float)view->y + (0.5f - c.clip.y * inv_w * 0.5f) * (float)view->height;
    s.z = c.clip.z * inv_w;
    s.inv_w = inv_w;
    s.u_w = c.u * inv_w;
    s.v_w = c.v * inv_w;
    s.l_w = c.light * inv_w;
    return s;
}

static float edge_function(const ScreenVertex *a, const ScreenVertex *b, float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* Culls, bounds and appends one triangle. Returns 1 if it was kept. */
static int emit_triangle(TriangleBuffer *out, const Texture *tex,
                         ScreenVertex a, ScreenVertex b, ScreenVertex c,
                         const Viewport *view)
{
    /* Screen space has y pointing down, so a front-facing (CCW in 3D) triangle
     * yields a negative signed area. Anything else is a backface: cull it. */
    if (edge_function(&a, &b, c.x, c.y) >= 0.0f)
        return 0;

    ScreenTriangle tri;
    tri.v[0] = a;
    tri.v[1] = b;
    tri.v[2] = c;
    tri.tex = tex;
    /* Clamped to the viewport, so geometry from one camera can never bleed into
     * a neighbouring split-screen pane. */
    tri.min_x = imax(view->x, (int)floorf(fminf(fminf(a.x, b.x), c.x)));
    tri.max_x = imin(view->x + view->width - 1, (int)ceilf(fmaxf(fmaxf(a.x, b.x), c.x)));
    tri.min_y = imax(view->y, (int)floorf(fminf(fminf(a.y, b.y), c.y)));
    tri.max_y = imin(view->y + view->height - 1, (int)ceilf(fmaxf(fmaxf(a.y, b.y), c.y)));

    /* Fully outside the viewport: drop it before it ever reaches the raster. */
    if (tri.min_x > tri.max_x || tri.min_y > tri.max_y)
        return 0;

    return tribuf_push(out, &tri);
}

static int occlusion_bit(uint32_t occlusion, int dx, int dy, int dz)
{
    return (int)((occlusion >> ((dx + 1) * 9 + (dy + 1) * 3 + (dz + 1))) & 1u);
}

/* Ambient occlusion for one corner of one face.
 *
 * A corner is darkened by the two neighbours that share its edges and by the
 * one diagonally across. The special case matters: when BOTH edge neighbours
 * are solid the corner is sealed, and the diagonal cannot make it brighter --
 * without that rule, inside corners flicker between two shades depending on a
 * block you cannot even see. */
static float vertex_occlusion(uint32_t occlusion, Vec3 normal, Vec3 pos)
{
    int n[3] = { (int)normal.x, (int)normal.y, (int)normal.z };
    float p[3] = { pos.x, pos.y, pos.z };

    /* The face spans the two axes its normal is zero on. Which way each one
     * points is decided by which corner of the face this vertex is. */
    int t[3] = { 0, 0, 0 };
    int u[3] = { 0, 0, 0 };
    int found = 0;
    for (int axis = 0; axis < 3; axis++) {
        if (n[axis] != 0)
            continue;
        int sign = (p[axis] > 0.0f) ? 1 : -1;
        if (found == 0)
            t[axis] = sign;
        else
            u[axis] = sign;
        found++;
    }

    int side1 = occlusion_bit(occlusion, n[0] + t[0], n[1] + t[1], n[2] + t[2]);
    int side2 = occlusion_bit(occlusion, n[0] + u[0], n[1] + u[1], n[2] + u[2]);
    int corner = occlusion_bit(occlusion,
                               n[0] + t[0] + u[0],
                               n[1] + t[1] + u[1],
                               n[2] + t[2] + u[2]);

    int level = (side1 && side2) ? 0 : 3 - (side1 + side2 + corner);
    static const float shade[4] = { 0.46f, 0.65f, 0.83f, 1.0f };
    return shade[level];
}

size_t cube_emit(TriangleBuffer *out, const Block *block, Mat4 model, Mat4 vp,
                 const Light *light, unsigned face_mask, uint32_t occlusion,
                 const Viewport *view)
{
    if (!block || (face_mask & FACE_ALL) == 0)
        return 0;

    Mat4 mvp = mat4_mul(vp, model);
    size_t emitted = 0;

    for (int face = 0; face < 6; face++) {
        if (!(face_mask & (1u << face)))
            continue;

        const CubeFace *f = &cube_faces[face];
        const Texture *tex = texture_get(block_texture_for_face(block, face));

        /* Flat shading: one lambert term per face, exactly like a voxel game. */
        Vec4 n4 = { f->normal.x, f->normal.y, f->normal.z, 0.0f };
        Vec4 wn = mat4_mul_vec4(model, n4);
        Vec3 normal = vec3_normalize(vec3_make(wn.x, wn.y, wn.z));
        float diffuse = vec3_dot(normal, light->direction);
        if (diffuse < 0.0f) diffuse = 0.0f;
        float light_term = light->ambient + light->intensity * diffuse;
        if (light_term > 1.0f) light_term = 1.0f;

        ClipVertex quad[4];
        for (int i = 0; i < 4; i++) {
            Vec4 p = { f->v[i].pos.x, f->v[i].pos.y, f->v[i].pos.z, 1.0f };
            quad[i].clip = mat4_mul_vec4(mvp, p);
            quad[i].u = f->v[i].u;
            quad[i].v = f->v[i].v;
            quad[i].light = light_term *
                            vertex_occlusion(occlusion, f->normal, f->v[i].pos);
        }

        /* Clipping one quad against one plane yields at most 5 vertices. */
        ClipVertex poly[8];
        int count = clip_near_plane(quad, 4, poly);
        if (count < 3)
            continue;

        ScreenVertex sv[8];
        for (int i = 0; i < count; i++)
            sv[i] = to_screen(poly[i], view);

        for (int i = 1; i + 1 < count; i++)
            emitted += (size_t)emit_triangle(out, tex,
                                             sv[0], sv[i], sv[i + 1], view);
    }

    return emitted;
}

/* -------------------------------------------------------------- rasterizer */

void raster_triangle(Framebuffer *fb, const ScreenTriangle *tri,
                     int clip_min_x, int clip_min_y, int clip_max_x, int clip_max_y)
{
    const ScreenVertex *a = &tri->v[0];
    const ScreenVertex *b = &tri->v[1];
    const ScreenVertex *c = &tri->v[2];

    float area = edge_function(a, b, c->x, c->y);
    if (area >= 0.0f)
        return;
    float inv_area = 1.0f / area;

    int min_x = imax(tri->min_x, clip_min_x);
    int max_x = imin(tri->max_x, clip_max_x);
    int min_y = imax(tri->min_y, clip_min_y);
    int max_y = imin(tri->max_y, clip_max_y);
    if (min_x > max_x || min_y > max_y)
        return;

    /* This double loop is the hot spot of the whole program: it is where the
     * per-pixel work lives, and the natural target for parallelization later. */
    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        size_t row = (size_t)y * fb->fb_width;

        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;

            float w0 = edge_function(b, c, px, py);
            float w1 = edge_function(c, a, px, py);
            float w2 = edge_function(a, b, px, py);
            /* area is negative, so inside means all edges are negative too. */
            if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)
                continue;

            float l0 = w0 * inv_area;
            float l1 = w1 * inv_area;
            float l2 = w2 * inv_area;

            /* z/w is already projected, so it interpolates linearly on screen. */
            float z = l0 * a->z + l1 * b->z + l2 * c->z;
            size_t idx = row + (size_t)x;
            if (z >= fb->depth[idx])
                continue;

            float inv_w = l0 * a->inv_w + l1 * b->inv_w + l2 * c->inv_w;
            float w = 1.0f / inv_w;
            float u = (l0 * a->u_w + l1 * b->u_w + l2 * c->u_w) * w;
            float v = (l0 * a->v_w + l1 * b->v_w + l2 * c->v_w) * w;

            float light = (l0 * a->l_w + l1 * b->l_w + l2 * c->l_w) * w;

            uint32_t texel = texture_sample(tri->tex, u, v);
            int r = (int)(((texel >> 16) & 0xFF) * light);
            int g = (int)(((texel >> 8) & 0xFF) * light);
            int bl = (int)((texel & 0xFF) * light);

            fb->depth[idx] = z;
            fb->color[idx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        }
    }
}

void raster_flush(Framebuffer *fb, const TriangleBuffer *tris,
                  const Viewport *view, int parallel)
{
    int x0 = view->x;
    int x1 = view->x + view->width - 1;

    if (!parallel) {
        for (size_t i = 0; i < tris->count; i++)
            raster_triangle(fb, &tris->data[i], x0, view->y,
                            x1, view->y + view->height - 1);
        return;
    }

    /* More bands than threads so dynamic scheduling has something to balance:
     * a band across the horizon carries far more covered pixels than one over
     * empty sky. */
    int bands = 1;
#ifdef _OPENMP
    bands = omp_get_max_threads() * 4;
#endif
    if (bands > view->height)
        bands = view->height;
    if (bands < 1)
        bands = 1;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
    for (int b = 0; b < bands; b++) {
        int y0 = view->y + (int)((long)view->height * b / bands);
        int y1 = view->y + (int)((long)view->height * (b + 1) / bands) - 1;
        for (size_t i = 0; i < tris->count; i++)
            raster_triangle(fb, &tris->data[i], x0, y0, x1, y1);
    }
}
