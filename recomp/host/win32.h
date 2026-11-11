/* Host implementation of the Win32 surface th123.exe imports (F2).
 *
 * Every import is a GuestFn reached through a fake address written into the IAT.
 * Implementations read stdcall arguments from the guest stack and return with
 * ret_stdcall, which pops the return address and the arguments like the callee
 * would. Unimplemented imports stop the program with their name.
 */
#ifndef T123_WIN32_H
#define T123_WIN32_H

#include "guest.h"
#include <stdio.h>

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

/* Guest memory map, identical on PC and Vita. Everything the guest can dereference
 * lies below GUEST_WINDOW (128 MiB), so on the Vita it is one memory block:
 *   0x00110000 stack (2 MiB)      0x00400000 image (th123.exe, to 0x008A4000)
 *   0x00900000 TIB, PEB           0x01000000 heap (96 MiB; Vita 80 MiB)
 *   0x07000000 VirtualAlloc (16 MiB; Vita: 4 MiB at 0x06000000)
 *   Vita only: 0x06400000-0x08000000 (28 MiB) holds host texture storage. The guest
 *   heap peaked at ~57 MiB in fights and VirtualAlloc was never used, while the
 *   compressed-texture pool ran out of memory after a fight (hardware).
 * Fake call targets and module handles sit above the window: they are compared
 * and called, never read. */
enum {
    GUEST_STACK_BASE = 0x00110000u, GUEST_STACK_SIZE = 0x00200000u,
    GUEST_TIB = 0x00900000u, GUEST_PEB = 0x00901000u,
#ifdef __vita__
    GUEST_HEAP_BASE = 0x01000000u, GUEST_HEAP_SIZE = 0x05000000u,
    GUEST_VIRTUAL_BASE = 0x06000000u, GUEST_VIRTUAL_SIZE = 0x00400000u,
    HOST_TEXTURE_BASE = 0x06400000u, HOST_TEXTURE_SIZE = 0x01C00000u,
#else
    GUEST_HEAP_BASE = 0x01000000u, GUEST_HEAP_SIZE = 0x06000000u,
    GUEST_VIRTUAL_BASE = 0x07000000u, GUEST_VIRTUAL_SIZE = 0x01000000u,
#endif
    GUEST_FAKE_BASE = 0x7F000000u,
    FAKE_MODULE_KERNEL32 = 0x7C800000u, FAKE_MODULE_OTHER = 0x7C900000u,
};
_Static_assert(GUEST_VIRTUAL_BASE + GUEST_VIRTUAL_SIZE <= GUEST_WINDOW, "guest map exceeds the window");

uint32_t heap_used(void);
uint32_t heap_live_bytes(void);
uint32_t virtual_used(void);
extern char trace_path[256];
/* Platform hooks (platform_pc.c / recomp/vita/platform_vita.c). */
void platform_log(const char *line);   /* mirrors trace lines (Vita: on-screen log) */
uint64_t platform_ticks_us(void);      /* monotonic microseconds */
void platform_sleep_ms(uint32_t ms);
int platform_split_cores(void);       /* Vita, cores.txt: the game thread has user core 1 to itself */
void platform_pin_helper(const char *what); /* with cores.txt: keep the calling thread on cores 0 and 2 */
/* Texture storage: zeroed calloc on PC (unpacked textures start blank), an unzeroed
   pool (user RAM + CDRAM chunks) on the Vita, whose callers overwrite every byte. */
void *platform_texture_alloc(size_t bytes);
void platform_texture_free(void *p);
void platform_texture_shrink(void *p, size_t bytes); /* give back the tail of a block */
#ifdef __vita__
void texture_pool_reserve(void);      /* before vitaGL takes the remaining memory */
void platform_clock_report(char *out, size_t size); /* Vita: clocks set at start-up and what they read back as */
void platform_display_taken(void);    /* the renderer owns the screen: stop the text log */
#endif
int host_stats(void);
void vprof_register(Cpu *c, uint32_t tag); /* vprof.c: sampling profiler (prof.txt) */         /* stats.txt (Vita) / always (PC): per-call timing of Win32 and COM calls for the trace summary; off = no timer reads in the call path */
int host_nosound(void);      /* nosound.txt (Vita) / T123_NOSOUND (PC): no audio output, mixing or music decoding */
/* Text input (the Vita keyboard): synthetic window messages and the text the IME returns */
void host_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam); /* other.c */
void host_ime_set_result(const uint8_t *sjis, size_t n);                /* other.c */
void ime_poll(void);          /* once per presented frame; vita/ime_vita.c, a no-op on PC */
int ime_dialog_active(void);  /* the common dialog is on screen: present with vglSwapBuffers(GL_TRUE) */
void platform_finish(const char *summary);
void platform_memory_report(char *out, size_t size);

#define ARG(c, n) rd32((c)->esp.d + 4u + 4u * (n))
#define STR(a) ((char *)G(a))

static inline void ret_stdcall(Cpu *c, uint32_t value, unsigned nargs) {
    c->eax.d = value;
    c->esp.d += 4u + 4u * nargs;
}
static inline void ret_cdecl(Cpu *c, uint32_t value) {
    c->eax.d = value;
    c->esp.d += 4u;
}

/* -- registry ---------------------------------------------------------------------- */
typedef struct { const char *name; GuestFn fn; } ApiEntry;
extern const ApiEntry win32_threads[];
extern const ApiEntry win32_d3d9[];
extern const ApiEntry win32_media[];
void d3d9_init(void);
void media_init(void);
void crt_native_init(void);
void d3d9_set_frame_limit(unsigned long long frames);
extern unsigned long long d3d_frames, d3d_draws;
/* per-thread time blocked in WaitForSingleObject / Sleep (microseconds), read by the
   frame pacing report in render_gl.c */
extern __thread uint64_t host_wait_us, host_sleep_us, host_wait_calls;
extern const ApiEntry win32_kernel32[];
extern const ApiEntry win32_other[];

int imports_init(void);                     /* fills the IAT with fake addresses */
uint32_t api_address(const char *name);     /* fake address for GetProcAddress, 0 if absent */
const char *api_name(uint32_t fake);        /* for traces */

/* -- support ----------------------------------------------------------------------- */
void trace(const char *fmt, ...);
void trace_summary(void);
void trace_close(void); /* before the process ends: the Vita does not flush on exit */
extern unsigned long long import_calls;
_Noreturn void host_exit(Cpu *c, uint32_t code, const char *why);

uint32_t heap_alloc(uint32_t size, int zero);
void heap_free(uint32_t p);
uint32_t heap_size(uint32_t p);
uint32_t heap_realloc(uint32_t p, uint32_t size, int zero);
uint32_t guest_strdup(const char *s);
uint32_t virtual_alloc(uint32_t address, uint32_t size);

void path_to_host(const char *guest_path, char *out, size_t size);
extern char host_data_dir[1024];
extern const char *guest_exe_path; /* the path the game sees for itself */

uint32_t handle_new(int type, void *host, uint32_t extra);
void *handle_get(uint32_t h, int type);
void handle_close(uint32_t h);
enum { H_FILE = 1, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_STD, H_RFILE /* read-only, kernel32.c */ };

extern uint32_t last_error;

#endif

extern uint64_t texture_live, texture_peak;
void texture_census(void);
void com_summary(void);
void rfile_close(void *r); /* kernel32.c: keeps idle read-only files for reuse */
GuestFn guest_lifted(uint32_t address); /* runtime/guest.c: the translated original, for wrappers */
void game_checks_init(void); /* game_checks.c */
void vorbis_init(void);      /* vorbis.c: native vorbisfile */
void vorbis_stats(uint64_t *us, uint64_t *bytes);
void th123e_init(void);      /* th123e.c: English patch resources */
void audio_out_start(void); /* audio_out_sdl.c (PC) / vita/audio_out_vita.c: plays audio_mix */
#ifndef __vita__
void profile_start(void); void profile_register_thread(uint32_t guest_id); void profile_write(void); /* profile_pc.c */
#endif
void find_close(void *f); /* kernel32.c: FindFirstFileA state */
