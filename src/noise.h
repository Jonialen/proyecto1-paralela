#ifndef NOISE_H
#define NOISE_H

#include <stdint.h>

/* Value noise: hash the integer lattice, interpolate with a smoothstep. It is
 * cheaper than Perlin and perfectly adequate for a heightmap, and being purely
 * a function of (x, y, seed) it needs no permutation table and no setup. */
float noise_value_2d(float x, float y, uint32_t seed);

/* Fractal Brownian motion: sum `octaves` layers of value noise, each one at
 * `lacunarity` times the frequency and `gain` times the amplitude of the last.
 * That is what turns smooth blobs into terrain with both broad hills and small
 * detail. Returns a value in [0, 1]. */
float noise_fbm_2d(float x, float y, uint32_t seed,
                   int octaves, float lacunarity, float gain);

/* Ridged noise: 1 - |2n - 1| folds the fBm around its midpoint, turning smooth
 * hills into sharp crests. Squaring the result sharpens them further. This is
 * what makes mountains look like mountains instead of large round blobs. */
float noise_ridged_2d(float x, float y, uint32_t seed,
                      int octaves, float lacunarity, float gain);

/* Deterministic 3D hash in [0, 1), used for scattering ores and trees. */
float noise_hash_3d(int x, int y, int z, uint32_t seed);

#endif /* NOISE_H */
