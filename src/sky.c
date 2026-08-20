#include "sky.h"
#include "noise.h"

#include <math.h>

#define SKY_PI 3.14159265358979323846f

/* Height of the cloud plane above the camera, in world units. */
#define CLOUD_HEIGHT 70.0f
#define CLOUD_SCALE 0.0022f

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static uint32_t rgb_of(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t color_lerp(uint32_t a, uint32_t b, float t)
{
    t = clamp01(t);
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    return rgb_of(ar + (int)((br - ar) * t),
                  ag + (int)((bg - ag) * t),
                  ab + (int)((bb - ab) * t));
}

Sky sky_make(float phase)
{
    Sky sky;
    sky.phase = phase - floorf(phase);

    /* The sun rides a tilted arc so it does not simply pass through the zenith.
     * Angle 0 is sunrise, pi/2 is noon. */
    float angle = 2.0f * SKY_PI * (sky.phase - 0.25f);
    float elevation = sinf(angle);
    float horizontal = cosf(angle);
    sky.sun_direction = vec3_normalize(vec3_make(horizontal * 0.62f, elevation,
                                                 horizontal * 0.79f));
    sky.sun_elevation = elevation;

    /* Palettes for the three regimes, blended by elevation. Twilight is not an
     * interpolation of day and night: it has its own warm horizon, which is
     * exactly what makes a sunrise read as a sunrise. */
    const uint32_t day_zenith    = 0x2E63BE, day_horizon    = 0xB9D6EF;
    const uint32_t dusk_zenith   = 0x2A3670, dusk_horizon   = 0xE3894A;
    const uint32_t night_zenith  = 0x060A18, night_horizon  = 0x111A33;

    if (elevation >= 0.0f) {
        /* Sunrise fades into full day over the first part of the climb. */
        float t = clamp01(elevation / 0.28f);
        sky.zenith = color_lerp(dusk_zenith, day_zenith, t);
        sky.horizon = color_lerp(dusk_horizon, day_horizon, t);
        sky.sun_tint = color_lerp(0xFFB765, 0xFFF3CE, t);
    } else {
        float t = clamp01(-elevation / 0.22f);
        sky.zenith = color_lerp(dusk_zenith, night_zenith, t);
        sky.horizon = color_lerp(dusk_horizon, night_horizon, t);
        sky.sun_tint = color_lerp(0xFFB765, 0xC8D4EE, t);
    }

    sky.star_alpha = clamp01((-elevation - 0.04f) * 4.0f);

    /* Terrain lighting. Above the horizon the sun leads; below it the moon takes
     * over from the opposite direction, dim and cool. */
    float daylight = clamp01(elevation * 2.4f);
    if (elevation > -0.02f) {
        sky.light.direction = sky.sun_direction;
        sky.light.ambient = 0.14f + 0.24f * daylight;
        sky.light.intensity = 0.10f + 0.58f * daylight;
    } else {
        sky.light.direction = vec3_make(-sky.sun_direction.x, -sky.sun_direction.y,
                                        -sky.sun_direction.z);
        sky.light.ambient = 0.11f;
        sky.light.intensity = 0.16f;
    }

    return sky;
}

/* Stars are hashed from a quantized direction rather than stored, so they cost
 * no memory and stay fixed relative to the sky as the camera turns. */
static float star_at(Vec3 dir)
{
    const float grid = 340.0f;
    int sx = (int)floorf(dir.x * grid);
    int sy = (int)floorf(dir.y * grid);
    int sz = (int)floorf(dir.z * grid);

    float h = noise_hash_3d(sx, sy, sz, 90210u);
    if (h < 0.9986f)
        return 0.0f;
    /* Vary the brightness so the field does not look like uniform dots. */
    return 0.45f + 0.55f * noise_hash_3d(sx, sz, sy, 1337u);
}

/* Clouds live on a horizontal plane above the camera. Intersecting the view ray
 * with that plane is what gives them perspective: they spread out overhead and
 * compress towards the horizon, instead of being pasted flat on the sky. */
static float cloud_at(Vec3 eye, Vec3 dir, float drift)
{
    if (dir.y < 0.012f)
        return 0.0f; /* at or below the horizon the plane is never hit */

    float s = CLOUD_HEIGHT / dir.y;
    float px = (eye.x + dir.x * s) * CLOUD_SCALE + drift;
    float pz = (eye.z + dir.z * s) * CLOUD_SCALE;

    float n = noise_fbm_2d(px, pz, 4242u, 3, 2.0f, 0.5f);

    /* Coverage threshold with a soft edge, so clouds have wispy borders. */
    float density = clamp01((n - 0.49f) * 5.0f);

    /* Fade near the horizon, where the plane intersection stretches towards
     * infinity and the noise would smear into bands. The explorers pitch down,
     * so the visible sky is a narrow band just above the horizon: fading too
     * aggressively here removes the clouds from the only part of the sky
     * anybody actually sees. */
    float fade = clamp01((dir.y - 0.012f) * 26.0f);
    return density * fade;
}

void sky_render(Framebuffer *fb, const Viewport *view, const Sky *sky,
                Vec3 eye, Vec3 forward, Vec3 right, Vec3 up,
                float tan_half_fov, float aspect)
{
    float drift = sky->phase * 3.0f; /* clouds crawl across the day */

    for (int y = 0; y < view->height; y++) {
        int py = view->y + y;
        size_t row = (size_t)py * fb->fb_width;

        /* +1 at the top of the screen, -1 at the bottom. */
        float ndc_y = 1.0f - 2.0f * ((float)y + 0.5f) / (float)view->height;

        for (int x = 0; x < view->width; x++) {
            size_t idx = row + (size_t)(view->x + x);

            /* Only untouched pixels are sky. Terrain already wrote its depth,
             * so everything it covers is skipped without computing a thing. */
            if (fb->depth[idx] < 1.0f)
                continue;

            float ndc_x = 2.0f * ((float)x + 0.5f) / (float)view->width - 1.0f;

            float sx = ndc_x * tan_half_fov * aspect;
            float sy = ndc_y * tan_half_fov;
            Vec3 dir = vec3_normalize(vec3_make(
                forward.x + right.x * sx + up.x * sy,
                forward.y + right.y * sx + up.y * sy,
                forward.z + right.z * sx + up.z * sy));

            /* Base gradient. The squared ramp keeps the bright band compressed
             * near the horizon, which is how a real sky reads. */
            float t = clamp01(dir.y);
            uint32_t color = color_lerp(sky->horizon, sky->zenith, t * t);

            if (sky->star_alpha > 0.01f) {
                float star = star_at(dir) * sky->star_alpha;
                if (star > 0.0f)
                    color = color_lerp(color, 0xFFFFFF, star);
            }

            /* Sun or moon disc, plus a glow that falls off sharply. */
            float toward = vec3_dot(dir, sky->light.direction);
            if (toward > 0.9992f) {
                color = sky->sun_tint;
            } else if (toward > 0.0f) {
                float glow = toward * toward;
                glow = glow * glow * glow * glow; /* toward^16 */
                glow = glow * glow;               /* toward^32 */
                if (glow > 0.004f)
                    color = color_lerp(color, sky->sun_tint, clamp01(glow * 0.85f));
            }

            float cloud = cloud_at(eye, dir, drift);
            if (cloud > 0.0f) {
                /* Clouds are lit by the same cycle: white at noon, orange at
                 * dusk, near black at night. */
                uint32_t lit = color_lerp(0x2A3346, 0xFFFFFF,
                                          clamp01(sky->sun_elevation * 2.0f + 0.35f));
                lit = color_lerp(lit, sky->sun_tint, 0.25f);
                color = color_lerp(color, lit, cloud * 0.9f);
            }

            fb->color[idx] = color;
        }
    }
}
