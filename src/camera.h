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
    float view_distance;/* how far this explorer can see; drives its render load */
} Camera;

/* Builds explorer `index` of `count`, spread over the world so their paths do
 * not overlap and each one gets its own speed. */
Camera camera_make(int index, int count, float world_extent, float terrain_height);

/* Where the camera is at time `t`, in world units. */
Vec3 camera_position(const Camera *camera, float t);

/* View-projection matrix for this camera at time `t`. `aspect` comes from the
 * viewport, not the window, so split-screen panes are not stretched. */
Mat4 camera_view_proj(const Camera *camera, float t, float aspect);

#endif /* CAMERA_H */
