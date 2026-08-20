#ifndef CAMERA_H
#define CAMERA_H

#include "math3d.h"

/* A first-person spectator flying a closed Lissajous path over the terrain.
 *
 * The path is pure trigonometry and, being closed, it never leaves the world:
 *
 *   x(t) = cx + radius_x * sin(rate_x * t + phase_x)
 *   z(t) = cz + radius_z * sin(rate_z * t + phase_z)
 *   y(t) = height + bob_amplitude * sin(bob_rate * t + phase_y)
 *
 * Different rate_x and rate_z turn the circle into a figure-eight, so the
 * explorer sweeps the map instead of orbiting the same ring. The yaw is taken
 * from the direction of travel, which is what makes it read as flying rather
 * than sliding sideways. */
typedef struct {
    Vec3 center;        /* centre of the path, in world units */
    float radius_x;
    float radius_z;
    float rate_x;       /* radians per second */
    float rate_z;
    float phase_x;      /* per-explorer offset so they do not fly in lockstep */
    float phase_z;

    float height;       /* mean flight height */
    float bob_amplitude;
    float bob_rate;
    float phase_y;

    float speed;        /* multiplies every rate: the "how fast" knob */
    float fov_y_rad;
    float view_distance;    /* how far this explorer can see: its render load */
    float generate_radius;  /* how far ahead terrain is generated */

    /* When set, the camera looks at `center` instead of along its own path.
     * A flyby looks TANGENTIALLY, which is right for exploring terrain but
     * never puts a single block at the origin in frame. */
    int look_at_center;
} Camera;

/* Terrain is generated out to this multiple of an explorer's view distance.
 *
 * It must be comfortably greater than 1. Face culling at the edge of the
 * rendered region needs the neighbouring chunk to already exist, and an
 * explorer moving at speed would otherwise fly into chunks that are still being
 * generated. The margin buys both. */
#define CAMERA_STREAM_FACTOR 2.5f

/* Builds explorer `index` of `count`, spread over the roaming area so their
 * paths do not overlap and each one gets its own speed and view distance. */
Camera camera_make(int index, int count, float roam_radius,
                   float view_distance, float flight_height);

/* Where the camera is at time `t`, in world units. */
Vec3 camera_position(const Camera *camera, float t);

/* Orthonormal camera basis at time `t`. The sky needs the same basis the
 * projection uses, so it is computed once here instead of twice. */
void camera_basis(const Camera *camera, float t,
                  Vec3 *eye, Vec3 *forward, Vec3 *right, Vec3 *up);

/* View-projection matrix for this camera at time `t`. `aspect` comes from the
 * viewport, not the window, so split-screen panes are not stretched. */
Mat4 camera_view_proj(const Camera *camera, float t, float aspect);

#endif /* CAMERA_H */
