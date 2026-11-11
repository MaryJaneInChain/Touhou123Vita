/* PC sampling profiler for the recompiled game (T123_PROFILE=1): a thread interrupts
 * every registered guest thread about once a millisecond and records where it is
 * running. profile.txt lists "thread host-address count"; tools map the addresses to
 * the generated functions (f_00xxxxxx = the game function at that address). */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "win32.h"

enum { MAX_PROFILED = 32, BUCKETS = 1 << 16 };
static struct { HANDLE h; uint32_t guest_id; } profiled[MAX_PROFILED];
static volatile LONG profiled_count;
static struct { uint64_t rip; uint32_t thread, count; } samples[BUCKETS];
static volatile int running;

void profile_register_thread(uint32_t guest_id) {
    if (!running) return;
    LONG i = InterlockedIncrement(&profiled_count) - 1;
    if (i >= MAX_PROFILED) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &profiled[i].h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    profiled[i].guest_id = guest_id;
}

static void record(uint64_t rip, uint32_t thread) {
    uint32_t k = (uint32_t)((rip >> 2) * 2654435761u + thread) & (BUCKETS - 1);
    for (unsigned n = 0; n < BUCKETS; n++, k = (k + 1) & (BUCKETS - 1)) {
        if (samples[k].count && (samples[k].rip != rip || samples[k].thread != thread)) continue;
        samples[k].rip = rip;
        samples[k].thread = thread;
        samples[k].count++;
        return;
    }
}

static DWORD WINAPI sampler(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    const char *from = getenv("T123_PROFILE_FROM"); /* first presented frame to sample */
    unsigned long long first = from ? strtoull(from, NULL, 10) : 0;
    while (running) {
        if (d3d_frames < first) { Sleep(5); continue; }
        LONG n = profiled_count < MAX_PROFILED ? profiled_count : MAX_PROFILED;
        for (LONG i = 0; i < n; i++) {
            if (!profiled[i].h || SuspendThread(profiled[i].h) == (DWORD)-1) continue;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(profiled[i].h, &ctx))
                record(ctx.Rip, profiled[i].guest_id);
            ResumeThread(profiled[i].h);
        }
        Sleep(1);
    }
    return 0;
}

void profile_start(void) {
    if (!getenv("T123_PROFILE")) return;
    running = 1;
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}

void profile_write(void) {
    if (!running) return;
    running = 0;
    Sleep(5);
    FILE *f = fopen("profile.txt", "w");
    if (!f) return;
    fprintf(f, "base %016llx\n", (unsigned long long)(uintptr_t)GetModuleHandle(NULL)); /* ASLR */
    for (unsigned k = 0; k < BUCKETS; k++)
        if (samples[k].count)
            fprintf(f, "%08x %016llx %u\n", samples[k].thread, (unsigned long long)samples[k].rip, samples[k].count);
    fclose(f);
}
#else
/* Linux PC build: no sampling profiler */
#include <stdint.h>
void profile_register_thread(uint32_t guest_id) { (void)guest_id; }
void profile_start(void) {}
void profile_write(void) {}
#endif
