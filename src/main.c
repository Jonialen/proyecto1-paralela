#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "math3d.h"
#include "render.h"
#include "texture.h"
#include "world.h"

#define PI 3.14159265358979323846f

enum { SCENE_BLOCK, SCENE_CHUNK };

typedef struct {
    int width;
    int height;
    int ssaa;
    int block;
    int scene;
    int chunks;
    unsigned seed;
    int bench_frames;      /* > 0 runs headless and prints timings */
    const char *dump_path; /* non-NULL renders a single frame to a PPM file */
} Options;

/* Everything the geometry stage needs, kept in one place so the interactive,
 * benchmark and dump paths all build their frames the exact same way. */
typedef struct {
    int scene;
    int block_index;
    World world;
    Vec3 light;
} Scene;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int scene_init(Scene *scene, int scene_kind, int block_index,
                      int chunks, unsigned seed)
{
    scene->scene = scene_kind;
    scene->block_index = block_index;
    scene->light = vec3_normalize(vec3_make(0.45f, 0.8f, 0.6f));

    TerrainParams params = terrain_default(seed);
    if (!world_init(&scene->world, chunks, &params)) {
        fprintf(stderr, "Out of memory allocating the world\n");
        return 0;
    }
    world_generate(&scene->world);
    return 1;
}

static void scene_free(Scene *scene)
{
    world_free(&scene->world);
}

/* Reseeds and regenerates in place, keeping the same allocation. */
static void scene_reseed(Scene *scene, unsigned seed)
{
    scene->world.params.seed = seed;
    world_generate(&scene->world);
}

/* Width of the scene in world units. Every camera limit is derived from it, so
 * nothing artificially caps how much world you can load and look at. */
static float scene_extent(const Scene *scene)
{
    if (scene->scene != SCENE_CHUNK)
        return 1.0f;
    return (float)(scene->world.size * CHUNK_SIZE_X);
}

static float scene_default_distance(const Scene *scene)
{
    return 1.9f * scene_extent(scene);
}

static float scene_max_distance(const Scene *scene)
{
    return 8.0f * scene_extent(scene);
}

/* The far plane has to reach past the furthest corner of the world, otherwise
 * distant geometry lands beyond NDC z = 1, fails the depth test and silently
 * disappears -- which looks exactly like a render limit. */
static float scene_far_plane(const Scene *scene, float distance)
{
    return distance + 2.0f * scene_extent(scene) + 10.0f;
}

/* Orbit camera: the eye sits on a sphere around the origin, so both scenes use
 * the same controls and the light stays fixed in world space. */
static Mat4 build_view_proj(float yaw, float pitch, float distance, float aspect,
                            float far_plane)
{
    Vec3 eye = vec3_make(distance * cosf(pitch) * sinf(yaw),
                         distance * sinf(pitch),
                         distance * cosf(pitch) * cosf(yaw));
    Mat4 view = mat4_look_at(eye, vec3_make(0.0f, 0.0f, 0.0f),
                             vec3_make(0.0f, 1.0f, 0.0f));
    Mat4 proj = mat4_perspective(50.0f * PI / 180.0f, aspect, 0.1f, far_plane);
    return mat4_mul(proj, view);
}

/* Geometry stage. Both scenes funnel through cube_emit(): the single block
 * passes an identity model and FACE_ALL, the chunk lets chunk_emit() derive one
 * model matrix and one face mask per voxel. */
static size_t scene_emit(TriangleBuffer *tris, const Scene *scene, Mat4 vp,
                         int fb_width, int fb_height)
{
    tribuf_clear(tris);

    if (scene->scene == SCENE_CHUNK)
        return world_emit(tris, &scene->world, vp, scene->light,
                          fb_width, fb_height);

    return cube_emit(tris, block_get(scene->block_index), mat4_identity(), vp,
                     scene->light, FACE_ALL, fb_width, fb_height);
}

static size_t render_frame(Framebuffer *fb, TriangleBuffer *tris, const Scene *scene,
                           float yaw, float pitch, float distance)
{
    float aspect = (float)fb->width / (float)fb->height;
    Mat4 vp = build_view_proj(yaw, pitch, distance, aspect,
                              scene_far_plane(scene, distance));

    size_t count = scene_emit(tris, scene, vp, fb->fb_width, fb->fb_height);
    framebuffer_clear(fb, 0x101820);
    raster_flush(fb, tris);
    return count;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --width N       window width  (default 900)\n");
    printf("  --height N      window height (default 700)\n");
    printf("  --ssaa N        supersampling factor, 1-8 (default 1)\n");
    printf("  --block N       initial block index, 0-%d (default 0)\n", block_count() - 1);
    printf("  --scene NAME    'block' or 'chunk' (default block)\n");
    printf("  --chunks N      world size in chunks per side, 1-64 (default 1)\n");
    printf("  --seed N        terrain seed (default 1337)\n");
    printf("  --bench N       render N frames headless and report timings\n");
    printf("  --dump PATH     render one frame headless into a binary PPM file\n");
    printf("  --help          show this message\n");
}

static int parse_options(int argc, char **argv, Options *opt)
{
    for (int i = 1; i < argc; i++) {
        int has_value = (i + 1 < argc);
        if (!strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--width") && has_value) {
            opt->width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && has_value) {
            opt->height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ssaa") && has_value) {
            opt->ssaa = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--block") && has_value) {
            opt->block = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--scene") && has_value) {
            const char *name = argv[++i];
            if (!strcmp(name, "chunk")) {
                opt->scene = SCENE_CHUNK;
            } else if (!strcmp(name, "block")) {
                opt->scene = SCENE_BLOCK;
            } else {
                fprintf(stderr, "Unknown scene: %s (expected 'block' or 'chunk')\n", name);
                return 0;
            }
        } else if (!strcmp(argv[i], "--chunks") && has_value) {
            opt->chunks = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && has_value) {
            opt->seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--bench") && has_value) {
            opt->bench_frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dump") && has_value) {
            opt->dump_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 0;
        }
    }

    if (opt->width < 64) opt->width = 64;
    if (opt->height < 64) opt->height = 64;
    if (opt->ssaa < 1) opt->ssaa = 1;
    if (opt->ssaa > 8) opt->ssaa = 8;
    if (opt->chunks < 1) opt->chunks = 1;
    if (opt->chunks > 64) opt->chunks = 64; /* 64x64 chunks is ~32 MB of voxels */
    return 1;
}

/* Binary PPM is the simplest lossless format to diff two renderers with. */
static int write_ppm(const char *path, const uint32_t *pixels, int width, int height)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Cannot open %s for writing\n", path);
        return 0;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        unsigned char rgb[3] = {
            (unsigned char)((pixels[i] >> 16) & 0xFF),
            (unsigned char)((pixels[i] >> 8) & 0xFF),
            (unsigned char)(pixels[i] & 0xFF)
        };
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    return 1;
}

/* Allocates the framebuffer, the resolve buffer and the triangle list together
 * because every headless mode needs the same three. */
static int alloc_frame_resources(const Options *opt, Framebuffer *fb,
                                 uint32_t **resolved, TriangleBuffer *tris)
{
    tribuf_init(tris);
    *resolved = malloc((size_t)opt->width * opt->height * sizeof(uint32_t));
    if (!framebuffer_init(fb, opt->width, opt->height, opt->ssaa) || !*resolved) {
        fprintf(stderr, "Out of memory allocating buffers\n");
        free(*resolved);
        *resolved = NULL;
        return 0;
    }
    return 1;
}

static int run_dump(const Options *opt)
{
    Framebuffer fb;
    TriangleBuffer tris;
    uint32_t *resolved;
    if (!alloc_frame_resources(opt, &fb, &resolved, &tris))
        return 1;

    Scene scene;
    if (!scene_init(&scene, opt->scene, opt->block, opt->chunks, opt->seed)) {
        free(resolved);
        tribuf_free(&tris);
        framebuffer_free(&fb);
        return 1;
    }

    size_t count = render_frame(&fb, &tris, &scene, 0.6f, 0.35f,
                                scene_default_distance(&scene));
    framebuffer_resolve(&fb, resolved);

    int ok = write_ppm(opt->dump_path, resolved, opt->width, opt->height);
    if (ok)
        printf("Wrote %s (%dx%d, %zu triangles)\n",
               opt->dump_path, opt->width, opt->height, count);

    free(resolved);
    tribuf_free(&tris);
    framebuffer_free(&fb);
    scene_free(&scene);
    return ok ? 0 : 1;
}

static int run_benchmark(const Options *opt)
{
    Framebuffer fb;
    TriangleBuffer tris;
    uint32_t *resolved;
    if (!alloc_frame_resources(opt, &fb, &resolved, &tris))
        return 1;

    Scene scene;
    if (!scene_init(&scene, opt->scene, opt->block, opt->chunks, opt->seed)) {
        free(resolved);
        tribuf_free(&tris);
        framebuffer_free(&fb);
        return 1;
    }
    float distance = scene_default_distance(&scene);

    printf("Benchmark: %dx%d, ssaa=%d (%dx%d internal), scene=%s, frames=%d\n",
           opt->width, opt->height, opt->ssaa, fb.fb_width, fb.fb_height,
           opt->scene == SCENE_CHUNK ? "chunk" : block_get(opt->block)->name,
           opt->bench_frames);

    size_t last_count = 0;
    double geometry_time = 0.0;
    double raster_time = 0.0;
    double start = now_seconds();

    for (int frame = 0; frame < opt->bench_frames; frame++) {
        float yaw = (float)frame * 0.017f;
        float pitch = 0.45f + 0.25f * sinf((float)frame * 0.011f);
        Mat4 vp = build_view_proj(yaw, pitch, distance,
                                  (float)opt->width / (float)opt->height,
                                  scene_far_plane(&scene, distance));

        /* Timed separately: the two stages parallelize in completely different
         * ways, so the split tells you which one is worth attacking first. */
        double t0 = now_seconds();
        last_count = scene_emit(&tris, &scene, vp, fb.fb_width, fb.fb_height);
        double t1 = now_seconds();

        framebuffer_clear(&fb, 0x101820);
        raster_flush(&fb, &tris);
        framebuffer_resolve(&fb, resolved);
        double t2 = now_seconds();

        geometry_time += t1 - t0;
        raster_time += t2 - t1;
    }
    double elapsed = now_seconds() - start;

    double per_frame_ms = elapsed * 1000.0 / opt->bench_frames;
    double mpix = (double)fb.fb_width * fb.fb_height * opt->bench_frames / 1e6;
    if (opt->scene == SCENE_CHUNK) {
        size_t solid = world_solid_blocks(&scene.world);
        printf("Solid blocks: %zu | faces without culling: %zu\n", solid, solid * 6);
    }
    printf("Triangles:  %zu (last frame)\n", last_count);
    printf("Total:      %.4f s\n", elapsed);
    printf("Per frame:  %.4f ms  (%.2f FPS)\n", per_frame_ms, 1000.0 / per_frame_ms);
    printf("  geometry: %.4f ms  (%.1f%%)\n",
           geometry_time * 1000.0 / opt->bench_frames, 100.0 * geometry_time / elapsed);
    printf("  raster:   %.4f ms  (%.1f%%)\n",
           raster_time * 1000.0 / opt->bench_frames, 100.0 * raster_time / elapsed);
    printf("Throughput: %.2f Mpixel/s (internal resolution)\n", mpix / elapsed);

    free(resolved);
    tribuf_free(&tris);
    framebuffer_free(&fb);
    scene_free(&scene);
    return 0;
}

static void update_title(SDL_Window *window, const Scene *scene, int ssaa,
                         size_t triangles, double fps)
{
    char title[224];
    snprintf(title, sizeof(title),
             "Voxel (sequential) | %s | %zu tris | ssaa x%d | %.1f FPS",
             scene->scene == SCENE_CHUNK ? "Chunk" : block_get(scene->block_index)->name,
             triangles, ssaa, fps);
    SDL_SetWindowTitle(window, title);
}

static int run_interactive(const Options *opt)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Voxel (sequential)",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          opt->width, opt->height, 0);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
    SDL_Texture *screen = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                                                       SDL_TEXTUREACCESS_STREAMING,
                                                       opt->width, opt->height) : NULL;
    Framebuffer fb;
    TriangleBuffer tris;
    uint32_t *resolved = NULL;

    if (!window || !renderer || !screen || !alloc_frame_resources(opt, &fb, &resolved, &tris)) {
        if (window && renderer && screen)
            fprintf(stderr, "Out of memory allocating buffers\n");
        else
            fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        free(resolved);
        if (screen) SDL_DestroyTexture(screen);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Scene scene;
    if (!scene_init(&scene, opt->scene, opt->block, opt->chunks, opt->seed)) {
        free(resolved);
        tribuf_free(&tris);
        framebuffer_free(&fb);
        SDL_DestroyTexture(screen);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    unsigned seed = opt->seed;
    int ssaa = opt->ssaa;
    float yaw = 0.6f, pitch = 0.35f;
    float distance = scene_default_distance(&scene);
    int auto_rotate = 1;
    int dragging = 0;
    size_t triangles = 0;

    double last_time = now_seconds();
    double title_timer = 0.0;
    int frames_since_title = 0;
    int running = 1;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE || key == SDLK_q) {
                    running = 0;
                } else if (key == SDLK_TAB) {
                    scene.scene = (scene.scene == SCENE_CHUNK) ? SCENE_BLOCK : SCENE_CHUNK;
                    distance = scene_default_distance(&scene);
                } else if (key == SDLK_n) {
                    scene_reseed(&scene, ++seed);
                    scene.scene = SCENE_CHUNK;
                    distance = scene_default_distance(&scene);
                } else if (key >= SDLK_1 && key <= SDLK_9) {
                    int wanted = key - SDLK_1;
                    if (wanted < block_count()) {
                        scene.block_index = wanted;
                        scene.scene = SCENE_BLOCK;
                    }
                } else if (key == SDLK_RIGHTBRACKET) {
                    scene.block_index = (scene.block_index + 1) % block_count();
                } else if (key == SDLK_LEFTBRACKET) {
                    scene.block_index = (scene.block_index + block_count() - 1) % block_count();
                } else if (key == SDLK_SPACE) {
                    auto_rotate = !auto_rotate;
                } else if (key == SDLK_r) {
                    yaw = 0.6f; pitch = 0.35f;
                    distance = scene_default_distance(&scene);
                } else if (key == SDLK_PLUS || key == SDLK_EQUALS || key == SDLK_MINUS) {
                    int wanted = ssaa + (key == SDLK_MINUS ? -1 : 1);
                    if (wanted >= 1 && wanted <= 8) {
                        Framebuffer next;
                        if (framebuffer_init(&next, opt->width, opt->height, wanted)) {
                            framebuffer_free(&fb);
                            fb = next;
                            ssaa = wanted;
                        }
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                dragging = 1;
            } else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                dragging = 0;
            } else if (event.type == SDL_MOUSEMOTION && dragging) {
                yaw -= event.motion.xrel * 0.01f;
                pitch += event.motion.yrel * 0.01f;
                auto_rotate = 0;
            } else if (event.type == SDL_MOUSEWHEEL) {
                distance -= event.wheel.y * (distance * 0.08f);
            }
        }

        double current = now_seconds();
        float dt = (float)(current - last_time);
        last_time = current;

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT])  { yaw -= 1.5f * dt; auto_rotate = 0; }
        if (keys[SDL_SCANCODE_RIGHT]) { yaw += 1.5f * dt; auto_rotate = 0; }
        if (keys[SDL_SCANCODE_UP])    { pitch += 1.5f * dt; auto_rotate = 0; }
        if (keys[SDL_SCANCODE_DOWN])  { pitch -= 1.5f * dt; auto_rotate = 0; }
        if (keys[SDL_SCANCODE_W]) distance -= distance * 1.5f * dt;
        if (keys[SDL_SCANCODE_S]) distance += distance * 1.5f * dt;
        if (auto_rotate) yaw += 0.6f * dt;

        if (pitch > 1.4f) pitch = 1.4f;
        if (pitch < -1.4f) pitch = -1.4f;
        float max_distance = scene_max_distance(&scene);
        if (distance < 1.2f) distance = 1.2f;
        if (distance > max_distance) distance = max_distance;

        triangles = render_frame(&fb, &tris, &scene, yaw, pitch, distance);
        framebuffer_resolve(&fb, resolved);

        SDL_UpdateTexture(screen, NULL, resolved, opt->width * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, screen, NULL, NULL);
        SDL_RenderPresent(renderer);

        frames_since_title++;
        title_timer += dt;
        if (title_timer >= 0.4) {
            update_title(window, &scene, ssaa, triangles, frames_since_title / title_timer);
            title_timer = 0.0;
            frames_since_title = 0;
        }
    }

    free(resolved);
    tribuf_free(&tris);
    framebuffer_free(&fb);
    scene_free(&scene);
    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    Options opt = { 900, 700, 1, 0, SCENE_BLOCK, 1, 1337u, 0, NULL };

    textures_init();
    if (!parse_options(argc, argv, &opt))
        return 0;

    if (opt.dump_path)
        return run_dump(&opt);

    if (opt.bench_frames > 0)
        return run_benchmark(&opt);

    printf("Controls: drag/arrows orbit | W,S or wheel zoom | TAB block <-> chunk\n");
    printf("          1-9 pick block | [ ] cycle | +/- supersampling | N new seed | SPACE spin | R reset | ESC quit\n");
    return run_interactive(&opt);
}
