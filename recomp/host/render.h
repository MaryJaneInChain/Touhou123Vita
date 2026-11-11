/* Rendering backend behind the D3D9 layer: fixed-function OpenGL (desktop GL on PC,
 * vitaGL's fixed-function emulation on the Vita for H3). */
#ifndef T123_RENDER_H
#define T123_RENDER_H

#include <stddef.h>
#include <stdint.h>

#define RENDER_MAX_TEXTURES 4096

typedef struct {
    uint32_t width, height, format, pitch;
    uint8_t *data;   /* level 0 in host memory (deflated on the Vita: see d3d9.c) */
    uint64_t stored; /* bytes held in data */
    uint8_t *raw;    /* Vita: uncompressed copy while its compression job is pending */
    uint32_t gen;    /* Vita: generation of the stored contents (compression jobs) */
    uint32_t pixels; /* guest staging copy while locked, else 0 */
    int dirty;
    unsigned gl;
    unsigned next_free; /* d3d9.c slot free list */
    unsigned drawn;     /* frames in which it was bound for a draw (measurement) */
    unsigned long long used_frame; /* last frame it was drawn in (GPU budget) */
    uint32_t gpu_bytes;           /* bytes uploaded to the GPU, 0 if not uploaded */
    uint8_t half;                 /* uploaded at half resolution (large backgrounds) */
    uint8_t gpu16;                /* uploaded as RGBA4444 (half the GPU memory) */
} RenderTexture;

extern unsigned long long texture_parallel_uploads; /* d3d9.c: uploads inflated by worker threads */
int render_init(uint32_t width, uint32_t height);
void render_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);
void render_state(uint32_t state, uint32_t value);
uint32_t render_get_state(uint32_t state);
void render_texture(uint32_t stage, RenderTexture *t);
void render_texture_free(RenderTexture *t);
void render_texture_pixels(RenderTexture *t, uint8_t *out); /* level 0 as stored by the game (d3d9.c) */
void render_texture_rows(RenderTexture *t, uint8_t *buf, size_t size,
                         void (*sink)(void *ctx, const uint8_t *rows, uint32_t y, uint32_t n), void *ctx);
void render_stage_state(uint32_t stage, uint32_t type, uint32_t value);
void render_sampler_state(uint32_t sampler, uint32_t type, uint32_t value);
void render_fvf(uint32_t fvf);
void render_draw_up(uint32_t type, uint32_t count, uint32_t data, uint32_t stride);
void render_present(void);
int render_pump_events(void);  /* returns 1 when the user closed the window */
int render_quit_requested(void);
void render_keyboard_state(uint8_t dik[256]); /* DirectInput key state, 0x80 = down */
int render_capture_bmp(const char *path);

#endif
