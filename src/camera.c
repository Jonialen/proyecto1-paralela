#include "camera.h"

#include <math.h>

#define CAMERA_PI 3.14159265358979323846f

Vec3 camera_position(const Camera *camera, float t)
{
    float ts = t * camera->speed;
    return vec3_make(
        camera->center.x + camera->radius_x * sinf(camera->rate_x * ts + camera->phase_x),
        camera->height + camera->bob_amplitude * sinf(camera->bob_rate * ts + camera->phase_y),
        camera->center.z + camera->radius_z * sinf(camera->rate_z * ts + camera->phase_z));
}

Mat4 camera_view_proj(const Camera *camera, float t, float aspect)
{
    Vec3 eye = camera_position(camera, t);

    /* Heading comes from the HORIZONTAL travel direction only. Including the
     * vertical bob here would be wrong: near the top and bottom of the sine the
     * vertical velocity dominates the horizontal one, and the explorer ends up
     * staring straight at the sky or the ground instead of ahead. */
    Vec3 ahead = camera_position(camera, t + 0.08f);
    float dx = ahead.x - eye.x;
    float dz = ahead.z - eye.z;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-6f) {         /* degenerate: path stalled, pick any heading */
        dx = 0.0f; dz = -1.0f; len = 1.0f;
    }
    dx /= len;
    dz /= len;

    /* Pitch is its own gentle oscillation, biased downwards so the terrain
     * stays in frame rather than the empty sky. */
    float ts = t * camera->speed;
    float pitch = -0.20f + 0.13f * sinf(camera->bob_rate * 0.6f * ts + camera->phase_y);
    float cos_pitch = cosf(pitch);

    Vec3 target = vec3_make(eye.x + dx * cos_pitch,
                            eye.y + sinf(pitch),
                            eye.z + dz * cos_pitch);

    Mat4 view = mat4_look_at(eye, target, vec3_make(0.0f, 1.0f, 0.0f));
    /* The far plane is this explorer's view distance, so a faster/farther-seeing
     * explorer genuinely rasterizes more terrain than a slower one. */
    Mat4 proj = mat4_perspective(camera->fov_y_rad, aspect, 0.15f, camera->view_distance);
    return mat4_mul(proj, view);
}

Camera camera_make(int index, int count, float world_extent, float terrain_height)
{
    if (count < 1) count = 1;

    Camera camera;
    float span = world_extent * 0.5f;

    camera.center = vec3_make(0.0f, 0.0f, 0.0f);
    camera.radius_x = span * 0.62f;
    camera.radius_z = span * 0.62f;

    /* Spread the explorers evenly around their paths so they start apart. */
    float share = (float)index / (float)count;
    camera.phase_x = share * 2.0f * CAMERA_PI;
    camera.phase_z = share * 2.0f * CAMERA_PI + CAMERA_PI * 0.5f;
    camera.phase_y = share * 2.0f * CAMERA_PI;

    /* Slightly irrational rate ratio: the figure-eight never exactly repeats,
     * so the explorer keeps covering new ground. */
    camera.rate_x = 0.085f;
    camera.rate_z = 0.052f;

    /* terrain_height is the highest ground the generator can produce, so this
     * clears the peaks with margin even at the bottom of the bob. */
    camera.height = terrain_height + 8.0f;
    camera.bob_amplitude = 3.5f;
    camera.bob_rate = 0.9f;

    /* Each explorer flies at its own speed. This is deliberate: unequal speeds
     * and view distances give the workers unequal work, which is exactly the
     * load imbalance that makes OpenMP scheduling policy matter. */
    camera.speed = 1.0f + 0.35f * (float)index;
    camera.fov_y_rad = 65.0f * CAMERA_PI / 180.0f;
    camera.view_distance = world_extent * (0.55f + 0.18f * (float)(index % 3));

    return camera;
}
