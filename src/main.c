#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "camera.h"
#include "math3d.h"
#include "overlay.h"
#include "render.h"
#include "texture.h"
#include "world.h"

#define PI 3.14159265358979323846f
#define MAX_PLAYERS 16
#define BENCH_DT (1.0f / 60.0f) /* fixed step: benchmarks must be reproducible */

enum { SCENE_BLOCK, SCENE_CHUNK };

typedef struct {
    int width;
    int height;
    int ssaa;
    int players;
    float view_distance;
    float roam;
    long max_chunks;
    unsigned seed;
    int block;
    int scene;
    int bench_frames;
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
} ViewTask;

typedef struct {
    int scene;
    int block_index;
    World *worlds;
    int world_count;
    ViewTask *views;
    int view_count;
    Vec3 light;
} Scene;

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
    scene->light = vec3_normalize(vec3_make(0.45f, 0.8f, 0.6f));

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

    /* Fly above the highest ground the generator can reach, not above the
     * average, or the explorers clip through peaks. */
    float flight_height = terrain_peak(&params) + 10.0f;

    scene->view_count = opt->players;
    for (int i = 0; i < scene->view_count; i++) {
        scene->views[i].camera = camera_make(i, scene->view_count, opt->roam,
                                             opt->view_distance, flight_height);
        scene->views[i].world = &scene->worlds[0];
        tribuf_init(&scene->views[i].triangles);
    }
    return 1;
}

/* --------------------------------------------------------------- rendering */

/* Renders one view. Everything it touches is private to that view or read-only,
 * which is what makes the loop over views safe to run in parallel. */
static void render_view(Framebuffer *fb, ViewTask *view, const Scene *scene, float t)
{
    Mat4 vp = camera_view_proj(&view->camera, t, viewport_aspect(&view->viewport));

    tribuf_clear(&view->triangles);
    if (scene->scene == SCENE_CHUNK) {
        Vec3 eye = camera_position(&view->camera, t);
        view->triangle_count = world_emit_view(&view->triangles, view->world, eye,
                                               view->camera.view_distance, vp,
                                               scene->light, &view->viewport);
    } else {
        view->triangle_count = cube_emit(&view->triangles, block_get(scene->block_index),
                                         mat4_identity(), vp, scene->light,
                                         FACE_ALL, &view->viewport);
    }
    raster_flush(fb, &view->triangles);
}

static size_t render_frame(Framebuffer *fb, Scene *scene, float t)
{
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

    /* Phase 2 -- rendering. The world is read-only from here on. */
    framebuffer_clear(fb, 0x0E1622);

    /* >>> The parallel decomposition lives here: this loop over independent
     * views is the one that becomes an OpenMP parallel for. <<< */
    for (int i = 0; i < scene->view_count; i++)
        render_view(fb, &scene->views[i], scene, t);

    size_t total = 0;
    for (int i = 0; i < scene->view_count; i++)
        total += scene->views[i].triangle_count;
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

/* --------------------------------------------------------------------- HUD */

#define HUD_SCALE 2
#define HUD_MARGIN 8
#define HUD_MIN_FPS 30.0

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
    snprintf(info, sizeof(info), "VIEWS %d  TRIS %zu  SSAA %d  SEQUENTIAL",
             scene->view_count, triangles, ssaa);
    int info_w = overlay_text_width(info, HUD_SCALE);

    char chunks[128];
    const World *world = &scene->worlds[0];
    snprintf(chunks, sizeof(chunks), "CHUNKS %zu  NEW %zu  DROPPED %zu",
             world_chunk_count(world), world->generated_this_frame,
             world->evicted_this_frame);
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
    printf("      --width N     window width  (default 960, minimum 640)\n");
    printf("      --height N    window height (default 720, minimum 480)\n");
    printf("      --ssaa N      supersampling factor, 1-8 (default 1)\n");
    printf("      --scene NAME  'chunk' or 'block' (default chunk)\n");
    printf("      --block N     block index for the block scene, 0-%d\n", block_count() - 1);
    printf("      --bench N     render N frames headless and report timings\n");
    printf("      --dump PATH   render one frame headless into a binary PPM file\n");
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

    size_t count = render_frame(&fb, &scene, 4.0f);
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

    printf("Benchmark: %dx%d, ssaa=%d (%dx%d internal), players=%d, view=%.0f, frames=%d\n",
           opt->width, opt->height, opt->ssaa, fb.fb_width, fb.fb_height,
           opt->players, (double)opt->view_distance, opt->bench_frames);

    size_t last_total = 0;
    double start = now_seconds();
    for (int frame = 0; frame < opt->bench_frames; frame++)
        last_total = render_frame(&fb, &scene, (float)frame * BENCH_DT);
    double elapsed = now_seconds() - start;

    double per_frame_ms = elapsed * 1000.0 / opt->bench_frames;
    printf("Triangles:  %zu (last frame, all views)\n", last_total);
    for (int i = 0; i < scene.view_count; i++)
        printf("  view %d: %zu triangles\n", i, scene.views[i].triangle_count);
    printf("Chunks:     %zu resident, %zu generated on the last frame\n",
           world_chunk_count(&scene.worlds[0]), scene.worlds[0].generated_this_frame);
    printf("Total:      %.4f s\n", elapsed);
    printf("Per frame:  %.4f ms  (%.2f FPS)\n", per_frame_ms, 1000.0 / per_frame_ms);

    scene_free(&scene);
    free(resolved);
    framebuffer_free(&fb);
    return 0;
}

/* ----------------------------------------------------------- interactive */

static int run_interactive(const Options *opt)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "Error: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Voxel Screensaver (sequential)",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          opt->width, opt->height, 0);
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

    double start_time = now_seconds();
    double last_time = start_time;
    double title_timer = 0.0;
    int frames_since_title = 0;
    size_t triangles = 0;
    double fps = 0.0;
    int running = 1;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = 0;
            else if (event.type == SDL_KEYDOWN &&
                     (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q))
                running = 0;
        }

        double current = now_seconds();
        double dt = current - last_time;
        last_time = current;

        /* Exponential moving average: a raw per-frame reciprocal jitters too
         * much to read, but a long window hides real stalls. */
        if (dt > 0.0) {
            double instant = 1.0 / dt;
            fps = (fps <= 0.0) ? instant : fps * 0.9 + instant * 0.1;
        }

        triangles = render_frame(&fb, &scene, (float)(current - start_time));
        draw_viewport_borders(&fb, &scene);
        framebuffer_resolve(&fb, resolved);
        draw_hud(resolved, opt->width, opt->height, &scene, fps, triangles, opt->ssaa);

        SDL_UpdateTexture(screen, NULL, resolved, opt->width * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, screen, NULL, NULL);
        SDL_RenderPresent(renderer);

        frames_since_title++;
        title_timer += dt;
        if (title_timer >= 0.4) {
            char title[224];
            snprintf(title, sizeof(title),
                     "Voxel Screensaver (sequential) | %d views | %zu tris | %.1f FPS",
                     scene.view_count, triangles, frames_since_title / title_timer);
            SDL_SetWindowTitle(window, title);
            title_timer = 0.0;
            frames_since_title = 0;
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
                    1337u, 0, SCENE_CHUNK, 0, NULL };

    textures_init();

    int status = parse_options(argc, argv, &opt);
    if (status == 0) return 0;
    if (status < 0) return 1;

    if (opt.dump_path)
        return run_dump(&opt);
    if (opt.bench_frames > 0)
        return run_benchmark(&opt);
    return run_interactive(&opt);
}
