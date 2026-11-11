/* PC platform hooks for the host (the Vita versions are in recomp/vita/). */
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void platform_log(const char *line) { (void)line; }

uint64_t platform_ticks_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    static uint64_t start; /* relative to the first call, like the Vita's process time */
    if (!start) start = now;
    return now - start;
}

void platform_finish(const char *summary) { printf("%s\n", summary); }

int platform_split_cores(void) { return 0; }
void platform_pin_helper(const char *what) { (void)what; }
void platform_sleep_ms(uint32_t ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

void platform_memory_report(char *out, size_t size) { snprintf(out, size, "(no memory report on PC)"); }

int host_stats(void) { return 1; }

int host_nosound(void) { return getenv("T123_NOSOUND") != NULL; }

/* PC: no networking behind the game's WS2_32 calls (they fail with WSAENETDOWN) */
#include "net.h"
int net_enabled(void) { return 0; }
void net_poll(void) {}
const char *net_own_ip(void) { return ""; }
int net_open(void) { return -1; }
int net_bind(int s, uint16_t port_be) { (void)s; (void)port_be; return -1; }
int net_send(int s, const void *data, int len, uint32_t addr_be, uint16_t port_be) { (void)s; (void)data; (void)len; (void)addr_be; (void)port_be; return -1; }
int net_recv(int s, void *buf, int len, uint32_t *addr_be, uint16_t *port_be) { (void)s; (void)buf; (void)len; (void)addr_be; (void)port_be; return -1; }
int net_shutdown(int s) { (void)s; return -1; }
int net_close(int s) { (void)s; return -1; }

/* PC: the real keyboard reaches the game through SDL, so there is no on-screen one */
void ime_poll(void) {}
int ime_dialog_active(void) { return 0; }

void *platform_texture_alloc(size_t bytes) { return calloc(1, bytes); }
void platform_texture_free(void *p) { free(p); }

void platform_texture_shrink(void *p, size_t bytes) { (void)p; (void)bytes; } /* calloc blocks stay whole */
