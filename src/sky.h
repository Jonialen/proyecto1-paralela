#ifndef SKY_H
#define SKY_H

#include <stdint.h>
#include "math3d.h"
#include "render.h"

/* Sky and day/night cycle.
 *
 * The same state drives both the background and the terrain shading: at night
 * the sun is replaced by a dim moon and the ambient floor drops, so the world
 * darkens together with the sky instead of staying lit under a black sky. */
typedef struct {
    Vec3 sun_direction;   /* normalized, toward the sun (may be below horizon) */
    Light light;          /* what the terrain is shaded with, sun or moon */
    uint32_t zenith;
    uint32_t horizon;
    uint32_t sun_tint;    /* colour of the sun/moon disc and its glow */
    float star_alpha;     /* 0 by day, 1 at night */
    float sun_elevation;  /* sin of the sun's altitude, -1..1 */
    float phase;          /* 0..1 through the day; 0.5 is noon */
} Sky;

/* `phase` is the fraction through the day: 0 midnight, 0.25 sunrise, 0.5 noon,
 * 0.75 sunset. */
Sky sky_make(float phase);

/* Fills the sky behind the geometry of one viewport.
 *
 * Runs AFTER the terrain and writes only where the depth buffer is still at the
 * far plane, so sky pixels hidden by terrain are never computed. On a typical
 * frame the terrain covers most of the pane, which is the majority of the work
 * skipped.
 *
 * `tan_half_fov` and `aspect` reconstruct the view ray per pixel from the
 * camera basis. */
/* `parallel` splits the rows across threads. Rows write disjoint pixels and only
 * read the depth buffer, so there is nothing to synchronize. Pass 0 when the
 * caller is already inside a parallel region: OpenMP leaves nesting off, so an
 * inner region would simply run serially and only add overhead. */
void sky_render(Framebuffer *fb, const Viewport *view, const Sky *sky,
                Vec3 eye, Vec3 forward, Vec3 right, Vec3 up,
                float tan_half_fov, float aspect, int parallel);

/* Fills only rows [y0, y1] of the viewport. Lets a caller schedule sky slices
 * from several views in one flat parallel loop instead of one loop per view. */
void sky_render_slice(Framebuffer *fb, const Viewport *view, const Sky *sky,
                      Vec3 eye, Vec3 forward, Vec3 right, Vec3 up,
                      float tan_half_fov, float aspect, int y0, int y1);

#endif /* SKY_H */
