/* Vita platform hooks for the recompiled host (H1): an on-screen log of the trace,
 * microsecond time, and a final report written to ux0:data/touhou123vita/
 * (recomp-result.txt and a BMP capture of the screen) for automated runs. */
#include "win32.h"

#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 960, H = 544, LINES = 32, COLS = 104, CELL_W = 12, CELL_H = 16, ADVANCE = 9 };

/* Without this, VitaSDK's newlib reserves most of the user memory for malloc at
   start-up and the 128 MiB guest window no longer fits (seen on real hardware;
   Vita3K is more permissive). The host itself only mallocs the .exe it loads. */
int _newlib_heap_size_user = 32 * 1024 * 1024;

/* Maximum clocks, set before main() so the 30 s of loading benefits too: the guest's
   own code is CPU bound (~17 ms of "logic" per frame at the default 333 MHz) and the
   user had been raising the clocks by hand from VitaShell. Values above the defaults
   need the app installed as an unsafe homebrew; if the call is refused the result
   shows in the trace line "# clocks" written by platform_clock_report. */
static int clock_result[4], clock_before, clock_500;
__attribute__((constructor)) static void set_max_clocks(void) {
    /* Never lower a clock someone else already raised (PSVshell's 500 MHz): try 500
       (refused without an overclock plugin), then 444 only when the CPU is below it. */
    clock_before = scePowerGetArmClockFrequency();
    clock_500 = scePowerSetArmClockFrequency(500);
    clock_result[0] = clock_500;
    if (clock_500 < 0 && clock_before < 444)
        clock_result[0] = scePowerSetArmClockFrequency(444);
    else if (clock_500 < 0)
        clock_result[0] = 0; /* already at or above 444: left alone */
    clock_result[1] = scePowerSetBusClockFrequency(222);
    clock_result[2] = scePowerSetGpuClockFrequency(222);
    clock_result[3] = scePowerSetGpuXbarClockFrequency(166);
}

void platform_clock_report(char *out, size_t size) {
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr)); /* FZ is bit 24, DN bit 25 */
    snprintf(out, size, "fpscr %08x; was %d MHz, asking for 500 gave %08x; ARM %d MHz (set %d), bus %d MHz (set %d), GPU %d MHz (set %d), GPU xbar %d MHz (set %d)",
             fpscr, clock_before, (unsigned)clock_500,
             scePowerGetArmClockFrequency(), clock_result[0], scePowerGetBusClockFrequency(), clock_result[1],
             scePowerGetGpuClockFrequency(), clock_result[2], scePowerGetGpuXbarClockFrequency(), clock_result[3]);
}

/* nosound.txt in the data directory: the game runs with no audio at all (to test whether
   mixing and decoding the music cost frame rate or stability) */
int host_stats(void) {
    static int on = -1;
    if (on < 0) {
        FILE *f = fopen("ux0:data/touhou123vita/stats.txt", "rb");
        on = f != NULL;
        if (f) fclose(f);
    }
    return on;
}

int host_nosound(void) {
    static int state = -1;
    if (state < 0) {
        FILE *f = fopen("ux0:data/touhou123vita/nosound.txt", "rb");
        state = f != NULL;
        if (f) fclose(f);
    }
    return state;
}

void platform_memory_report(char *out, size_t size) {
    SceKernelFreeMemorySizeInfo info = {sizeof info};
    if (sceKernelGetFreeMemorySize(&info) < 0) {
        snprintf(out, size, "free memory unknown");
        return;
    }
    snprintf(out, size, "free memory: user %u KiB, cdram %u KiB, phycont %u KiB",
             (unsigned)(info.size_user / 1024), (unsigned)(info.size_cdram / 1024),
             (unsigned)(info.size_phycont / 1024));
}

static uint32_t *frame;
static uint32_t *font;
static unsigned font_w, font_h;
static char lines[LINES][COLS + 1];
static unsigned line_count;
static uint64_t last_draw;
static int display_taken; /* vitaGL owns the screen from CreateDevice on */

void platform_display_taken(void) { display_taken = 1; }

static void init_display(void) {
    if (frame)
        return;
    SceUID block = sceKernelAllocMemBlock("t123 frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                          0x200000, NULL);
    void *base = NULL;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0)
        return;
    frame = base;
    memset(frame, 0, W * H * 4);
    SceDisplayFrameBuf fb = {sizeof fb, frame, W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, W, H};
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    FILE *f = fopen("app0:assets/font.rgba", "rb");
    if (f) {
        uint32_t size[2];
        if (fread(size, 4, 2, f) == 2 && (font = malloc((size_t)size[0] * size[1] * 4)) &&
            fread(font, 4, (size_t)size[0] * size[1], f) == (size_t)size[0] * size[1]) {
            font_w = size[0];
            font_h = size[1];
        }
        fclose(f);
    }
}

static void draw_text(int x, int y, const char *s, uint32_t color) {
    for (; *s && x + CELL_W <= W; s++, x += ADVANCE) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch >= 127)
            ch = '?';
        if (!font_w)
            continue;
        for (int py = 0; py < CELL_H; py++)
            for (int px = 0; px < CELL_W; px++) {
                uint32_t p = font[(ch / 16 * 16 + py) * font_w + ch % 16 * 12 + px];
                if (p >> 24)
                    frame[(y + py) * W + x + px] = color;
            }
    }
}

static void redraw(void) {
    if (display_taken)
        return;
    init_display();
    if (!frame)
        return;
    memset(frame, 0, W * H * 4);
    draw_text(8, 4, "Touhou 12.3 recompiled - H1 boot test (recomp/vita)", 0xFF60D0FFu);
    unsigned first = line_count > LINES - 2 ? line_count - (LINES - 2) : 0;
    for (unsigned i = first; i < line_count; i++)
        draw_text(8, 4 + (int)(i - first + 1) * CELL_H, lines[i % LINES],
                  lines[i % LINES][0] == '#' ? 0xFFA0A0A0u : 0xFFFFFFFFu);
}

uint64_t platform_ticks_us(void) { return sceKernelGetProcessTimeWide(); }

void platform_sleep_ms(uint32_t ms) { sceKernelDelayThread(ms * 1000u); }

/* cores.txt in the data directory: the game thread (logic + drawing) alone on user core 1,
   every other thread (the game's own, the host's helpers and the main thread) on cores 0 and
   2, so nothing preempts it there (threads.c pins the game's threads). */
int platform_split_cores(void) {
    static int on = -1;
    if (on < 0) {
        FILE *f = fopen("ux0:data/touhou123vita/cores.txt", "rb");
        on = f != NULL;
        if (f) fclose(f);
    }
    return on;
}
void platform_pin_helper(const char *what) {
    if (!platform_split_cores()) return;
    int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000 | 0x40000); /* USER_0 | USER_2 */
    trace("# thread %s: cores 0+2 (%08x)", what, (unsigned)r);
}

void platform_log(const char *line) {
    if (!strncmp(line, "#   ", 4) || !strncmp(line, "GetProcAddress", 14) || !strncmp(line, "GetModuleHandleA", 16))
        return; /* per-call detail stays in the trace file; the screen shows milestones */
    snprintf(lines[line_count % LINES], COLS + 1, "%s", line);
    line_count++;
    uint64_t now = platform_ticks_us();
    if (now - last_draw > 100000) {
        last_draw = now;
        redraw();
    }
}

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f || !frame)
        return;
    uint32_t data = W * H * 3, header[13] = {0, 0, 54, 40, W, (uint32_t)-H, 0x00180001u, 0, data, 2835, 2835, 0, 0};
    header[0] = 54 + data;
    fwrite("BM", 1, 2, f);
    fwrite(header, 4, 13, f);
    for (int i = 0; i < W * H; i++) {
        uint32_t p = frame[i];
        uint8_t bgr[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
        fwrite(bgr, 1, 3, f);
    }
    fclose(f);
}

void platform_finish(const char *summary) {
    platform_log(summary);
    redraw();
    char path[1100];
    snprintf(path, sizeof path, "%s/recomp-screen.bmp", host_data_dir);
    if (!display_taken) /* otherwise the renderer wrote the game frame there */
        write_bmp(path);
    snprintf(path, sizeof path, "%s/recomp-result.txt", host_data_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", summary);
        fclose(f);
    }
    if (!display_taken)
        sceKernelDelayThread(5 * 1000 * 1000); /* leave the result on screen */
    sceKernelExitProcess(0);
}
