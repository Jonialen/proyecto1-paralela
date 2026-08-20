#include "noise.h"

#include <math.h>

static uint32_t hash_ints(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    h = (h ^ (h >> 7)) * 2654435761u;
    return h ^ (h >> 15);
}

static float hash_unit(uint32_t h)
{
    /* Top 24 bits give a clean [0, 1) without touching the weakest low bits. */
    return (float)(h >> 8) * (1.0f / 16777216.0f);
}

/* Hermite smoothstep. Using the raw fraction here would make the lattice grid
 * plainly visible as diamond-shaped creases in the terrain. */
static float smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

float noise_value_2d(float x, float y, uint32_t seed)
{
    float fx = floorf(x), fy = floorf(y);
    int x0 = (int)fx, y0 = (int)fy;
    float tx = smooth(x - fx), ty = smooth(y - fy);

    float v00 = hash_unit(hash_ints((uint32_t)x0,     (uint32_t)y0,     0u, seed));
    float v10 = hash_unit(hash_ints((uint32_t)(x0+1), (uint32_t)y0,     0u, seed));
    float v01 = hash_unit(hash_ints((uint32_t)x0,     (uint32_t)(y0+1), 0u, seed));
    float v11 = hash_unit(hash_ints((uint32_t)(x0+1), (uint32_t)(y0+1), 0u, seed));

    return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), ty);
}

float noise_fbm_2d(float x, float y, uint32_t seed,
                   int octaves, float lacunarity, float gain)
{
    if (octaves < 1) octaves = 1;

    float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, total = 0.0f;
    for (int i = 0; i < octaves; i++) {
        /* Each octave gets its own seed so the layers are independent instead
         * of being the same pattern scaled up. */
        sum += amplitude * noise_value_2d(x * frequency, y * frequency, seed + (uint32_t)i * 7919u);
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / total;
}

float noise_hash_3d(int x, int y, int z, uint32_t seed)
{
    return hash_unit(hash_ints((uint32_t)x, (uint32_t)y, (uint32_t)z, seed));
}
