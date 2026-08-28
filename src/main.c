#include <SDL2/SDL.h>

#ifdef _OPENMP
#include <omp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "camera.h"
#include "math3d.h"
#include "overlay.h"
#include "render.h"
#include "sky.h"
#include "texture.h"
#include "world.h"

#define PI 3.14159265358979323846f
#define MAX_PLAYERS 16
#define BENCH_DT (1.0f / 60.0f) /* fixed step: benchmarks must be reproducible */

/* Where the cycle starts at t = 0. Mid-morning, so a fresh run and every
 * headless dump open in daylight instead of the middle of the night. */
#define DAY_PHASE_START 0.28f

enum { SCENE_BLOCK, SCENE_CHUNK };

typedef struct {
    int width;
    int height;
    int ssaa;
    int players;
    float view_distance;
    float roam;
    long max_chunks;
    float day_length; /* seconds for a full day/night cycle */
    unsigned seed;
    int block;
    int scene;
    int bench_frames;
    int warmup_frames;
    int survey;      /* sample the generator over N x N blocks and report */
    int threads;     /* 0 = let OpenMP decide */
    int dynamic_schedule;
    const char *texture_pack;
    const char *dump_path;
} Options;

/* One camera rendering into one viewport, with its own triangle list.
 *
 * This is the unit of parallel work. Two views share nothing: disjoint
 * framebuffer rectangles, private triangle buffers, read-only access to the
 * world. Parallelizing the renderer means running the loop over ViewTasks
 * concurrently -- no locks, no atomics, no reduction.
 *
 * `world` is a pointer rather than a shared global on purpose: today every view
 * points at the same world, but pointing them at different worlds is all that
 * separates split-screen from independent sub-worlds. */
typedef struct {
    Camera camera;
    const World *world;
    Viewport viewport;
    TriangleBuffer triangles;
    size_t triangle_count;

    /* Stage timings live in the task, not in a shared accumulator. Threads
     * running this loop concurrently would race on a shared one, and the race
     * would quietly corrupt the very numbers the speedup is judged on. */
    double seconds_geometry;
    double seconds_raster;
    double seconds_sky;
} ViewTask;

typedef struct {
    int scene;
    int block_index;
    World *worlds;
    int world_count;
    ViewTask *views;
    int view_count;
    float day_length;
    Sky sky;          /* recomputed every frame from the elapsed time */
    char build_label[64];
} Scene;

static const char *build_label(const Options *opt, char *buffer, size_t size);

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* -------------------------------------------------------- viewport layout */

/* Tiles `count` viewports over the framebuffer in the squarest grid that fits.
 * Trailing rows absorb the remainder so no pixel column is left unpainted. */
static void layout_viewports(ViewTask *views, int count, int fb_width, int fb_height)
{
    int cols = (int)ceil(sqrt((double)count));
    if (cols < 1) cols = 1;
    int rows = (count + cols - 1) / cols;

    for (int i = 0; i < count; i++) {
        int col = i % cols;
        int row = i / cols;
        int x0 = fb_width * col / cols;
        int x1 = fb_width * (col + 1) / cols;
        int y0 = fb_height * row / rows;
        int y1 = fb_height * (row + 1) / rows;

        views[i].viewport.x = x0;
        views[i].viewport.y = y0;
        views[i].viewport.width = x1 - x0;
        views[i].viewport.height = y1 - y0;
    }
}

/* ------------------------------------------------------------------ scene */

static void scene_free(Scene *scene)
{
    if (scene->views) {
        for (int i = 0; i < scene->view_count; i++)
            tribuf_free(&scene->views[i].triangles);
        free(scene->views);
        scene->views = NULL;
    }
    if (scene->worlds) {
        for (int i = 0; i < scene->world_count; i++)
            world_free(&scene->worlds[i]);
        free(scene->worlds);
        scene->worlds = NULL;
    }
    scene->view_count = 0;
    scene->world_count = 0;
}

static int scene_init(Scene *scene, const Options *opt)
{
    memset(scene, 0, sizeof(*scene));
    scene->scene = opt->scene;
    scene->block_index = opt->block;
    scene->day_length = opt->day_length;
    build_label(opt, scene->build_label, sizeof(scene->build_label));
    scene->sky = sky_make(DAY_PHASE_START);

    /* One world today. Raising world_count and handing each view a different
     * entry is the whole change needed for independent sub-worlds. */
    scene->world_count = 1;
    scene->worlds = calloc((size_t)scene->world_count, sizeof(World));
    scene->views = calloc((size_t)opt->players, sizeof(ViewTask));
    if (!scene->worlds || !scene->views) {
        fprintf(stderr, "Error: out of memory allocating the scene\n");
        scene_free(scene);
        return 0;
    }

    TerrainParams params = terrain_default(opt->seed);
    if (!world_init(&scene->worlds[0], &params, (size_t)opt->max_chunks)) {
        fprintf(stderr, "Error: out of memory allocating the chunk map\n");
        scene_free(scene);
        return 0;
    }

    /* Above the ordinary terrain and above all but the rarest peaks. Clearing
     * the absolute maximum would put the explorers so high that the ground
     * became a distant texture. */
    float roam = opt->roam;
    float view_distance = opt->view_distance;
    float flight_height = terrain_peak(&params) * 0.78f + 7.0f;

    if (opt->scene == SCENE_BLOCK) {
        /* The block scene inspects one 1x1x1 cube at the origin. The terrain
         * flight parameters would orbit it from hundreds of units away, where
         * it covers no pixels at all, so the camera is brought in close. */
        roam = 2.6f;
        view_distance = 40.0f;
        flight_height = 1.1f;
    }

    scene->view_count = opt->players;
    for (int i = 0; i < scene->view_count; i++) {
        scene->views[i].camera = camera_make(i, scene->view_count, roam,
                                             view_distance, flight_height);
        if (opt->scene == SCENE_BLOCK) {
            /* Orbit the block and keep it in frame. */
            scene->views[i].camera.center = vec3_make(0.0f, 0.0f, 0.0f);
            scene->views[i].camera.height = 1.1f;
            scene->views[i].camera.bob_amplitude = 0.35f;
            scene->views[i].camera.look_at_center = 1;
        }
        scene->views[i].world = &scene->worlds[0];
        tribuf_init(&scene->views[i].triangles);
    }
    return 1;
}

/* --------------------------------------------------------------- rendering */

/* Per-stage wall clock for one frame. The three stages scale with different
 * inputs and parallelize differently, so the split is what tells you where to
 * spend effort -- a single frame time hides all of it. */
typedef struct {
    double stream;   /* generating and evicting chunks */
    double geometry; /* transform, cull, clip, project */
    double raster;   /* per-pixel fill, depth test, texturing */
    double sky;      /* background: only the pixels terrain left uncovered */
} FrameTimings;

/* Renders one view. Everything it touches is private to that view or read-only,
 * which is what makes the loop over views safe to run in parallel. */
static void render_view(Framebuffer *fb, ViewTask *view, const Scene *scene, float t,
                        int measure)
{
    Mat4 vp = camera_view_proj(&view->camera, t, viewport_aspect(&view->viewport));

    double t0 = measure ? now_seconds() : 0.0;

    tribuf_clear(&view->triangles);
    if (scene->scene == SCENE_CHUNK) {
        Vec3 eye = camera_position(&view->camera, t);
        view->triangle_count = world_emit_view(&view->triangles, view->world, eye,
                                               view->camera.view_distance, vp,
                                               &scene->sky.light, &view->viewport);
    } else {
        view->triangle_count = cube_emit(&view->triangles, block_get(scene->block_index),
                                         mat4_identity(), vp, &scene->sky.light,
                                         FACE_ALL, CUBE_NO_OCCLUSION,
                                         &view->viewport);
    }

    double t1 = measure ? now_seconds() : 0.0;
    raster_flush(fb, &view->triangles);
    double t2 = measure ? now_seconds() : 0.0;

    /* Sky last, so it only fills the pixels the terrain left at the far plane. */
    Vec3 sky_eye, forward, right, up;
    camera_basis(&view->camera, t, &sky_eye, &forward, &right, &up);
    sky_render(fb, &view->viewport, &scene->sky, sky_eye, forward, right, up,
               tanf(view->camera.fov_y_rad * 0.5f), viewport_aspect(&view->viewport));

    if (measure) {
        double t3 = now_seconds();
        view->seconds_geometry = t1 - t0;
        view->seconds_raster = t2 - t1;
        view->seconds_sky = t3 - t2;
    }
}

static size_t render_frame(Framebuffer *fb, Scene *scene, float t, FrameTimings *timings)
{
    double stream_start = timings ? now_seconds() : 0.0;

    /* Phase 1 -- streaming. Every explorer claims the chunks within its own
     * generation radius, and only once ALL of them have claimed does anything
     * get evicted: dropping after each explorer would throw away chunks the
     * next one still needs. This phase mutates the shared chunk map, so it
     * cannot simply be merged into the render loop below. */
    if (scene->scene == SCENE_CHUNK) {
        World *world = &scene->worlds[0];
        world_begin_frame(world);
        for (int i = 0; i < scene->view_count; i++) {
            Vec3 eye = camera_position(&scene->views[i].camera, t);
            world_stream_around(world, eye, scene->views[i].camera.generate_radius);
        }
        world_end_frame(world);
    }
    if (timings)
        timings->stream += now_seconds() - stream_start;

    /* The cycle advances with the same clock the explorers fly on, so a dump at
     * a given t always shows the same time of day. */
    scene->sky = sky_make(DAY_PHASE_START + t / scene->day_length);

    /* Phase 2 -- rendering. The world is read-only from here on. */
    framebuffer_clear(fb, 0x0E1622);

    /* >>> THE PARALLEL DECOMPOSITION <<<
     *
     * Views share nothing: disjoint framebuffer rectangles, private triangle
     * buffers, read-only access to the world. No lock, no atomic, no reduction.
     *
     * schedule(runtime) so the policy can be chosen from the command line:
     * per-view cost is deliberately uneven, which is exactly the case where
     * static and dynamic diverge. */
    int measure = (timings != NULL);
#ifdef _OPENMP
#pragma omp parallel for schedule(runtime)
#endif
    for (int i = 0; i < scene->view_count; i++)
        render_view(fb, &scene->views[i], scene, t, measure);

    size_t total = 0;
    for (int i = 0; i < scene->view_count; i++) {
        total += scene->views[i].triangle_count;
        if (measure) {
            /* Summed after the loop, never inside it. */
            timings->geometry += scene->views[i].seconds_geometry;
            timings->raster += scene->views[i].seconds_raster;
            timings->sky += scene->views[i].seconds_sky;
        }
    }
    return total;
}

/* Thin separators so the split-screen panes read as separate windows. */
static void draw_viewport_borders(Framebuffer *fb, const Scene *scene)
{
    if (scene->view_count < 2)
        return;

    const uint32_t border = 0x2C3A4E;
    for (int i = 0; i < scene->view_count; i++) {
        const Viewport *v = &scene->views[i].viewport;
        for (int x = v->x; x < v->x + v->width; x++) {
            fb->color[(size_t)v->y * fb->fb_width + (size_t)x] = border;
            fb->color[(size_t)(v->y + v->height - 1) * fb->fb_width + (size_t)x] = border;
        }
        for (int y = v->y; y < v->y + v->height; y++) {
            fb->color[(size_t)y * fb->fb_width + (size_t)v->x] = border;
            fb->color[(size_t)y * fb->fb_width + (size_t)(v->x + v->width - 1)] = border;
        }
    }
}

/* Reports which build is running and how it is configured. Also the single
 * place that knows whether OpenMP is compiled in at all. */
static const char *build_label(const Options *opt, char *buffer, size_t size)
{
#ifdef _OPENMP
    snprintf(buffer, size, "PARALLEL %dT %s", omp_get_max_threads(),
             opt->dynamic_schedule ? "DYNAMIC" : "STATIC");
#else
    (void)opt;
    snprintf(buffer, size, "SEQUENTIAL");
#endif
    return buffer;
}

/* Applies the thread count and schedule. A no-op in the sequential build, so
 * --threads and --schedule are accepted and ignored there rather than being an
 * error: the same command line then works against both binaries, which is what
 * makes a fair comparison easy to script. */
static void configure_parallelism(const Options *opt)
{
#ifdef _OPENMP
    if (opt->threads > 0)
        omp_set_num_threads(opt->threads);
    omp_set_schedule(opt->dynamic_schedule ? omp_sched_dynamic : omp_sched_static, 0);
#else
    (void)opt;
#endif
}

/* --------------------------------------------------------------------- HUD */

#define HUD_SCALE 2
#define HUD_MARGIN 8
#define HUD_MIN_FPS 30.0

/* Frame rate is averaged over a fixed WINDOW OF TIME, not with a per-frame
 * smoothing factor. A per-frame factor is frame-rate dependent: at 1 FPS a 0.9
 * weight remembers several seconds, so one bad sample survives for ages. A time
 * window behaves the same at 200 FPS and at 2 FPS. */
#define FPS_WINDOW_SECONDS 0.35

/* Draws the on-screen readout. The FPS number turns red below 30 because that
 * is the floor the project has to hold, so a failing frame rate is visible at a
 * glance instead of having to be read off a number.
 *
 * A negative `fps` means "not measured" and renders as dashes. Headless dumps
 * pass it: a frame rate derived from wall-clock time would differ on every run
 * and make the byte-identical --dump comparison useless, and a single-frame
 * reading is meaningless anyway because it includes the initial streaming. */
static void draw_hud(uint32_t *pixels, int width, int height, const Scene *scene,
                     double fps, size_t triangles, int ssaa)
{
    char line[128];
    int line_h = OVERLAY_GLYPH_H * HUD_SCALE;

    if (fps < 0.0)
        snprintf(line, sizeof(line), "FPS ---");
    else
        snprintf(line, sizeof(line), "FPS %.1f", fps);
    int fps_w = overlay_text_width(line, HUD_SCALE);

    char info[128];
    snprintf(info, sizeof(info), "VIEWS %d  TRIS %zu  SSAA %d  %s",
             scene->view_count, triangles, ssaa, scene->build_label);
    int info_w = overlay_text_width(info, HUD_SCALE);

    char chunks[128];
    const World *world = &scene->worlds[0];
    float hours = scene->sky.phase * 24.0f;
    snprintf(chunks, sizeof(chunks), "CHUNKS %zu  NEW %zu  %02d:%02d",
             world_chunk_count(world), world->generated_this_frame,
             (int)hours, (int)((hours - (int)hours) * 60.0f));
    int chunks_w = overlay_text_width(chunks, HUD_SCALE);

    int widest = fps_w;
    if (info_w > widest) widest = info_w;
    if (chunks_w > widest) widest = chunks_w;

    int panel_w = widest + 2 * HUD_MARGIN;
    int panel_h = 3 * line_h + 2 * HUD_MARGIN;
    overlay_panel(pixels, width, height, 0, 0, panel_w, panel_h, 0x000000, 150);

    uint32_t fps_color = (fps < 0.0) ? 0xD8E4F0
                       : (fps < HUD_MIN_FPS) ? 0xFF5C5C : 0x7CFF9A;
    overlay_text(pixels, width, height, HUD_MARGIN, HUD_MARGIN / 2,
                 HUD_SCALE, fps_color, line);
    overlay_text(pixels, width, height, HUD_MARGIN, HUD_MARGIN / 2 + line_h + 2,
                 HUD_SCALE, 0xD8E4F0, info);
    overlay_text(pixels, width, height, HUD_MARGIN, HUD_MARGIN / 2 + 2 * (line_h + 2),
                 HUD_SCALE, 0x9FC6E8, chunks);

    if (scene->view_count < 2)
        return;

    /* Per-view label. Viewports live in supersampled coordinates, so they are
     * divided down to the resolved image the HUD is drawn on. */
    for (int i = 0; i < scene->view_count; i++) {
        const Viewport *v = &scene->views[i].viewport;
        int vx = v->x / ssaa;
        int vy = v->y / ssaa;

        snprintf(line, sizeof(line), "P%d  %zu TRIS", i, scene->views[i].triangle_count);
        int w = overlay_text_width(line, 1);
        int x = vx + 6;
        int y = vy + v->height / ssaa - OVERLAY_GLYPH_H - 6;

        overlay_panel(pixels, width, height, x - 3, y - 3,
                      w + 6, OVERLAY_GLYPH_H + 6, 0x000000, 130);
        overlay_text(pixels, width, height, x, y, 1, 0xC9D8E8, line);
    }
}

/* ------------------------------------------------------------------- CLI */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -n, --players N   explorers rendered in split-screen, 1-%d (default 1)\n", MAX_PLAYERS);
    printf("      --view N      render distance per explorer, world units (default 96)\n");
    printf("      --roam N      radius of the flight path, world units (default 320)\n");
    printf("      --max-chunks N  ceiling on resident chunks (default %d)\n", WORLD_DEFAULT_MAX_CHUNKS);
    printf("      --seed N      terrain seed (default 1337)\n");
    printf("      --daylen N    seconds for a full day/night cycle (default 180)\n");
    printf("      --width N     window width  (default 960, minimum 640)\n");
    printf("      --height N    window height (default 720, minimum 480)\n");
    printf("      --ssaa N      supersampling factor, 1-8 (default 1)\n");
    printf("      --scene NAME  'chunk' or 'block' (default chunk)\n");
    printf("      --block N     block index for the block scene, 0-%d\n", block_count() - 1);
    printf("      --bench N     render N frames headless and report timings\n");
    printf("      --warmup N    untimed frames before the benchmark (default 3)\n");
    printf("      --textures P  texture atlas from scripts/make_texture_atlas.py\n");
    printf("      --dump PATH   render one frame headless into a binary PPM file\n");
    printf("      --survey N    sample terrain over an NxN block area and report\n");
    printf("      --threads N   OpenMP threads (default: all cores; 0 = auto)\n");
    printf("      --schedule S  'static' or 'dynamic' loop schedule (default static)\n");
    printf("      --help        show this message\n");
}

/* Parses one integer argument with range checking. Returns 0 and complains on
 * anything that is not a well-formed number inside [low, high]. */
static int parse_int(const char *text, const char *flag, long low, long high, long *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        fprintf(stderr, "Error: %s expects an integer, got '%s'\n", flag, text);
        return 0;
    }
    if (value < low || value > high) {
        fprintf(stderr, "Error: %s must be between %ld and %ld, got %ld\n", flag, low, high, value);
        return 0;
    }
    *out = value;
    return 1;
}

/* Returns 1 to run, 0 to exit cleanly (--help), -1 on a bad argument. */
static int parse_options(int argc, char **argv, Options *opt)
{
    for (int i = 1; i < argc; i++) {
        const char *flag = argv[i];
        int has_value = (i + 1 < argc);
        long value = 0;

        if (!strcmp(flag, "--help") || !strcmp(flag, "-h")) {
            print_usage(argv[0]);
            return 0;
        }

        if (!has_value) {
            fprintf(stderr, "Error: %s requires a value\n", flag);
            return -1;
        }

        if (!strcmp(flag, "--players") || !strcmp(flag, "-n")) {
            if (!parse_int(argv[++i], flag, 1, MAX_PLAYERS, &value)) return -1;
            opt->players = (int)value;
        } else if (!strcmp(flag, "--view")) {
            if (!parse_int(argv[++i], flag, 8, 2000, &value)) return -1;
            opt->view_distance = (float)value;
        } else if (!strcmp(flag, "--roam")) {
            if (!parse_int(argv[++i], flag, 0, 100000, &value)) return -1;
            opt->roam = (float)value;
        } else if (!strcmp(flag, "--max-chunks")) {
            if (!parse_int(argv[++i], flag, 64, 200000, &value)) return -1;
            opt->max_chunks = value;
        } else if (!strcmp(flag, "--daylen")) {
            if (!parse_int(argv[++i], flag, 4, 100000, &value)) return -1;
            opt->day_length = (float)value;
        } else if (!strcmp(flag, "--seed")) {
            if (!parse_int(argv[++i], flag, 0, 2147483647L, &value)) return -1;
            opt->seed = (unsigned)value;
        } else if (!strcmp(flag, "--width")) {
            if (!parse_int(argv[++i], flag, 640, 7680, &value)) return -1;
            opt->width = (int)value;
        } else if (!strcmp(flag, "--height")) {
            if (!parse_int(argv[++i], flag, 480, 4320, &value)) return -1;
            opt->height = (int)value;
        } else if (!strcmp(flag, "--ssaa")) {
            if (!parse_int(argv[++i], flag, 1, 8, &value)) return -1;
            opt->ssaa = (int)value;
        } else if (!strcmp(flag, "--block")) {
            if (!parse_int(argv[++i], flag, 0, block_count() - 1, &value)) return -1;
            opt->block = (int)value;
        } else if (!strcmp(flag, "--bench")) {
            if (!parse_int(argv[++i], flag, 1, 100000, &value)) return -1;
            opt->bench_frames = (int)value;
        } else if (!strcmp(flag, "--warmup")) {
            if (!parse_int(argv[++i], flag, 0, 10000, &value)) return -1;
            opt->warmup_frames = (int)value;
        } else if (!strcmp(flag, "--scene")) {
            const char *name = argv[++i];
            if (!strcmp(name, "chunk")) {
                opt->scene = SCENE_CHUNK;
            } else if (!strcmp(name, "block")) {
                opt->scene = SCENE_BLOCK;
            } else {
                fprintf(stderr, "Error: --scene expects 'chunk' or 'block', got '%s'\n", name);
                return -1;
            }
        } else if (!strcmp(flag, "--textures")) {
            opt->texture_pack = argv[++i];
        } else if (!strcmp(flag, "--threads")) {
            if (!parse_int(argv[++i], flag, 0, 1024, &value)) return -1;
            opt->threads = (int)value;
        } else if (!strcmp(flag, "--schedule")) {
            const char *name = argv[++i];
            if (!strcmp(name, "dynamic")) {
                opt->dynamic_schedule = 1;
            } else if (!strcmp(name, "static")) {
                opt->dynamic_schedule = 0;
            } else {
                fprintf(stderr, "Error: --schedule expects 'static' or 'dynamic', got '%s'\n", name);
                return -1;
            }
        } else if (!strcmp(flag, "--survey")) {
            if (!parse_int(argv[++i], flag, 16, 8192, &value)) return -1;
            opt->survey = (int)value;
        } else if (!strcmp(flag, "--dump")) {
            opt->dump_path = argv[++i];
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", flag);
            print_usage(argv[0]);
            return -1;
        }
    }
    return 1;
}

/* --------------------------------------------------------- headless modes */

static int write_ppm(const char *path, const uint32_t *pixels, int width, int height)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", path);
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

static int alloc_frame_resources(const Options *opt, Framebuffer *fb, uint32_t **resolved)
{
    *resolved = malloc((size_t)opt->width * opt->height * sizeof(uint32_t));
    if (!framebuffer_init(fb, opt->width, opt->height, opt->ssaa) || !*resolved) {
        fprintf(stderr, "Error: out of memory allocating a %dx%d framebuffer at ssaa %d\n",
                opt->width, opt->height, opt->ssaa);
        free(*resolved);
        *resolved = NULL;
        return 0;
    }
    return 1;
}

static int run_dump(const Options *opt)
{
    Framebuffer fb;
    uint32_t *resolved;
    Scene scene;

    if (!alloc_frame_resources(opt, &fb, &resolved))
        return 1;
    if (!scene_init(&scene, opt)) {
        free(resolved);
        framebuffer_free(&fb);
        return 1;
    }
    layout_viewports(scene.views, scene.view_count, fb.fb_width, fb.fb_height);

    /* A dumped frame is the correctness reference the parallel build is diffed
     * against, so it must hold SCENE CONTENT ONLY. Anything that varies with
     * the build, the clock or the configuration has to stay out of the image:
     * the frame rate already shows as dashes, and the build label would
     * otherwise read "SEQUENTIAL" in one binary and "PARALLEL 8T STATIC" in the
     * other, failing every comparison on text rather than on pixels. */
    snprintf(scene.build_label, sizeof(scene.build_label), "REFERENCE");

    size_t count = render_frame(&fb, &scene, 4.0f, NULL);
    draw_viewport_borders(&fb, &scene);
    framebuffer_resolve(&fb, resolved);
    draw_hud(resolved, opt->width, opt->height, &scene, -1.0, count, opt->ssaa);

    int ok = write_ppm(opt->dump_path, resolved, opt->width, opt->height);
    if (ok)
        printf("Wrote %s (%dx%d, %d views, %zu triangles)\n",
               opt->dump_path, opt->width, opt->height, scene.view_count, count);

    scene_free(&scene);
    free(resolved);
    framebuffer_free(&fb);
    return ok ? 0 : 1;
}

static int run_benchmark(const Options *opt)
{
    Framebuffer fb;
    uint32_t *resolved;
    Scene scene;

    if (!alloc_frame_resources(opt, &fb, &resolved))
        return 1;
    if (!scene_init(&scene, opt)) {
        free(resolved);
        framebuffer_free(&fb);
        return 1;
    }
    layout_viewports(scene.views, scene.view_count, fb.fb_width, fb.fb_height);

    char label[64];
    printf("Benchmark: %dx%d, ssaa=%d (%dx%d internal), players=%d, view=%.0f, frames=%d\n",
           opt->width, opt->height, opt->ssaa, fb.fb_width, fb.fb_height,
           opt->players, (double)opt->view_distance, opt->bench_frames);
    printf("Build:     %s\n", build_label(opt, label, sizeof(label)));

    /* Untimed warmup. The very first frames stream thousands of chunks at once,
     * and averaging that startup cost into the steady-state figure understates
     * the frame rate the screensaver actually sustains. */
    for (int frame = 0; frame < opt->warmup_frames; frame++)
        render_frame(&fb, &scene, (float)frame * BENCH_DT, NULL);
    if (opt->warmup_frames > 0)
        printf("Warmup:     %d frames (untimed)\n", opt->warmup_frames);

    size_t last_total = 0;
    FrameTimings timings = { 0.0, 0.0, 0.0, 0.0 };
    double start = now_seconds();
    for (int frame = 0; frame < opt->bench_frames; frame++)
        last_total = render_frame(&fb, &scene,
                                  (float)(opt->warmup_frames + frame) * BENCH_DT,
                                  &timings);
    double elapsed = now_seconds() - start;

    double per_frame_ms = elapsed * 1000.0 / opt->bench_frames;
    printf("Triangles:  %zu (last frame, all views)\n", last_total);
    for (int i = 0; i < scene.view_count; i++)
        printf("  view %d: %zu triangles\n", i, scene.views[i].triangle_count);
    printf("Chunks:     %zu resident, %zu generated on the last frame\n",
           world_chunk_count(&scene.worlds[0]), scene.worlds[0].generated_this_frame);
    printf("Total:      %.4f s\n", elapsed);
    printf("Per frame:  %.4f ms  (%.2f FPS)\n", per_frame_ms, 1000.0 / per_frame_ms);
    /* With more than one thread these are AGGREGATE WORK across threads, not
     * wall clock: several views run at once, so the parts can exceed the whole.
     * They still say where the work is, which is what they are for. */
#ifdef _OPENMP
    if (omp_get_max_threads() > 1 && opt->players > 1)
        printf("  (stage times below are aggregate work across threads)\n");
#endif
    printf("  stream:   %.4f ms  (%.1f%%)\n",
           timings.stream * 1000.0 / opt->bench_frames, 100.0 * timings.stream / elapsed);
    printf("  geometry: %.4f ms  (%.1f%%)\n",
           timings.geometry * 1000.0 / opt->bench_frames, 100.0 * timings.geometry / elapsed);
    printf("  raster:   %.4f ms  (%.1f%%)\n",
           timings.raster * 1000.0 / opt->bench_frames, 100.0 * timings.raster / elapsed);
    printf("  sky:      %.4f ms  (%.1f%%)\n",
           timings.sky * 1000.0 / opt->bench_frames, 100.0 * timings.sky / elapsed);

    scene_free(&scene);
    free(resolved);
    framebuffer_free(&fb);
    return 0;
}

/* Samples the generator over a square area and reports what it actually
 * produces. Arguing about whether terrain "looks flat" is guesswork; a height
 * histogram and a biome census are not. */
static int run_survey(const Options *opt)
{
    TerrainParams params = terrain_default(opt->seed);
    int span = opt->survey;
    int step = span > 512 ? span / 512 : 1;

    const char *biome_names[] = { "ocean", "beach", "desert", "plains",
                                  "forest", "tundra", "mountain" };
    size_t biome_count[7] = { 0 };
    size_t samples = 0;
    int min_height = CHUNK_SIZE_Y, max_height = 0;
    double sum = 0.0;

    int buckets[CHUNK_SIZE_Y];
    memset(buckets, 0, sizeof(buckets));

    for (int z = -span / 2; z < span / 2; z += step) {
        for (int x = -span / 2; x < span / 2; x += step) {
            TerrainSample sample = terrain_sample(&params, x, z);
            buckets[sample.height]++;
            biome_count[sample.biome]++;
            if (sample.height < min_height) min_height = sample.height;
            if (sample.height > max_height) max_height = sample.height;
            sum += sample.height;
            samples++;
        }
    }

    printf("Terrain survey: %dx%d blocks, seed %u, %zu samples\n",
           span, span, opt->seed, samples);
    printf("Height: min %d, max %d, mean %.1f, sea level %d, snow line %d\n\n",
           min_height, max_height, sum / (double)samples,
           params.sea_level, params.snow_line);

    printf("Height distribution\n");
    for (int h = 0; h < CHUNK_SIZE_Y; h++) {
        if (buckets[h] == 0)
            continue;
        double pct = 100.0 * (double)buckets[h] / (double)samples;
        printf("  %3d %s%5.1f%%", h, h == params.sea_level ? "<-sea " : "      ", pct);
        int bar = (int)(pct * 2.0);
        for (int i = 0; i < bar && i < 60; i++)
            putchar('#');
        putchar('\n');
    }

    printf("\nBiomes\n");
    for (int b = 0; b < 7; b++)
        printf("  %-9s %5.1f%%\n", biome_names[b],
               100.0 * (double)biome_count[b] / (double)samples);
    return 0;
}

/* ----------------------------------------------------------- interactive */

/* Reallocates everything that depends on the window size: framebuffer, resolve
 * buffer, streaming texture and viewport layout.
 *
 * The new surfaces are allocated BEFORE the old ones are released, and the swap
 * only happens once all three succeed. A resize that runs out of memory then
 * leaves the program running at the previous size instead of tearing down
 * buffers it cannot replace. */
static int resize_surfaces(SDL_Renderer *renderer, SDL_Texture **screen,
                           Framebuffer *fb, uint32_t **resolved, Scene *scene,
                           int width, int height, int ssaa)
{
    Framebuffer next_fb;
    if (!framebuffer_init(&next_fb, width, height, ssaa)) {
        fprintf(stderr, "Warning: cannot resize to %dx%d at ssaa %d, keeping %dx%d\n",
                width, height, ssaa, fb->width, fb->height);
        return 0;
    }

    uint32_t *next_resolved = malloc((size_t)width * height * sizeof(uint32_t));
    SDL_Texture *next_screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 width, height);
    if (!next_resolved || !next_screen) {
        fprintf(stderr, "Warning: cannot resize to %dx%d, keeping %dx%d\n",
                width, height, fb->width, fb->height);
        free(next_resolved);
        if (next_screen) SDL_DestroyTexture(next_screen);
        framebuffer_free(&next_fb);
        return 0;
    }

    framebuffer_free(fb);
    free(*resolved);
    SDL_DestroyTexture(*screen);

    *fb = next_fb;
    *resolved = next_resolved;
    *screen = next_screen;

    /* Panes are re-tiled over the new framebuffer; each viewport recomputes its
     * own aspect ratio, so the views are not stretched. */
    layout_viewports(scene->views, scene->view_count, fb->fb_width, fb->fb_height);
    return 1;
}

static int run_interactive(const Options *opt)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "Error: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Voxel Screensaver (sequential)",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          opt->width, opt->height,
                                          SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
    SDL_Texture *screen = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                                                       SDL_TEXTUREACCESS_STREAMING,
                                                       opt->width, opt->height) : NULL;
    if (!window || !renderer || !screen) {
        fprintf(stderr, "Error: SDL setup failed: %s\n", SDL_GetError());
        if (screen) SDL_DestroyTexture(screen);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Framebuffer fb;
    uint32_t *resolved;
    Scene scene;
    if (!alloc_frame_resources(opt, &fb, &resolved) || !scene_init(&scene, opt)) {
        free(resolved);
        framebuffer_free(&fb);
        SDL_DestroyTexture(screen);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    layout_viewports(scene.views, scene.view_count, fb.fb_width, fb.fb_height);

    /* The rubric sets a 640x480 floor; enforcing it on the window stops the
     * user dragging below what the CLI already refuses. */
    SDL_SetWindowMinimumSize(window, 640, 480);

    int win_w = opt->width;
    int win_h = opt->height;
    int ssaa = opt->ssaa;
    int show_hud = 1;
    int fullscreen = 0;

    double start_time = now_seconds();
    double window_start = start_time;
    int window_frames = 0;
    size_t triangles = 0;
    double fps = -1.0; /* negative until the first window closes: no fake reading */
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
                } else if (key == SDLK_h || key == SDLK_F1) {
                    show_hud = !show_hud;
                } else if (key == SDLK_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window,
                        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
            } else if (event.type == SDL_WINDOWEVENT &&
                       event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                int new_w = event.window.data1;
                int new_h = event.window.data2;
                if (new_w > 0 && new_h > 0 && (new_w != win_w || new_h != win_h)) {
                    if (resize_surfaces(renderer, &screen, &fb, &resolved, &scene,
                                        new_w, new_h, ssaa)) {
                        win_w = new_w;
                        win_h = new_h;
                    }
                }
            }
        }

        double current = now_seconds();

        triangles = render_frame(&fb, &scene, (float)(current - start_time), NULL);
        draw_viewport_borders(&fb, &scene);
        framebuffer_resolve(&fb, resolved);
        if (show_hud)
            draw_hud(resolved, win_w, win_h, &scene, fps, triangles, ssaa);

        SDL_UpdateTexture(screen, NULL, resolved, win_w * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, screen, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Counted after the frame is actually on screen, so the reading covers
         * the whole frame: simulation, geometry, rasterization and present. */
        window_frames++;
        double window_elapsed = now_seconds() - window_start;
        if (window_elapsed >= FPS_WINDOW_SECONDS) {
            fps = (double)window_frames / window_elapsed;
            window_start = now_seconds();
            window_frames = 0;

            char title[224];
            snprintf(title, sizeof(title),
                     "Voxel Screensaver (sequential) | %d views | %zu tris | %.1f FPS",
                     scene.view_count, triangles, fps);
            SDL_SetWindowTitle(window, title);
        }
    }

    scene_free(&scene);
    free(resolved);
    framebuffer_free(&fb);
    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    Options opt = { 960, 720, 1, 1, 96.0f, 320.0f, WORLD_DEFAULT_MAX_CHUNKS,
                    180.0f, 1337u, 0, SCENE_CHUNK, 0, 3, 0, 0, 0, NULL, NULL };

    textures_init();

    int status = parse_options(argc, argv, &opt);
    if (status == 0) return 0;
    if (status < 0) return 1;

    /* A pack that fails to load leaves the procedural set in place, so the
     * program still runs; the loader has already explained why on stderr. */
    if (opt.texture_pack)
        textures_load_atlas(opt.texture_pack);

    configure_parallelism(&opt);

    if (opt.survey > 0) {
        int survey_result = run_survey(&opt);
        textures_free();
        return survey_result;
    }

    int result;
    if (opt.dump_path) {
        result = run_dump(&opt);
    } else if (opt.bench_frames > 0) {
        result = run_benchmark(&opt);
    } else {
        printf("Controls: H or F1 toggles the HUD | F11 fullscreen | Esc or Q quits\n");
        printf("          the window is resizable; panes re-tile and keep their aspect\n");
        result = run_interactive(&opt);
    }

    textures_free();
    return result;
}
