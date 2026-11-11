/* Fixed-function OpenGL implementation of render.h, shared by both platforms:
 *   PC    SDL2 window + desktop GL (vsync);
 *   Vita  vitaGL (H3), whose fixed-function emulation compiles its shaders at run
 *         time with ur0:data/libshacccg.suprx. The 640x480 game fills the 960x544
 *         screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <pthread.h>
#include <vitaGL.h>
#else
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#endif

#include "input_map.h"
#include "net.h"
#include "render.h"
#include "win32.h"

static uint32_t screen_w, screen_h, fvf;
static uint32_t states[256];
static RenderTexture *bound;
static int quit, ready;
static int mag_linear, min_linear;

/* -- platform glue: context, presentation, input ------------------------------------ */
#ifdef __vita__
/* The 640x480 game keeps its 4:3 shape in the middle of the 16:9 screen (725x544 at
   native resolution), with black side bars. */
/* Render resolution: native 960x544 by default. The display unit scales 720x408, 640x368
   and 480x272 to the screen, so a lower one is cheaper to fill and just looks coarser;
   res.txt in the data directory holds 720, 640 or 480 to pick one. */
static int vita_w = 960, vita_h = 544;
static int wide_view; /* wide 960x544 fight view, see wide_wanted() */
#define VITA_W vita_w
#define VITA_H vita_h
#define VIEW_W (wide_view ? vita_w : vita_h * 4 / 3)
#define VIEW_X (wide_view ? 0 : (vita_w - VIEW_W) / 2)
/* vitaGL has no per-thread context, but it is not thread-safe: the device is created
   on one guest thread and two others draw, so every call goes through one lock. */
static pthread_mutex_t gl_lock = PTHREAD_MUTEX_INITIALIZER;
static void gl_enter(void) { pthread_mutex_lock(&gl_lock); }
static void gl_leave(void) { pthread_mutex_unlock(&gl_lock); }
#define blend_equation glBlendEquation

static int platform_init(uint32_t width, uint32_t height) {
    /* Texture storage is reserved first, then vitaGL gets fixed pools: 8 MiB of the
       ~32 MiB user RAM left (the rest is for ~20 thread stacks and file buffers), 80 MiB
       of CDRAM (the rest, ~16 MiB, grows the texture pool) and 22 MiB of physically
       contiguous memory (~4 MiB left). With 64 + 16 MiB at 960x544 (larger display and
       depth buffers) a heavy stage ran vitaGL's free memory down to ~3 MiB with only
       28 MiB of textures loaded: 100 evictions and up to 190 re-uploads per 10 s, and the
       fight fell to 15 FPS with 26 MiB of CDRAM and 10 MiB of phycont never used; in a
       15 minute session the texture pool grew by one 4 MiB chunk. */
    texture_pool_reserve();
    char memory[256];
    platform_clock_report(memory, sizeof memory);
    trace("# clocks: %s", memory);
    platform_memory_report(memory, sizeof memory);
    trace("# before vitaGL: %s", memory);
    /* Explicit pool sizes (the threshold variant derives them from the remaining
       budget). The return value is not a success flag: vitaGL returns whether it had
       to lower the resolution, i.e. GL_FALSE after a normal start, so it is ignored. */
    /* Triple buffering (before vglInit): with two buffers and vsync a logic step of 17 ms
       missed the 16.7 ms vblank and waited until 33 ms, which also tripped the frame skip
       (heartbeats showed ~58 steps but only ~42 frames drawn per second). */
    vglSetDisplayBufferCount(3);
    {
        FILE *rf = fopen("ux0:data/touhou123vita/res.txt", "rb");
        int want = 0;
        if (rf) { /* (fscanf pulled in libc code that vita-elf-create crashed on) */
            char text[16] = "";
            if (fgets(text, sizeof text, rf)) want = atoi(text);
            fclose(rf);
        }
        if (want == 720) { vita_w = 720; vita_h = 408; }
        else if (want == 640) { vita_w = 640; vita_h = 368; }
        else if (want == 480) { vita_w = 480; vita_h = 272; }
        else { vita_w = 960; vita_h = 544; }
        trace("# display: render resolution %dx%d (res.txt: %d)", vita_w, vita_h, want);
    }
    vglInitWithCustomSizes(0, VITA_W, VITA_H, 8 << 20, 80 << 20, 22 << 20, 0, SCE_GXM_MULTISAMPLE_NONE);
    {   /* vsync-off.txt in the data directory: present without waiting for the vblank (tearing) */
        FILE *vs = fopen("ux0:data/touhou123vita/vsync-off.txt", "rb");
        if (vs) { fclose(vs); vglWaitVblankStart(GL_FALSE); }
        trace("# display: 3 buffers, vsync %s", vs ? "off (vsync-off.txt)" : "on");
    }
    platform_memory_report(memory, sizeof memory);
    trace("# after vitaGL: %s", memory);
    platform_display_taken();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(VIEW_X, 0, VIEW_W, VITA_H);
    /* vitaGL does not clip to the viewport: keep sprites that slide in from outside the
       640x480 screen off the side bars */
    glEnable(GL_SCISSOR_TEST);
    glScissor(VIEW_X, 0, VIEW_W, VITA_H);
    (void)width; (void)height;
    return 0;
}
/* GL_TRUE while a system dialog (the keyboard) is on screen: vitaGL then lets the
   common dialog draw over the frame and updates it */
static void platform_swap(void) { vglSwapBuffers(ime_dialog_active() ? GL_TRUE : GL_FALSE); }
int render_pump_events(void) { return 0; }
void render_keyboard_state(uint8_t dik[256]) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    /* every change goes to the trace: the idle game left the title on its own on the
       Vita but not on PC, so check what the pad reports */
    static uint32_t last = 0xFFFFFFFFu;
    if (pad.buttons != last) {
        trace("# vita buttons: %08x (frame %llu)", (unsigned)pad.buttons, d3d_frames);
        last = pad.buttons;
    }
    /* START + SELECT held for 2 s ends the run and writes the trace, capture and result */
    static uint64_t held_since;
    uint32_t combo = SCE_CTRL_START | SCE_CTRL_SELECT;
    if ((pad.buttons & combo) == combo) {
        uint64_t now = platform_ticks_us();
        if (!held_since) held_since = now;
        else if (now - held_since > 2000000u) host_exit(NULL, 0, "START+SELECT pressed");
    } else {
        held_since = 0;
    }
    dik_state_from_vita_buttons(pad.buttons, dik);
}
#else
static SDL_Window *window;
static SDL_GLContext context;
static void (APIENTRY *blend_equation)(GLenum);

/* Desktop GL contexts are per thread: every render call takes the context under a
   lock and releases it on the way out (wglMakeCurrent refuses a context current on
   another thread). */
static SDL_mutex *gl_lock;
static void *gl_holder; /* thread that has the context current, or NULL */
static _Thread_local char gl_me;

static void gl_enter(void) {
    SDL_LockMutex(gl_lock);
    if (gl_holder == &gl_me)
        return;
    if (SDL_GL_MakeCurrent(window, context) == 0)
        gl_holder = &gl_me;
    else
        trace("# GL context unavailable on host thread %lu: %s", SDL_ThreadID(), SDL_GetError());
}
static void gl_leave(void) {
    SDL_GL_MakeCurrent(window, NULL);
    gl_holder = NULL;
    SDL_UnlockMutex(gl_lock);
}

static int platform_init(uint32_t width, uint32_t height) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        trace("# SDL_Init failed: %s", SDL_GetError());
        return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window = SDL_CreateWindow("Touhou 12.3 (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              (int)width, (int)height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window || !(context = SDL_GL_CreateContext(window))) {
        trace("# window/GL context failed: %s", SDL_GetError());
        return -1;
    }
    SDL_GL_SetSwapInterval(1);
    blend_equation = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glBlendEquation");
    gl_lock = SDL_CreateMutex();
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    return 0;
}
static void platform_swap(void) { SDL_GL_SwapWindow(window); }

int render_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT)
            quit = 1;
    return quit;
}

/* T123_PRESS="first-last:scancode[,...]" holds an SDL scancode down during those
   presented frames, so runs can be scripted without a person at the window. */
static void scripted_keys(uint8_t dik[256]) {
    const char *p = getenv("T123_PRESS");
    while (p && *p) {
        unsigned long long first, last;
        int sc, used = 0;
        if (sscanf(p, "%llu-%llu:%d%n", &first, &last, &sc, &used) != 3)
            return;
        if (d3d_frames >= first && d3d_frames <= last && dik_from_sdl_scancode(sc))
            dik[dik_from_sdl_scancode(sc)] = 0x80;
        p += used;
        if (*p == ',') p++;
    }
}

void render_keyboard_state(uint8_t dik[256]) {
    scripted_keys(dik);
    int count = 0;
    const Uint8 *keys = SDL_GetKeyboardState(&count);
    for (int sc = 0; sc < count; sc++)
        if (keys[sc]) {
            uint8_t code = dik_from_sdl_scancode(sc);
            if (code) dik[code] = 0x80;
        }
}
#endif

int render_quit_requested(void) { return quit; }

/* -- common ------------------------------------------------------------------------- */
#ifndef __vita__
static int wide_view;
#endif

/* Wide view: the game keeps drawing in its 640x480 coordinates and the screen shows -160..800 x
   -32..512 of them at 1:1 on 960x544, so the HUD and menus stay centred while the camera, widened by
   recomp/lift_all.py --wide (built with -DT123_WIDE_LIFT), fills the rest in a fight. On by
   default in such a build; 4x3.txt in the data directory (Vita) or T123_NARROW (PC) turns it off. */
static int wide_wanted(void) {
#ifndef T123_WIDE_LIFT
    return 0;
#elif defined(__vita__)
    FILE *f = fopen("ux0:data/touhou123vita/4x3.txt", "rb");
    if (f) fclose(f);
    return f == NULL;
#else
    return getenv("T123_NARROW") == NULL;
#endif
}
#ifdef __vita__
#define FB_W vita_w
#define FB_H vita_h
#else
#define FB_W screen_w
#define FB_H screen_h
#endif

/* In a fight, a quad covering exactly the game's 640x480 screen (fades, dimming, flashes)
   is stretched over the whole wide view. Menus keep their centred 640x480. */
static void widen_fullscreen_quad(float *pos, uint32_t n) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < n; i++) {
        float x = pos[i * 3] - 0.5f, y = pos[i * 3 + 1] - 0.5f;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    if (fabsf(x0) > 2 || fabsf(y0) > 2 || fabsf(x1 - 640) > 2 || fabsf(y1 - 480) > 2) return;
    for (uint32_t i = 0; i < n; i++) {
        float *x = &pos[i * 3], *y = &pos[i * 3 + 1];
        *x = *x - 0.5f < 320 ? -160.0f : 800.0f;
        *y = *y - 0.5f < 240 ? -32.0f : 512.0f;
    }
}

/* -- wide view: per-scene layout of fixed 640x480 elements ----------------------------------
   Elements are recognised by their texture size within a scene and moved/scaled in game
   coordinates (the screen shows -160..800 x -32..512 of them). p' = to + (p - from) * s. */
static void place(float *pos, uint32_t n, float s, float from_x, float from_y, float to_x, float to_y) {
    for (uint32_t i = 0; i < n; i++) {
        pos[i * 3] = to_x + (pos[i * 3] - 0.5f - from_x) * s + 0.5f;
        pos[i * 3 + 1] = to_y + (pos[i * 3 + 1] - 0.5f - from_y) * s + 0.5f;
    }
}
/* the same with separate x / y scales */
static void place_xy(float *pos, uint32_t n, float sx, float sy, float from_x, float from_y, float to_x, float to_y) {
    for (uint32_t i = 0; i < n; i++) {
        pos[i * 3] = to_x + (pos[i * 3] - 0.5f - from_x) * sx + 0.5f;
        pos[i * 3 + 1] = to_y + (pos[i * 3 + 1] - 0.5f - from_y) * sy + 0.5f;
    }
}
#define TEX_IS(t, w, h) ((t) && (t)->width == (w) && (t)->height == (h))
enum { SCENE_TITLE = 2 };
static const float TITLE_MENU_SCALE = 1.25f;
/* Title: the scrolling background layers cover the 960x544 screen (x1.5, centred); the menu
   column (gear, cursor, strip, items) keeps its left and top margins and grows; the copyright
   line sits at the bottom centre. The logo stays where it is. */
static void layout_title(const RenderTexture *t, float *pos, uint32_t n) {
    if (TEX_IS(t, 640, 480) || TEX_IS(t, 512, 128) || TEX_IS(t, 40, 128) || TEX_IS(t, 640, 128) || TEX_IS(t, 768, 256))
        place(pos, n, 1.5f, 320, 480, 320, 512);
    else if (TEX_IS(t, 96, 96) || TEX_IS(t, 86, 57) || TEX_IS(t, 160, 480) || TEX_IS(t, 108, 288))
        place(pos, n, TITLE_MENU_SCALE, 0, 0, -160, -32);
    else if (TEX_IS(t, 368, 14)) { /* copyright: centred at the bottom wherever the title animation puts it */
        float x0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < n; i++) {
            float x = pos[i * 3] - 0.5f, y = pos[i * 3 + 1] - 0.5f;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
        place(pos, n, 1, (x0 + x1) / 2, y1, 320, 504);
    }
}
enum { SCENE_SELECT_SCENARIO = 16 };
/* a layout rule may ask for the draw to be repeated, shifted by these x offsets (e.g. to tile a
   background wider than the game ever drew it) */
static float layout_copy_dx[2];
static int layout_copies;
static void quad_bounds(const float *pos, uint32_t n, float *x0, float *y0, float *x1, float *y1) {
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (uint32_t i = 0; i < n; i++) {
        float x = pos[i * 3] - 0.5f, y = pos[i * 3 + 1] - 0.5f;
        if (x < *x0) *x0 = x;
        if (x > *x1) *x1 = x;
        if (y < *y0) *y0 = y;
        if (y > *y1) *y1 = y;
    }
}
/* x' = 320 + (x - 320) * sx, y' = y + dy: a band stretched across the screen width */
static void stretch_x(float *pos, uint32_t n, float sx, float dy) {
    for (uint32_t i = 0; i < n; i++) {
        pos[i * 3] = 320 + (pos[i * 3] - 0.5f - 320) * sx + 0.5f;
        pos[i * 3 + 1] += dy;
    }
}
/* The gear panel shared by the story mode scenario select and the pause / title sub-menus, spread
   over the whole screen: the dark pattern fills it, the header and help strips grow x1.5 to the top
   and bottom edges, the top and bottom bands stretch to the edges and their gears (and the rod
   under the bottom band) tile along them. Returns 0 for anything else. */
static int layout_gears(const RenderTexture *t, float *pos, uint32_t n) {
    const float h = 544.0f / 480.0f;
    float x0, y0, x1, y1;
    quad_bounds(pos, n, &x0, &y0, &x1, &y1);
    int top = (y0 + y1) / 2 < 240;
    uint32_t w = t->width, ht = t->height;
    if (TEX_IS(t, 640, 16) && top)                                   /* header ("SCENARIO SELECT") */
        place(pos, n, 1.5f, 320, 0, 320, -32);
    else if (TEX_IS(t, 640, 16))                                     /* button help line: like the top */
        place(pos, n, 1.5f, 320, 480, 320, 512);                     /* strip, x1.5 at the bottom edge */
    else if (w == 640 && (ht == 96 || ht == 76 || ht == 78 || ht == 48 || ht == 64 || ht == 68))
        stretch_x(pos, n, 1.5f, top ? -32 : 32);                     /* top / bottom bands */
    else if (TEX_IS(t, 480, 480)) {                                  /* dark background pattern: */
        place(pos, n, h, x0 < 80 ? 0 : 640, 0, x0 < 80 ? -160 : 800, -32);  /* left and right tiles */
        if (x0 >= 80) { layout_copy_dx[0] = -260; layout_copies = 1; }    /* plus one in the middle: */
    }                                                                /* its ends fade out */
    else if (TEX_IS(t, 160, 160) || TEX_IS(t, 80, 80) || TEX_IS(t, 128, 128) || TEX_IS(t, 32, 32) ||
             TEX_IS(t, 64, 65) || TEX_IS(t, 400, 400) || TEX_IS(t, 312, 312) || TEX_IS(t, 198, 24) ||
             TEX_IS(t, 296, 34))
    {   /* gears keep their spacing (so they still mesh) at the screen edge, and the whole row is
           repeated one band width (640) to each side, like a tile */
        place(pos, n, 1, 0, 0, 0, top ? -32 : 32);
        layout_copy_dx[0] = -640; layout_copy_dx[1] = 1280; layout_copies = 2; /* offsets add up */
    }
    else return 0;
    return 1;
}
/* Story mode scenario select: the gear panel, the portrait fills the height on the left, the
   character list moves to the right edge, the profile line to the bottom left. */
static void layout_scenario(const RenderTexture *t, float *pos, uint32_t n) {
    if (layout_gears(t, pos, n)) return;
    float x0, y0, x1, y1;
    quad_bounds(pos, n, &x0, &y0, &x1, &y1);
    if ((TEX_IS(t, 198, 102) || TEX_IS(t, 512, 16) || TEX_IS(t, 20, 20) || TEX_IS(t, 96, 64) ||
         TEX_IS(t, 128, 24) || TEX_IS(t, 160, 32)) && y0 > 150 && y1 < 320)
        place(pos, n, 1, 0, 0, 160, 0);                              /* difficulty / deck popup: */
                                                                     /* stays next to the name plate */
    else if (TEX_IS(t, 640, 480))                                    /* character portrait */
        place(pos, n, 544.0f / 480.0f, 0, 0, -160, -32);
    else if (TEX_IS(t, 317, 58) || TEX_IS(t, 12, 118) || (TEX_IS(t, 256, 64) && x0 > 300))
        place(pos, n, 1, 0, 0, 160, 0);                              /* character list: right edge */
    else if (TEX_IS(t, 216, 32) || TEX_IS(t, 256, 64))
        place(pos, n, 1, 0, 0, -160, 32);                            /* profile line: bottom left */
}
/* VS character select (also the netplay ones): the portrait row in the middle grows x1.5 and fills
   the width (20 x 48) and the extra 64 rows; the 1P half moves up to the top edge, the 2P half down
   to the bottom; in each, the scrolling pattern tiles to both sides, the banner portrait goes left,
   the big 1P/2P letters, name plate and profile line go right. Header and help strips as in story mode. */
enum { SCENE_SELECT = 3, SCENE_SELECT_SERVER = 8, SCENE_SELECT_CLIENT = 9 };
/* The stage select is a panel drawn over the character select in the same scene; it starts with the
   480x480 dark pattern, which the character select never uses, so from there to the end of the frame
   the draws belong to the panel (reset at every present). */
static int stage_panel;
static void layout_stage(const RenderTexture *t, float *pos, uint32_t n) {
    const float s = 1.25f, h = 544.0f / 480.0f;
    float x0, y0, x1, y1;
    quad_bounds(pos, n, &x0, &y0, &x1, &y1);
    if (!t)                                                          /* highlight of the chosen row */
        place_xy(pos, n, 1.5f, s, 320, 240, 320, 240);
    else if (TEX_IS(t, 480, 480)) {                                  /* dark pattern covers the screen */
        place(pos, n, h, x0 < 80 ? 0 : 640, 0, x0 < 80 ? -160 : 800, -32);
        if (x0 >= 80) { layout_copy_dx[0] = -260; layout_copies = 1; }
    } else if (TEX_IS(t, 368, 80))                                   /* stage preview: centre */
        place(pos, n, s, 320, 240, 320, 240);
    else if (TEX_IS(t, 256, 64) || TEX_IS(t, 32, 32))                /* stage name, arrows: left edge */
        place(pos, n, s, 0, 240, -160, 240);
    else if (TEX_IS(t, 256, 16) || TEX_IS(t, 96, 12))                /* music title, selector: right edge */
        place(pos, n, s, 640, 240, 800, 240);
    else if (TEX_IS(t, 640, 17))                                     /* "STAGE SELECT" */
        place(pos, n, 1.5f, 320, 0, 320, -32);
    else if (TEX_IS(t, 640, 16))                                     /* help strip */
        place(pos, n, 1.5f, 320, 480, 320, 512);
}
static void layout_select(const RenderTexture *t, float *pos, uint32_t n) {
    if (TEX_IS(t, 480, 480)) stage_panel = 1;
    if (stage_panel) { layout_stage(t, pos, n); return; }
    if (!t) return;
    float x0, y0, x1, y1;
    quad_bounds(pos, n, &x0, &y0, &x1, &y1);
    int top = (y0 + y1) / 2 < 240;
    float dy = top ? -32 : 32;
    if (TEX_IS(t, 640, 16))                                          /* header / help strip */
        place(pos, n, 1.5f, 320, top ? 0 : 480, 320, top ? -32 : 512);
    else if (TEX_IS(t, 640, 128) || TEX_IS(t, 32, 128) || TEX_IS(t, 48, 48) || TEX_IS(t, 34, 48) ||
             TEX_IS(t, 33, 48) || TEX_IS(t, 12, 95))
        place(pos, n, 1.5f, 320, 176, 320, 144);                     /* portrait row and its cursors */
    else if (TEX_IS(t, 640, 176)) {                                  /* scrolling pattern, tiled */
        place(pos, n, 1, 0, 0, 0, dy);
        layout_copy_dx[0] = -640; layout_copy_dx[1] = 1280; layout_copies = 2;
    } else if (TEX_IS(t, 768, 192) || TEX_IS(t, 91, 12))             /* banner portrait: left (and a */
        place(pos, n, 1, 0, 0, -160, dy);                            /* label parked off its left edge) */
    else if (TEX_IS(t, 137, 142) || TEX_IS(t, 155, 141) || TEX_IS(t, 256, 64) || TEX_IS(t, 216, 32))
        place(pos, n, 1, 0, 0, 160, dy);                             /* 1P/2P, name plate, profile: right */
}
/* Fight HUD: the 1P half (left of the weather clock) moves to the left edge, the 2P half to the right
   edge, the clock stays; top elements go up to the top edge, bottom ones down to the bottom. Only the
   HUD's own textures in the top and bottom strips are touched, so characters and effects are not.
   The top frame is one 640-wide picture (the arcs framing both portraits at its ends, the weather
   clock in the middle): it is drawn in three cuts through the gaps between them, so the arcs follow
   the portraits and the clock stays centred. */
static int layout_split;
static const float HUD_CUT_L = 172, HUD_CUT_R = 468;   /* frame cuts (texture gaps) */
static const float HUD_SIDE_L = 280, HUD_SIDE_R = 360; /* element centres beyond these belong to 1P / 2P */
static void layout_hud(const RenderTexture *t, float *pos, uint32_t n) {
    float x0, y0, x1, y1;
    quad_bounds(pos, n, &x0, &y0, &x1, &y1);
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    int top = cy < 80, bottom = cy > 390;
    /* dialogue (story mode, win quotes, also the spell card cut-ins): the portraits (512x512 at full
       size) go to their side's edge like the bars (clamped, so a sliding portrait moves smoothly), the
       text box (untextured, 8..632 x 392..472) spans the screen with the same margins, its text
       (1024x128) keeps its inset; all of it sits 32 lower, on the bottom edge. */
    if (TEX_IS(t, 512, 512) && y1 - y0 > 300) {
        float dx = cx - 320;
        place(pos, n, 1, 0, 0, dx < -160 ? -160 : dx > 160 ? 160 : dx, 32);
        return;
    }
    if (!t && fabsf(x0 - 8) < 2 && fabsf(x1 - 632) < 2 && y0 > 380) { stretch_x(pos, n, 944.0f / 624, 32); return; }
    if (TEX_IS(t, 1024, 128) && cy > 380) { place(pos, n, 1, 0, 0, -160, 32); return; }
    if (TEX_IS(t, 256, 16) && cy > 360 && cy < 400) {               /* BGM title slides in at the 2P side */
        place(pos, n, 1, 0, 0, 160, 32);
        return;
    }
    /* combo counter (big digits, HIT, Damage/Rate/Limit) under the attacked side's health bar */
    if (cy > 80 && cy < 200 && (TEX_IS(t, 512, 35) || TEX_IS(t, 161, 13) || TEX_IS(t, 27, 15) || TEX_IS(t, 9, 13) ||
                                TEX_IS(t, 49, 14) || TEX_IS(t, 31, 12) || TEX_IS(t, 29, 12))) {
        place(pos, n, 1, 0, 0, cx < 320 ? -160 : 160, -32);
        return;
    }
    /* spell card name: the declaration (512x256 text, big and centred) shrinks onto its plate (192x32)
       at the caster's bottom corner, which then slides up under the caster's health bar. Moved by a
       clamped function of the centre, so the animation stays continuous: corners go 160 out and
       32 up/down like the bars, the middle of the field stays put. (512x256 is also used by
       full-screen effects; those are much taller than text.) */
    if (TEX_IS(t, 192, 32) || (TEX_IS(t, 512, 256) && y1 - y0 < 120)) {
        float dx = cx - 320, dy = (cy - 240) / 4;
        place(pos, n, 1, 0, 0, dx < -160 ? -160 : dx > 160 ? 160 : dx, dy < -32 ? -32 : dy > 32 ? 32 : dy);
        return;
    }
    if (!t || !(top || bottom)) return;
    int hud = top ? (TEX_IS(t, 160, 64) || TEX_IS(t, 640, 128) || TEX_IS(t, 288, 64) || TEX_IS(t, 252, 17) ||
                     TEX_IS(t, 28, 16) || TEX_IS(t, 256, 64) || TEX_IS(t, 64, 32) || TEX_IS(t, 144, 31) ||
                     TEX_IS(t, 144, 33) || TEX_IS(t, 128, 96) || TEX_IS(t, 13, 7) || TEX_IS(t, 4, 4) ||
                     TEX_IS(t, 128, 32))                                /* weather name over the clock */
                  : (TEX_IS(t, 160, 64) || TEX_IS(t, 45, 70) || TEX_IS(t, 41, 65) || TEX_IS(t, 21, 32) ||
                     TEX_IS(t, 30, 28) || TEX_IS(t, 24, 23) || TEX_IS(t, 22, 21) || TEX_IS(t, 108, 54) ||
                     TEX_IS(t, 109, 53) || TEX_IS(t, 79, 4) || TEX_IS(t, 3, 3) || TEX_IS(t, 17, 27) ||
                     TEX_IS(t, 64, 96) || TEX_IS(t, 32, 64) || TEX_IS(t, 30, 15));
    if (!hud) return;
    float dy = top ? -32 : 32;
    if (TEX_IS(t, 640, 128)) { place(pos, n, 1, 0, 0, 0, dy); layout_split = 1; return; }
    float dx = cx < HUD_SIDE_L ? -160 : cx > HUD_SIDE_R ? 160 : 0;
    place(pos, n, 1, 0, 0, dx, dy);
}
/* Gear panels drawn over another scene: the pause and post-match menus in a fight, the sub-menus of
   the title (network, music room, result, profile, deck construction, config). Each starts with the
   480x480 dark pattern (never used by the fight or the title themselves), so from there to the end of
   the frame the draws belong to it (reset at every present). The gear panel fills the screen; the
   title (PAUSE 171x93, RESULT 198x98, NETWORK 209x62, MUSIC ROOM 310x96, PROFILE 240x80, DECK
   CONSTRUCTION 336x80, CONFIG 243x80, the card editor's character name drawn at the origin) grows x1.25 in the top-left corner like the title menu; the
   profile names stay in the top band at the right edge; a character portrait (640x480) fills the
   height on the left. The rest is the content: the few options of the fight's menus grow x1.25 from
   the top-left corner too, the fuller title sub-menus grow to the screen height (x1.133), centred. */
static int pause_panel;
static void layout_panel(const RenderTexture *t, float *pos, uint32_t n, int fight) {
    const float h = 544.0f / 480.0f;
    if (!t || layout_gears(t, pos, n)) return;
    float x0, y0, x1, y1;
    quad_bounds(pos, n, &x0, &y0, &x1, &y1);
    if (TEX_IS(t, 171, 93) || TEX_IS(t, 198, 98) || TEX_IS(t, 209, 62) || TEX_IS(t, 310, 96) ||
        TEX_IS(t, 240, 80) || TEX_IS(t, 336, 80) || TEX_IS(t, 243, 80) ||
        (fabsf(x0) < 1 && fabsf(y0) < 1 && y1 <= 100))              /* card editor: character name */
        place(pos, n, 1.25f, 0, 0, -160, -32);
    else if ((TEX_IS(t, 216, 32) || TEX_IS(t, 256, 64)) && y0 < 80)
        place(pos, n, 1, 0, 0, 160, -32);
    else if (TEX_IS(t, 640, 480))
        place(pos, n, h, 0, 0, -160, -32);
    else if (fight)
        place(pos, n, 1.25f, 0, 0, -160, -32);
    else
        place(pos, n, h, 320, 0, 320, -32);
}
static int scene_is_fight(uint32_t scene) { return scene == 5 || (scene >= 13 && scene <= 15); }
/* scenes laid out for the whole screen; the others stay clipped to their 640x480 */
static int scene_is_wide(uint32_t scene) {
    return scene_is_fight(scene) || scene == SCENE_TITLE || scene == SCENE_SELECT_SCENARIO ||
           scene == SCENE_SELECT || scene == SCENE_SELECT_SERVER || scene == SCENE_SELECT_CLIENT;
}

int render_init(uint32_t width, uint32_t height) {
    if (ready)
        return 0;
    int wide = t123_wide = wide_view = wide_wanted();
    if (wide) { width = 960; height = 544; }
    trace("# display: %s", wide ? "wide 960x544 fight view" : "4:3");
    if (platform_init(width, height))
        return -1;
    screen_w = width;
    screen_h = height;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (wide) glOrtho(-160, 800, 512, -32, -1, 1);
    else glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    trace("# renderer: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
#ifndef __vita__
    SDL_GL_MakeCurrent(window, NULL);
#endif
    ready = 1;
    return 0;
}

static void color4(uint32_t argb, float out[4]) {
    out[0] = ((argb >> 16) & 255) / 255.0f;
    out[1] = ((argb >> 8) & 255) / 255.0f;
    out[2] = (argb & 255) / 255.0f;
    out[3] = (argb >> 24) / 255.0f;
}

/* Vita frame skipping: the game's logic advances one step per presented frame, so a
   lower frame rate would slow the game down. Instead, when a frame took longer than a
   refresh, the next one is not drawn (its clears and draws are dropped and it is not
   swapped), never twice in a row: the game keeps its speed and the screen shows at
   least 30 FPS. */
static int skip_drawing;
static uint64_t swap_us_sum, swap_count; /* time inside vglSwapBuffers, for the pacing report */

static void batch_flush(void); /* draw batching, below */
static int applied_valid; /* 0: the GL state differs from the last batch's (see apply_key) */
void render_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (skip_drawing) return;
    gl_enter();
    batch_flush();
    float c[4];
    color4(color, c);
    glClearColor(c[0], c[1], c[2], c[3]);
    glClearDepth(z);
    glClearStencil((GLint)stencil);
    GLbitfield mask = (flags & 1 ? GL_COLOR_BUFFER_BIT : 0) | (flags & 2 ? GL_DEPTH_BUFFER_BIT : 0) |
                      (flags & 4 ? GL_STENCIL_BUFFER_BIT : 0);
    glDepthMask(GL_TRUE);
    glClear(mask);
    applied_valid = 0;
    gl_leave();
}

static GLenum blend_factor(uint32_t b) {
    static const GLenum map[] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE};
    return b < sizeof map / sizeof *map ? map[b] : GL_ONE;
}

static GLenum compare(uint32_t f) { return f >= 1 && f <= 8 ? GL_NEVER + (f - 1) : GL_ALWAYS; }

/* -- draw batching -----------------------------------------------------------------------
 * The game draws every sprite with its own DrawPrimitiveUP (a 4-vertex strip); one
 * glDrawArrays each cost ~19 us of CPU on the Vita (vitaGL copies the vertices, picks a
 * shader, sets the state), ~3 ms of a fight frame. Consecutive triangle draws with the same
 * texture, filtering and blend/alpha/depth state are collected as one triangle list and drawn
 * together. A batch keeps the state it was started with (the next draw has already changed
 * the device state when it finds it different). Every other GL operation (clear, present,
 * freeing a texture) draws the batch first, and so does a texture that has to be uploaded
 * again, so what reaches the screen and its order do not change. */
typedef struct {
    const RenderTexture *tex;   /* NULL: untextured */
    GLuint gl;
    uint32_t mag, min;          /* linear filtering */
    uint32_t blend, src, dst, op, atest, afunc, aref, ztest, zfunc, zwrite;
} DrawKey;
enum { BATCH_MAX = 6 * 2048 };  /* vertices */
static DrawKey batch_key;
static uint32_t batch_n;
static float batch_pos[BATCH_MAX * 3], batch_uv[BATCH_MAX * 2];
static uint8_t batch_col[BATCH_MAX * 4];
static uint64_t batch_draws, batch_flushes; /* for the heartbeat */

static DrawKey draw_key(const RenderTexture *t) {
    static const GLenum ops[] = {0x8006, 0x8006, 0x800A, 0x800B, 0x8007, 0x8008}; /* ADD SUB REVSUB MIN MAX */
    DrawKey k;
    memset(&k, 0, sizeof k); /* compared with memcmp */
    if (t) {
        k.tex = t;
        k.gl = t->gl;
        k.mag = mag_linear || t->half; /* a half-resolution texture is scaled back up */
        k.min = min_linear || t->half;
    }
    k.blend = states[27] != 0;
    k.src = states[19] ? states[19] : 2;
    k.dst = states[20] ? states[20] : 1;
    k.op = states[171] < 6 ? ops[states[171]] : 0x8006;
    k.atest = states[15] != 0;
    k.afunc = states[25] ? states[25] : 8;
    k.aref = states[24] & 255;
    k.ztest = states[7] != 0;
    k.zfunc = states[23] ? states[23] : 4;
    k.zwrite = states[14] != 0;
    return k;
}

/* Only what differs from the last batch's state is sent (vitaGL marks its shader state
   dirty on every call). Clear, present, uploads and deletes touch the GL state directly and
   reset applied_valid, so the next batch sends everything. Filtering is per texture: it is
   set again whenever the texture changes. */
static DrawKey applied;
static void apply_key(const DrawKey *k) {
    const DrawKey *a = applied_valid ? &applied : NULL;
#define CHANGED(f) (!a || a->f != k->f)
    if (CHANGED(blend)) { if (k->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
    if (CHANGED(src) || CHANGED(dst)) glBlendFunc(blend_factor(k->src), blend_factor(k->dst));
    if (blend_equation && CHANGED(op)) blend_equation(k->op);
    if (CHANGED(atest)) { if (k->atest) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST); }
    if (CHANGED(afunc) || CHANGED(aref)) glAlphaFunc(compare(k->afunc), k->aref / 255.0f);
    if (CHANGED(ztest)) { if (k->ztest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
    if (CHANGED(zfunc)) glDepthFunc(compare(k->zfunc));
    if (CHANGED(zwrite)) glDepthMask(k->zwrite ? GL_TRUE : GL_FALSE);
    if (k->tex) {
        if (!a || !a->tex) glEnable(GL_TEXTURE_2D);
        int other = !a || !a->tex || a->gl != k->gl;
        if (other) glBindTexture(GL_TEXTURE_2D, k->gl);
        if (other || a->mag != k->mag) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, k->mag ? GL_LINEAR : GL_NEAREST);
        if (other || a->min != k->min) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, k->min ? GL_LINEAR : GL_NEAREST);
    } else if (!a || a->tex)
        glDisable(GL_TEXTURE_2D);
#undef CHANGED
    applied = *k;
    applied_valid = 1;
}

/* vertex arrays for a draw: under the GL lock */
static void set_arrays(const DrawKey *k, const float *pos, const float *uv, const uint8_t *col) {
    if (k->tex) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
    } else
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
}

static void batch_flush(void) { /* under the GL lock */
    if (!batch_n) return;
    apply_key(&batch_key);
    set_arrays(&batch_key, batch_pos, batch_uv, batch_col);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)batch_n);
    batch_n = 0;
    batch_flushes++;
}

/* appends vertices 0..n-1 of a triangle list (4), strip (5) or fan (6) as triangles */
static void batch_add(uint32_t type, uint32_t n, const float *pos, const float *uv, const uint8_t *col) {
    uint32_t tris = type == 4 ? n / 3 : n - 2;
    if (batch_n + tris * 3 > BATCH_MAX) batch_flush();
    for (uint32_t t = 0; t < tris; t++) {
        if (batch_n + 3 > BATCH_MAX) batch_flush(); /* a draw larger than a batch goes in pieces */
        uint32_t v[3];
        if (type == 4) { v[0] = 3 * t; v[1] = 3 * t + 1; v[2] = 3 * t + 2; }
        else if (type == 5) { v[0] = t + (t & 1); v[1] = t + 1 - (t & 1); v[2] = t + 2; } /* keep the winding */
        else { v[0] = 0; v[1] = t + 1; v[2] = t + 2; }
        for (int k = 0; k < 3; k++, batch_n++) {
            memcpy(&batch_pos[batch_n * 3], &pos[v[k] * 3], 12);
            memcpy(&batch_uv[batch_n * 2], &uv[v[k] * 2], 8);
            memcpy(&batch_col[batch_n * 4], &col[v[k] * 4], 4);
        }
    }
    batch_draws++;
}

void render_state(uint32_t state, uint32_t value) {
    if (state < 256)
        states[state] = value;
}
uint32_t render_get_state(uint32_t state) { return state < 256 ? states[state] : 0; }

void render_stage_state(uint32_t stage, uint32_t type, uint32_t value) { (void)stage; (void)type; (void)value; }

void render_sampler_state(uint32_t sampler, uint32_t type, uint32_t value) {
    if (sampler) return;
    if (type == 5) mag_linear = value == 2;
    if (type == 6) min_linear = value == 2;
}

void render_fvf(uint32_t value) { fvf = value; }

/* Uploaded textures stay on the GPU until the game releases them. In a fight on the
   Vita that filled vitaGL's memory (sprites vanished, then ~10 FPS), so the Vita keeps
   a budget and drops the textures drawn longest ago; a dropped texture is uploaded
   again from its stored copy the next time it is drawn. */
extern RenderTexture textures[RENDER_MAX_TEXTURES];
#ifdef __vita__
static uint64_t gpu_budget = 60u << 20; /* of vitaGL's 80 MiB of CDRAM + 22 MiB of phycont */
#else
static uint64_t gpu_budget = UINT64_MAX; /* T123_GPU_BUDGET_MB to test the Vita's eviction */
#endif
static uint64_t gpu_bytes, gpu_evictions;
static uint64_t gpu_t_query, gpu_t_scan, gpu_t_delete, gpu_short_stops; /* microseconds, see up_report */

static void gpu_forget(RenderTexture *t) { /* under the GL lock */
    if (!t->gl) return;
    glDeleteTextures(1, &t->gl);
    applied_valid = 0; /* it may have been the bound texture */
    t->gl = 0;
    gpu_bytes -= t->gpu_bytes;
    t->gpu_bytes = 0;
}

/* vitaGL frees a deleted texture only once the GPU is done with it, so its real free
   memory can be far below what the budget suggests: 5300 evictions in a long fight
   ended in a GPU crash on the Vita (52 MiB budget). Room is also made while vitaGL's
   own free memory is short. */
/* vglMemFree walks vitaGL's allocator and cost ~4.5 ms per upload (24.9 s of 26.6 s of
   "making room" in a 14 minute fight), so it is asked at most once every 6 frames; in
   between the free memory is the last answer minus what was uploaded since (deferred
   frees only make that estimate pessimistic). */
static uint64_t gpu_free_cache, gpu_added_since, gpu_free_frame = ~0ull;
static int gpu_memory_short(uint64_t need) {
#ifdef __vita__
    if (gpu_free_frame == ~0ull || d3d_frames - gpu_free_frame >= 6) {
        gpu_free_cache = (uint64_t)vglMemFree(VGL_MEM_VRAM) + vglMemFree(VGL_MEM_PHYCONT);
        gpu_free_frame = d3d_frames;
        gpu_added_since = 0;
    }
    uint64_t free_now = gpu_free_cache > gpu_added_since ? gpu_free_cache - gpu_added_since : 0;
    return free_now < need + (8u << 20);
#else
    (void)need;
    return 0;
#endif
}

static void gpu_make_room(uint64_t need) {
#ifndef __vita__
    static int read_env;
    if (!read_env++ && getenv("T123_GPU_BUDGET_MB"))
        gpu_budget = strtoull(getenv("T123_GPU_BUDGET_MB"), NULL, 10) << 20;
#endif
    /* vitaGL frees a deleted texture late, so its free memory does not rise while this
       loop evicts: with the short-memory test alone it kept dropping every older
       texture (3500 evictions with ~11-30 MiB resident in a 10 minute fight, "making
       room" 56 % of the upload time). When only that test asks for room, evict about
       the size of the new texture and stop; glTexImage2D's retry in upload() still
       evicts more if the allocation really fails. */
    uint64_t freed = 0;
    for (;;) {
        uint64_t q0 = platform_ticks_us();
        int over_budget = gpu_bytes + need > gpu_budget;
        int short_mem = !over_budget && gpu_bytes && gpu_memory_short(need);
        gpu_t_query += platform_ticks_us() - q0;
        if (!over_budget && !short_mem) break;
        if (!over_budget && freed >= need) { gpu_short_stops++; break; }
        uint64_t s0 = platform_ticks_us();
        RenderTexture *oldest = NULL;
        for (unsigned i = 1; i < RENDER_MAX_TEXTURES; i++) {
            RenderTexture *t = &textures[i];
            if (t->gl && t->used_frame < d3d_frames && (!oldest || t->used_frame < oldest->used_frame))
                oldest = t;
        }
        gpu_t_scan += platform_ticks_us() - s0;
        if (!oldest) return; /* everything was drawn this frame: go over budget */
        freed += oldest->gpu_bytes;
        uint64_t d0 = platform_ticks_us();
        gpu_forget(oldest);
        gpu_t_delete += platform_ticks_us() - d0;
        if (++gpu_evictions % 100 == 1)
#ifdef __vita__
            trace("# GPU texture budget: %llu evictions so far, %llu KiB uploaded, vitaGL free %u KiB VRAM %u KiB phycont",
                  (unsigned long long)gpu_evictions, (unsigned long long)(gpu_bytes >> 10),
                  (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_PHYCONT) >> 10));
#else
            trace("# GPU texture budget: %llu evictions so far, %llu KiB uploaded",
                  (unsigned long long)gpu_evictions, (unsigned long long)(gpu_bytes >> 10));
#endif
    }
}

/* D3D formats -> RGBA8, n rows. */
/* The format switch is outside the pixel loops (it ran once per pixel before), and the
   5-bit channel expansion uses a table instead of a multiply and divide per channel. */
static uint8_t expand5[32], expand6[64];
static void init_expand_tables(void) {
    for (unsigned i = 0; i < 32; i++) expand5[i] = (uint8_t)(i * 255 / 31);
    for (unsigned i = 0; i < 64; i++) expand6[i] = (uint8_t)(i * 255 / 63);
}

static void convert_rows(const RenderTexture *t, const uint8_t *src, uint8_t *rgba, uint32_t n) {
    if (!expand5[31]) init_expand_tables();
    const uint32_t w = t->width, fmt = t->format;
    for (uint32_t y = 0; y < n; y++) {
        const uint8_t *p = src + (size_t)y * t->pitch;
        uint8_t *o = rgba + (size_t)y * w * 4;
        switch (fmt) {
        case 21: case 22:
            for (uint32_t x = 0; x < w; x++, p += 4, o += 4) {
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = fmt == 22 ? 255 : p[3];
            }
            break;
        case 26: /* A4R4G4B4 */
            for (uint32_t x = 0; x < w; x++, p += 2, o += 4) {
                uint32_t v = p[0] | p[1] << 8;
                o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; o[3] = (v >> 12) * 17;
            }
            break;
        case 25: case 24: /* A1R5G5B5, X1R5G5B5 */
            for (uint32_t x = 0; x < w; x++, p += 2, o += 4) {
                uint32_t v = p[0] | p[1] << 8;
                o[0] = expand5[(v >> 10) & 31]; o[1] = expand5[(v >> 5) & 31]; o[2] = expand5[v & 31];
                o[3] = fmt == 24 || (v >> 15) ? 255 : 0;
            }
            break;
        case 23: /* R5G6B5 */
            for (uint32_t x = 0; x < w; x++, p += 2, o += 4) {
                uint32_t v = p[0] | p[1] << 8;
                o[0] = expand5[(v >> 11) & 31]; o[1] = expand6[(v >> 5) & 63]; o[2] = expand5[v & 31]; o[3] = 255;
            }
            break;
        case 28: /* A8 */
            for (uint32_t x = 0; x < w; x++, o += 4) { o[0] = o[1] = o[2] = 0; o[3] = p[x]; }
            break;
        case 50: /* L8 */
            for (uint32_t x = 0; x < w; x++, o += 4) { o[0] = o[1] = o[2] = p[x]; o[3] = 255; }
            break;
        default:
            for (uint32_t x = 0; x < w; x++, o += 4) { o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; } /* unknown: magenta */
            break;
        }
    }
}

/* Where upload time goes (microseconds), reported every 10 s while uploads happen. */
/* conv/gl: time inside the band sinks converting pixels and in glTexSubImage2D; the rest of
   fill is reading the stored pixels (inflate on the Vita). big: textures of >= 256x256. */
static struct { uint64_t n, total, room, alloc, fill, conv, gl, n_big, fill_big, last_report; } up_stats;
static void up_report(uint64_t now) {
    if (!up_stats.n || now - up_stats.last_report < 10000000) return;
    trace("# texture uploads: %llu in %llu ms (making room %llu ms [free query %llu, scan %llu, delete %llu], GL alloc %llu ms, fill = inflate+convert+GL %llu ms), %llu evictions so far, %llu short-memory stops",
          (unsigned long long)up_stats.n, (unsigned long long)(up_stats.total / 1000), (unsigned long long)(up_stats.room / 1000),
          (unsigned long long)(gpu_t_query / 1000), (unsigned long long)(gpu_t_scan / 1000), (unsigned long long)(gpu_t_delete / 1000),
          (unsigned long long)(up_stats.alloc / 1000), (unsigned long long)(up_stats.fill / 1000), (unsigned long long)gpu_evictions,
          (unsigned long long)gpu_short_stops);
    static unsigned long long parallel_seen;
    trace("# texture fill split: read/inflate %llu ms, convert %llu ms, glTexSubImage2D %llu ms; %llu of the uploads >= 256x256 took %llu ms of the fill; %llu inflated in parallel",
          (unsigned long long)((up_stats.fill - up_stats.conv - up_stats.gl) / 1000), (unsigned long long)(up_stats.conv / 1000),
          (unsigned long long)(up_stats.gl / 1000), (unsigned long long)up_stats.n_big, (unsigned long long)(up_stats.fill_big / 1000),
          texture_parallel_uploads - parallel_seen);
    parallel_seen = texture_parallel_uploads;
    gpu_t_query = gpu_t_scan = gpu_t_delete = 0;
    uint64_t last = now;
    memset(&up_stats, 0, sizeof up_stats);
    up_stats.last_report = last;
}

/* Upload in bands: the texture is created empty and filled row band by row band from
   the stored pixels (inflated a band at a time on the Vita), so no buffer the size of
   the texture is needed. A whole-texture scratch buffer (4 MiB for 1024x1024) could
   not be found once memory was fragmented on the Vita, and the game then waited
   forever for that texture on its loading screen. */
enum { BAND_BYTES = 256 * 1024 };
static uint8_t band[BAND_BYTES], band_rgba[BAND_BYTES * 2];

/* Large textures (both sides >= 640: backgrounds and scenery, not the 512x512 and
   smaller sprite sheets) go to the GPU at half resolution on the Vita, a quarter of
   the memory; the texture coordinates are normalised, so the game does not notice.
   Each 2x2 block of BGRA pixels becomes its average. */
#ifdef __vita__
static int half_res_enabled = 1;
#else
static int half_res_enabled = -1; /* T123_HALF_RES=1 to see it on PC */
#endif
/* Only the 640+ backgrounds are halved by default. The resolution test that halved every
   texture of 512x512 and up also halved the game's text (the result screen draws its
   spell-card names into one of those big textures, and came out as 5-pixel smears) and
   it gained no frame rate, so it is off now. half-sprites.txt in the data directory turns
   it back on, for measuring. */
static int half_min_side(void) {
    static int min_side;
    if (!min_side) {
#ifdef __vita__
        FILE *f = fopen("ux0:data/touhou123vita/half-sprites.txt", "rb");
        min_side = f ? 512 : 640;
        if (f) fclose(f);
        trace("# textures: halving textures with both sides >= %d (half-sprites.txt present: %d)", min_side, f != NULL);
#else
        min_side = getenv("T123_HALF_SPRITES") ? 512 : 640;
#endif
    }
    return min_side;
}
static int wants_half(const RenderTexture *t) {
    if (half_res_enabled < 0) half_res_enabled = getenv("T123_HALF_RES") != NULL;
    int m = half_min_side();
    return half_res_enabled && t->format == 21 && t->pitch == t->width * 4 && !(t->width & 1) &&
           t->width >= (uint32_t)m && t->height >= (uint32_t)m;
}

static void upload_band_half(void *ctx, const uint8_t *rows, uint32_t y, uint32_t n) {
    RenderTexture *t = ctx;
    uint64_t c0 = platform_ticks_us();
    uint32_t w = t->width / 2, out_rows = (n + 1) / 2;
    for (uint32_t r = 0; r < out_rows; r++) {
        const uint8_t *a = rows + (size_t)(2 * r) * t->pitch;
        const uint8_t *b = 2 * r + 1 < n ? a + t->pitch : a; /* odd last row: reuse it */
        uint8_t *o = band_rgba + (size_t)r * w * 4;
        for (uint32_t x = 0; x < w; x++, a += 8, b += 8, o += 4)
            for (unsigned k = 0; k < 4; k++)
                o[k] = (uint8_t)((a[k] + a[k + 4] + b[k] + b[k + 4] + 2) >> 2);
    }
    uint64_t c1 = platform_ticks_us();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)(y / 2), (GLsizei)w, (GLsizei)out_rows, GL_BGRA, GL_UNSIGNED_BYTE, band_rgba);
    up_stats.conv += c1 - c0; up_stats.gl += platform_ticks_us() - c1;
}

/* 16-bit upload (RGBA4444): half the GPU memory and half the upload traffic for the
   large 32-bit sprite sheets. vitaGL copies 16-bit data raw into a U4U4U4U4_RGBA
   texture, so the pixels are packed here as the GL type says (R in the top nibble).
   On the Vita it is on unless tex32.txt exists in the data directory; on PC
   T123_TEX16=1 turns it on to look at it. Only A8R8G8B8/X8R8G8B8 textures of at least
   256x256 are packed: small UI textures keep their 32 bits. */
static int tex16_enabled(void) {
    static int state = -1;
    if (state < 0) {
#ifdef __vita__
        FILE *f = fopen("ux0:data/touhou123vita/tex32.txt", "rb");
        state = f == NULL;
        if (f) fclose(f);
        trace("# textures: 16-bit upload %s", state ? "on (tex32.txt turns it off)" : "off (tex32.txt present)");
#else
        state = getenv("T123_TEX16") != NULL;
#endif
    }
    return state;
}

static uint8_t q4[256];
static uint16_t band16[BAND_BYTES / 4]; /* a band of 32-bit rows holds at most BAND_BYTES/4 pixels */
static void upload_band_16(void *ctx, const uint8_t *rows, uint32_t y, uint32_t n) {
    RenderTexture *t = ctx;
    uint64_t c0 = platform_ticks_us();
    if (!q4[255]) for (unsigned i = 0; i < 256; i++) q4[i] = (uint8_t)((i * 15 + 127) / 255);
    const uint8_t *p = rows;
    uint16_t *o = band16;
    const int opaque = t->format == 22;
    for (size_t i = 0, count = (size_t)n * t->width; i < count; i++, p += 4)
        *o++ = (uint16_t)(q4[p[2]] << 12 | q4[p[1]] << 8 | q4[p[0]] << 4 | (opaque ? 15 : q4[p[3]]));
    uint64_t c1 = platform_ticks_us();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)y, (GLsizei)t->width, (GLsizei)n, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, band16);
    up_stats.conv += c1 - c0; up_stats.gl += platform_ticks_us() - c1;
}

/* what the game really creates, to decide what else is worth compressing */
static struct { unsigned n, n16; uint64_t px; } fmt_census[64];
static void census_report(uint64_t now) { /* once a minute: uploads per D3D format, and how many went out as 16 bits */
    static uint64_t last;
    if (now - last < 60000000) return;
    last = now;
    char line[300];
    int len = snprintf(line, sizeof line, "# texture formats (uploads, Mpixels, as 16-bit):");
    for (unsigned f = 0; f < 64 && len < (int)sizeof line - 40; f++)
        if (fmt_census[f].n)
            len += snprintf(line + len, sizeof line - len, " fmt%u %u/%u.%02u/%u;", f, fmt_census[f].n,
                            (unsigned)(fmt_census[f].px / 1000000), (unsigned)(fmt_census[f].px / 10000 % 100), fmt_census[f].n16);
    trace("%s", line);
}

static void upload_band(void *ctx, const uint8_t *rows, uint32_t y, uint32_t n) {
    RenderTexture *t = ctx;
    uint64_t c0 = platform_ticks_us(), c1 = c0;
    if (t->format == 21 && t->pitch == t->width * 4) { /* A8R8G8B8 is BGRA in memory: as is */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)y, (GLsizei)t->width, (GLsizei)n, GL_BGRA, GL_UNSIGNED_BYTE, rows);
    } else {
        convert_rows(t, rows, band_rgba, n);
        c1 = platform_ticks_us();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)y, (GLsizei)t->width, (GLsizei)n, GL_RGBA, GL_UNSIGNED_BYTE, band_rgba);
    }
    up_stats.conv += c1 - c0; up_stats.gl += platform_ticks_us() - c1;
}

static void upload(RenderTexture *t) {
    applied_valid = 0; /* binds the texture it fills */
    int half = wants_half(t);
    uint32_t gw = half ? t->width / 2 : t->width, gh = half ? (t->height + 1) / 2 : t->height;
    int use16 = !half && (t->format == 21 || t->format == 22) && t->pitch == t->width * 4 &&
                (uint64_t)t->width * t->height >= 256 * 256 && tex16_enabled();
    GLenum gtype = use16 ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_BYTE;
    size_t out_bytes = (size_t)gw * gh * (use16 ? 2 : 4);
    if (t->format < 64) {
        fmt_census[t->format].n++;
        fmt_census[t->format].n16 += use16;
        fmt_census[t->format].px += (uint64_t)t->width * t->height;
    }
    if (t->pitch > BAND_BYTES || (size_t)t->width * 4 > sizeof band_rgba) {
        trace("# texture upload %ux%u: rows wider than the upload band", t->width, t->height);
        return;
    }
    uint64_t t0 = platform_ticks_us();
    gpu_forget(t);
    gpu_make_room(out_bytes);
    uint64_t t1 = platform_ticks_us();
    glGenTextures(1, &t->gl);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* When vitaGL cannot allocate the storage, glTexSubImage2D writes through a NULL
       pointer (a crash in write_rgba8888 in Story mode): evict and retry, and if there
       is still no room leave the texture empty for this frame. */
    while (glGetError()) {}
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)gw, (GLsizei)gh, 0, GL_RGBA, gtype, NULL);
    for (unsigned attempt = 0; glGetError() != GL_NO_ERROR; attempt++) {
        RenderTexture *oldest = NULL;
        for (unsigned i = 1; i < RENDER_MAX_TEXTURES; i++) {
            RenderTexture *o = &textures[i];
            if (o != t && o->gl && (!oldest || o->used_frame < oldest->used_frame))
                oldest = o;
        }
        if (!oldest || attempt == 64) {
            trace("# texture upload %ux%u: no GPU memory, skipped", t->width, t->height);
            glDeleteTextures(1, &t->gl);
            t->gl = 0;
            return;
        }
        gpu_forget(oldest);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)gw, (GLsizei)gh, 0, GL_RGBA, gtype, NULL);
    }
    /* band rows: bounded by both buffers (16-bit formats widen to 4 bytes per pixel);
       even, so a half-resolution band starts on a whole output row */
    uint64_t t2 = platform_ticks_us();
    size_t rows = BAND_BYTES / t->pitch, rgba_rows = sizeof band_rgba / ((size_t)t->width * 4);
    if (rgba_rows < rows) rows = rgba_rows;
    if (rows > 1) rows &= ~(size_t)1;
    t->gpu16 = (uint8_t)use16;
    render_texture_rows(t, band, rows * t->pitch, half ? upload_band_half : use16 ? upload_band_16 : upload_band, t);
    uint64_t t3 = platform_ticks_us();
    up_stats.n++; up_stats.total += t3 - t0; up_stats.room += t1 - t0; up_stats.alloc += t2 - t1; up_stats.fill += t3 - t2;
    if ((uint64_t)t->width * t->height >= 256 * 256) { up_stats.n_big++; up_stats.fill_big += t3 - t2; }
    up_report(t3);
    census_report(t3);
    t->half = (uint8_t)half;
    GLenum error = glGetError();
    if (error) {
        static unsigned reported;
        if (reported++ < 20)
            trace("# texture upload %ux%u failed: GL error %04x (%llu KiB uploaded)", t->width, t->height,
                  (unsigned)error, (unsigned long long)(gpu_bytes >> 10));
    }
    t->gpu_bytes = (uint32_t)out_bytes;
    gpu_bytes += out_bytes;
    gpu_added_since += out_bytes;
    t->dirty = 0;
}
void render_texture_free(RenderTexture *t) {
    if (!t->gl) return;
    gl_enter();
    batch_flush();
    gpu_forget(t);
    gl_leave();
}

void render_texture(uint32_t stage, RenderTexture *t) {
    if (!stage)
        bound = t;
}

#ifndef __vita__
/* T123_DUMPTEX="640x128,160x64": each distinct texture of those sizes is written once to the working
   directory as tex-WxH-N.bgra (raw rows, for working out what a layout rule is moving) */
static void dump_texture(const RenderTexture *t) {
    static const char *want = (const char *)-1;
    if (want == (const char *)-1) want = getenv("T123_DUMPTEX");
    if (!want || !t || !t->data || t->format != 21) return;
    char key[32];
    snprintf(key, sizeof key, "%ux%u", t->width, t->height);
    if (!strstr(want, key)) return;
    static const RenderTexture *seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == t) return;
    if (nseen == 64) return;
    seen[nseen++] = t;
    char name[64];
    snprintf(name, sizeof name, "tex-%s-%d.bgra", key, nseen);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    for (uint32_t y = 0; y < t->height; y++) fwrite(t->data + (size_t)y * t->pitch, 4, t->width, f);
    fclose(f);
}
#endif

void render_draw_up(uint32_t type, uint32_t count, uint32_t data, uint32_t stride) {
    if (skip_drawing) return;
    static const GLenum modes[] = {0, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    uint32_t n = type == 1 ? count : type == 2 ? count * 2 : type == 3 ? count + 1
               : type == 4 ? count * 3 : count + 2;
    if (type < 1 || type > 6 || !n || n > 65536)
        return;
    /* XYZW (0x4002) comes only from the effect passes (Utsuho's pshaderEx0), whose
       vertex shader takes screen pixels, so it is drawn like XYZRHW */
    int rhw = (fvf & 0x400E) == 0x004 || (fvf & 0x400E) == 0x4002;
    uint32_t off = rhw ? 16 : 12;
    if (fvf & 0x010) off += 12;
    uint32_t diffuse = fvf & 0x040 ? off : 0xFFFFFFFFu;
    if (fvf & 0x040) off += 4;
    if (fvf & 0x080) off += 4;
    uint32_t tex = ((fvf >> 8) & 15) ? off : 0xFFFFFFFFu;
    gl_enter(); /* the vertex arrays below are shared by the drawing threads */
    static float pos[65536 * 3], uv[65536 * 2];
    static uint8_t col[65536 * 4];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = data + i * stride;
        float xyz[3];
        memcpy(xyz, G(v), 12);
        pos[i * 3] = xyz[0] + (rhw ? 0.5f : 0);   /* D3D9 pixel centres */
        pos[i * 3 + 1] = xyz[1] + (rhw ? 0.5f : 0);
        pos[i * 3 + 2] = 0;
        uint32_t argb = diffuse != 0xFFFFFFFFu ? rd32(v + diffuse) : 0xFFFFFFFFu;
        col[i * 4] = (uint8_t)(argb >> 16); col[i * 4 + 1] = (uint8_t)(argb >> 8);
        col[i * 4 + 2] = (uint8_t)argb; col[i * 4 + 3] = (uint8_t)(argb >> 24);
        if (tex != 0xFFFFFFFFu)
            memcpy(&uv[i * 2], G(v + tex), 8);
    }
    layout_copies = 0;
    layout_split = 0;
    if (wide_view && rhw && n >= 4 && n <= 6) {
        uint32_t scene = rd32(0x8A0044); /* SokuLib ADDR_SCENE_ID */
        if (scene_is_fight(scene)) {
            const RenderTexture *b = tex != 0xFFFFFFFFu ? bound : NULL;
            if (TEX_IS(b, 480, 480)) pause_panel = 1;
            if (pause_panel) layout_panel(b, pos, n, 1);
            else { widen_fullscreen_quad(pos, n); layout_hud(b, pos, n); }
        }
        else if (scene == SCENE_TITLE) {
            const RenderTexture *b = tex != 0xFFFFFFFFu ? bound : NULL;
            if (TEX_IS(b, 480, 480)) pause_panel = 1;
            if (pause_panel) layout_panel(b, pos, n, 0);
            else if (b) layout_title(b, pos, n);
        }
        else if (scene == SCENE_SELECT_SCENARIO && bound) layout_scenario(bound, pos, n);
        else if (scene == SCENE_SELECT || scene == SCENE_SELECT_SERVER || scene == SCENE_SELECT_CLIENT)
            layout_select(tex != 0xFFFFFFFFu ? bound : NULL, pos, n);
    }
    RenderTexture *t = bound && tex != 0xFFFFFFFFu ? bound : NULL;
    if (t) {
#ifndef __vita__
        dump_texture(t);
#endif
        if (t->dirty || !t->gl) { /* the batch was drawn with the old contents */
            batch_flush();
            upload(t);
        }
        t->drawn++;
        t->used_frame = d3d_frames;
    }
    DrawKey key = draw_key(t);
    if (batch_n && memcmp(&key, &batch_key, sizeof key)) batch_flush();
    batch_key = key;
    if (type < 4) { /* points and lines: drawn on their own */
        batch_flush();
        apply_key(&key);
        set_arrays(&key, pos, uv, col);
        glDrawArrays(modes[type], 0, (GLsizei)n);
        gl_leave();
        return;
    }
    if (layout_split && type == 5 && n == 4) {
        /* a strip quad (0,1 top, 2,3 bottom) drawn in three x cuts: left cut moved left, middle kept,
           right cut moved right; u follows the cut */
        float p0[12], u0[8];
        memcpy(p0, pos, sizeof p0);
        memcpy(u0, uv, sizeof u0);
        float xa = p0[0], xb = p0[3], ua = u0[0], ub = u0[2];
        const float lo[3] = {-1e9f, HUD_CUT_L + 0.5f, HUD_CUT_R + 0.5f}, hi[3] = {HUD_CUT_L + 0.5f, HUD_CUT_R + 0.5f, 1e9f};
        const float move[3] = {-160, 0, 160};
        for (int s = 0; s < 3 && xb != xa; s++) {
            float l = xa < lo[s] ? lo[s] : xa, r = xb > hi[s] ? hi[s] : xb;
            if (r <= l) continue;
            float ul = ua + (l - xa) / (xb - xa) * (ub - ua), ur = ua + (r - xa) / (xb - xa) * (ub - ua);
            pos[0] = pos[6] = l + move[s]; pos[3] = pos[9] = r + move[s];
            uv[0] = uv[4] = ul; uv[2] = uv[6] = ur;
            batch_add(type, 4, pos, uv, col);
        }
        layout_copies = 0;
    } else
        batch_add(type, n, pos, uv, col);
    for (int k = 0; k < layout_copies; k++) {
        for (uint32_t i = 0; i < n; i++) pos[i * 3] += layout_copy_dx[k];
        batch_add(type, n, pos, uv, col);
    }
    gl_leave();
}

/* The last frame is kept for the capture written at exit. Reading it back stalls the
   GPU, so the Vita only does it once a second. */
static uint8_t *last_frame;
static uint32_t capture_w, capture_h;

/* Every 10 s: game steps, frames drawn and music decode cost, so a run that freezes
   (and never writes its exit report) still shows where it was going. */
static void heartbeat(int drawn) {
    static uint64_t next, steps, draws;
    uint64_t now = platform_ticks_us();
    steps++;
    draws += drawn != 0;
    /* Where a logic step goes: the interval between presents of this thread, split into
       time blocked in WaitForSingleObject, Sleep and the swap; the rest is the guest's own
       code plus the host calls it makes. The histogram shows whether the intervals sit at
       the game's own limiter (~17 ms) or are spikes of work. */
    static uint64_t last_present, w_wait, w_sleep, w_calls, sum_interval, intervals, hist[7];
    static uint64_t prev_wait, prev_sleep, prev_calls, thread_switches;
    static uint64_t *thread_tag; /* the wait counters are per thread: the address of one identifies the caller */
    if (thread_tag != &host_wait_us) {
        if (thread_tag) thread_switches++;
        thread_tag = &host_wait_us;
        prev_wait = host_wait_us; prev_sleep = host_sleep_us; prev_calls = host_wait_calls;
        last_present = 0; /* a step measured across two threads is meaningless */
    }
    if (last_present) {
        uint64_t iv = now - last_present;
        sum_interval += iv;
        intervals++;
        hist[iv < 15000 ? 0 : iv < 16500 ? 1 : iv < 17500 ? 2 : iv < 20000 ? 3 : iv < 25000 ? 4 : iv < 34000 ? 5 : 6]++;
        w_wait += host_wait_us - prev_wait;
        w_sleep += host_sleep_us - prev_sleep;
        w_calls += host_wait_calls - prev_calls;
    }
    prev_wait = host_wait_us; prev_sleep = host_sleep_us; prev_calls = host_wait_calls;
    last_present = now;
    if (!next) next = now + 10000000;
    if (now < next) return;
    if (intervals) {
        uint64_t n = intervals;
        uint64_t swap_avg = swap_count ? swap_us_sum / swap_count : 0;
        long long other = (long long)(sum_interval / n) - (long long)(w_wait / n) - (long long)(w_sleep / n) - (long long)swap_avg;
        trace("# pacing: %llu steps, interval %llu us avg = wait %llu us (%llu calls) + sleep %llu us + swap %llu us + other %lld us; "
              "present thread changed %llu times; "
              "intervals <15ms %llu, 15-16.5 %llu, 16.5-17.5 %llu, 17.5-20 %llu, 20-25 %llu, 25-34 %llu, >=34 %llu",
              (unsigned long long)n, (unsigned long long)(sum_interval / n), (unsigned long long)(w_wait / n),
              (unsigned long long)(w_calls / n), (unsigned long long)(w_sleep / n), (unsigned long long)swap_avg,
              other, (unsigned long long)thread_switches,
              (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3],
              (unsigned long long)hist[4], (unsigned long long)hist[5], (unsigned long long)hist[6]);
        swap_us_sum = swap_count = 0;
    }
    w_wait = w_sleep = w_calls = sum_interval = intervals = 0;
    memset(hist, 0, sizeof hist);
    uint64_t us, bytes;
    vorbis_stats(&us, &bytes);
    char memory[128];
    platform_memory_report(memory, sizeof memory);
    trace("# heartbeat: frame %llu, %llu steps and %llu frames drawn in the last 10 s, "
          "music decode %llu ms for %llu KiB; guest heap %u KiB live of %u KiB, "
          "textures %llu KiB on the GPU, %llu KiB stored, %s", (unsigned long long)d3d_frames,
          (unsigned long long)steps, (unsigned long long)draws, (unsigned long long)(us / 1000),
          (unsigned long long)(bytes >> 10), heap_live_bytes() >> 10, heap_used() >> 10,
          (unsigned long long)(gpu_bytes >> 10), (unsigned long long)(texture_live >> 10), memory);
    trace("# batching: %llu draws went to the GPU in %llu calls in the last 10 s",
          (unsigned long long)batch_draws, (unsigned long long)batch_flushes);
    batch_draws = batch_flushes = 0;
    steps = draws = 0;
    next = now + 10000000;
}

void render_present(void) {
    {   /* scene changes in the trace (SokuLib scene ids: 2 title, 3 select, 5 fight, ...) */
        static uint32_t last_scene = 0xFFFFFFFFu;
        uint32_t scene = rd32(0x8A0044);
        if (scene != last_scene) {
            unsigned alive = 0, waiting = 0;
            uint64_t waiting_px = 0;
            for (unsigned i = 1; i < RENDER_MAX_TEXTURES; i++)
                if (textures[i].width) {
                    alive++;
                    if (!textures[i].gl) { waiting++; waiting_px += (uint64_t)textures[i].width * textures[i].height; }
                }
            trace("# scene %u (frame %llu, %llu KiB of textures on the GPU; %u textures alive, %u not uploaded, %llu Kpixels)",
                  scene, d3d_frames, (unsigned long long)(gpu_bytes >> 10), alive, waiting, (unsigned long long)(waiting_px >> 10));
            last_scene = scene;
        }
    }
    ime_poll();                                  /* the keyboard for text fields (Vita) */
    net_poll();                                  /* SceNet + ad-hoc search, from the first frame (Vita) */
    if (ime_dialog_active()) skip_drawing = 0;   /* the dialog needs a swap every frame */
    heartbeat(!skip_drawing);
#ifdef __vita__
    static uint64_t last_swap, skipped;
    if (skip_drawing) { /* nothing was drawn: no swap, no wait for the display */
        skip_drawing = 0;
        last_swap = platform_ticks_us(); /* measure the next frame on its own */
        if (++skipped % 600 == 1)
            trace("# frame skip: %llu frames not drawn so far (frame %llu)", (unsigned long long)skipped, d3d_frames);
        return;
    }
#endif
    gl_enter();
    batch_flush();
#ifdef __vita__
    /* vitaGL only runs the frame's draws at the swap, so glReadPixels here returned
       the cleared target; the capture is copied from the displayed buffer below. */
    capture_w = VITA_W;
    capture_h = VITA_H;
    int read = 0;
#else
    capture_w = screen_w;
    capture_h = screen_h;
    int read = 1;
#endif
    if (!last_frame)
        last_frame = malloc((size_t)capture_w * capture_h * 4);
    if (read && last_frame)
        glReadPixels(0, 0, (GLsizei)capture_w, (GLsizei)capture_h, GL_RGBA, GL_UNSIGNED_BYTE, last_frame);
#ifdef __vita__
    /* The side bars of the 960x544 screen are outside the game's 4:3 area: clear them
       every frame, or each of vitaGL's display buffers keeps whatever was there. */
    if (VIEW_X > 0) {
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glClearColor(0, 0, 0, 1);
    glScissor(0, 0, VIEW_X, VITA_H);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(VIEW_X + VIEW_W, 0, VITA_W - VIEW_X - VIEW_W, VITA_H);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(VIEW_X, 0, VIEW_W, VITA_H);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    }
#endif
    {
        uint64_t s0 = platform_ticks_us();
        platform_swap();
        swap_us_sum += platform_ticks_us() - s0;
        swap_count++;
    }
    stage_panel = pause_panel = 0;
    if (wide_view) { /* the next frame starts black: menus only cover their centred 640x480 */
        GLfloat clear[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, FB_W, FB_H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        /* outside a fight, clip to the game's own 640x480: menus park sprites off screen */
        if (!scene_is_wide(rd32(0x8A0044)))
            glScissor(FB_W * 160 / 960, FB_H * 32 / 544, FB_W * 640 / 960, FB_H * 480 / 544);
    }
#ifdef __vita__
    {
        uint64_t now = platform_ticks_us(); /* a frame slower than ~50 FPS: skip drawing the next */
        if (last_swap && now - last_swap > 20000) skip_drawing = 1;
        last_swap = now;
    }
#endif
    /* (The screen used to be copied here once a second for the final BMP: reading 2 MiB of
       uncached framebuffer and waiting for a vblank cost 20-50 ms, one slow logic step
       every second. The copy now happens in render_capture_bmp, when it is needed.) */
    applied_valid = 0; /* clears, scissor and the swap went past apply_key */
    gl_leave();
#ifndef __vita__
    /* T123_CAPTURE="n[,n...]" saves frame-<n>.bmp at those presented frames, so a
       scripted run (T123_PRESS) can be checked screen by screen. */
    const char *list = getenv("T123_CAPTURE");
    for (const char *p = list; p && *p; ) {
        char *end;
        unsigned long long n = strtoull(p, &end, 10);
        if (end == p) break;
        if (n == d3d_frames) {
            char name[64];
            snprintf(name, sizeof name, "frame-%llu.bmp", n);
            render_capture_bmp(name);
        }
        p = *end == ',' ? end + 1 : end;
    }
#endif
    render_pump_events();
}

int render_capture_bmp(const char *path) {
#ifdef __vita__
    /* the displayed buffer is copied only now (see render_present) */
    if (!last_frame)
        last_frame = malloc((size_t)VITA_W * VITA_H * 4);
    capture_w = VITA_W;
    capture_h = VITA_H;
    if (last_frame) {
        SceDisplayFrameBuf fb;
        memset(&fb, 0, sizeof fb);
        fb.size = sizeof fb;
        if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0 && fb.base)
            for (uint32_t y = 0; y < VITA_H; y++) /* stored bottom-up, like GL rows */
                memcpy(last_frame + (size_t)(VITA_H - 1 - y) * VITA_W * 4,
                       (const uint8_t *)fb.base + (size_t)y * fb.pitch * 4, VITA_W * 4);
    }
#endif
    if (!last_frame)
        return -1;
    char full[1200];
#ifdef __vita__
    snprintf(full, sizeof full, "%s/recomp-screen.bmp", host_data_dir); /* where the Vita runner looks */
    (void)path;
#else
    snprintf(full, sizeof full, "%s", path);
#endif
    FILE *f = fopen(full, "wb");
    if (!f)
        return -1;
    uint32_t data = capture_w * capture_h * 3;
    uint32_t header[13] = {54 + data, 0, 54, 40, capture_w, capture_h, 0x00180001u, 0, data, 2835, 2835, 0, 0};
    fwrite("BM", 1, 2, f);
    fwrite(header, 4, 13, f);
    for (uint32_t i = 0; i < capture_w * capture_h; i++) { /* GL rows are bottom-up, like BMP */
        uint8_t bgr[3] = {last_frame[i * 4 + 2], last_frame[i * 4 + 1], last_frame[i * 4]};
        fwrite(bgr, 1, 3, f);
    }
    fclose(f);
    return 0;
}
