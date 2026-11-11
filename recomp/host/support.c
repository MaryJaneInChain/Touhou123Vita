/* Host support: guest heap, virtual memory, handles, path mapping, imports, traces. */
#include "win32.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

/* -- traces ---------------------------------------------------------------------------- */
/* trace() copies the line into a memory buffer; a writer thread puts it in the file. One
   fflush cost ~10 ms on the Vita's card, and the game thread used to pay it for every '#'
   line (each button change, each sound effect played, the per-second frame line): a skipped
   frame each time, ~17 s of 9 minutes stalled in sound effects and 23 s in button logging.
   Milestones and errors ('#') still wake the writer at once, so a crash report keeps them;
   other lines go out within a second; flush.txt in the data directory makes every line wake
   it (to find where a hang is). Two buffers: the writer writes one outside the lock while
   the other fills; a full buffer makes trace() wait. trace_close (also at exit) writes the
   rest synchronously. */
static FILE *trace_file;
unsigned long long import_calls;
char trace_path[256] = "host-trace.log";

enum { TRACE_BUF = 256 * 1024 };
static char trace_buf[2][TRACE_BUF];
static size_t trace_len;          /* bytes waiting in trace_buf[trace_cur] */
static int trace_cur, trace_urgent, trace_writing, trace_writer; /* trace_writer: 0 not started, 1 running, -1 failed */
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trace_wake = PTHREAD_COND_INITIALIZER, trace_done = PTHREAD_COND_INITIALIZER;

static void trace_write_file(const char *p, size_t n) { /* by whoever set trace_writing */
    if (!trace_file) {
        static int reopened; /* a line written after trace_close (a thread at exit) appends instead of wiping the log */
        trace_file = fopen(trace_path, reopened++ ? "a" : "w");
    }
    if (trace_file) {
        fwrite(p, 1, n, trace_file);
        fflush(trace_file);
    }
}

static size_t trace_take(const char **p) { /* under trace_lock: hands out the filled buffer */
    *p = trace_buf[trace_cur];
    size_t n = trace_len;
    trace_cur ^= 1;
    trace_len = 0;
    trace_urgent = 0;
    trace_writing = 1;
    return n;
}

static void *trace_writer_main(void *arg) {
    (void)arg;
    platform_pin_helper("trace writer");
    pthread_mutex_lock(&trace_lock);
    for (;;) {
        while (trace_writing || !trace_len || (!trace_urgent && trace_len < TRACE_BUF / 2)) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            if (pthread_cond_timedwait(&trace_wake, &trace_lock, &until) && trace_len && !trace_writing)
                break; /* a second went by: write what is there */
        }
        const char *p;
        size_t n = trace_take(&p);
        pthread_mutex_unlock(&trace_lock);
        trace_write_file(p, n);
        pthread_mutex_lock(&trace_lock);
        trace_writing = 0;
        pthread_cond_broadcast(&trace_done);
    }
    return NULL;
}

void trace(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    pthread_mutex_lock(&trace_lock);
    static int every = -1; /* flush.txt */
    if (every < 0) {
        char p[300];
        snprintf(p, sizeof p, "%.*s", (int)(strrchr(trace_path, '/') ? strrchr(trace_path, '/') - trace_path + 1 : 0), trace_path);
        strncat(p, "flush.txt", sizeof p - strlen(p) - 1);
        FILE *t = fopen(p, "rb");
        every = t != NULL;
        if (t) fclose(t);
    }
    if (!trace_writer) {
        pthread_t id;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 32 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        trace_writer = pthread_create(&id, &attr, trace_writer_main, NULL) ? -1 : 1;
        pthread_attr_destroy(&attr);
        atexit(trace_close);
    }
    while (trace_len + len + 1 > TRACE_BUF) { /* full */
        if (trace_writer < 0 && !trace_writing) { /* no writer thread: write here */
            const char *p;
            size_t n = trace_take(&p);
            trace_write_file(p, n);
            trace_writing = 0;
            continue;
        }
        trace_urgent = 1;
        pthread_cond_signal(&trace_wake);
        pthread_cond_wait(&trace_done, &trace_lock);
    }
    memcpy(trace_buf[trace_cur] + trace_len, line, len);
    trace_len += len;
    trace_buf[trace_cur][trace_len++] = '\n';
    if (every || line[0] == '#') {
        trace_urgent = 1;
        if (trace_writer > 0) pthread_cond_signal(&trace_wake);
        else if (!trace_writing) { const char *p; size_t n = trace_take(&p); trace_write_file(p, n); trace_writing = 0; }
    }
    platform_log(line);
    pthread_mutex_unlock(&trace_lock);
}

void trace_close(void) {
    pthread_mutex_lock(&trace_lock);
    while (trace_writing)
        pthread_cond_wait(&trace_done, &trace_lock);
    if (trace_len) {
        const char *p;
        size_t n = trace_take(&p);
        trace_write_file(p, n);
        trace_writing = 0;
    }
    if (trace_file) {
        fclose(trace_file);
        trace_file = NULL;
    }
    pthread_mutex_unlock(&trace_lock);
}

/* -- guest heap -------------------------------------------------------------------------
 * Size-class free lists (16-byte classes up to 4 KiB) plus a first-fit list for large
 * blocks; a 16-byte header before every block holds the requested size. Large blocks are
 * split on allocation and coalesced with their free neighbours (the DAT loader churns
 * multi-MiB buffers; without it 96 MiB were exhausted during boot). */
enum { HDR = 16, CLASS_STEP = 16, CLASSES = 256, MAGIC = 0x48454150u /* "HEAP" */ };
static uint32_t heap_top = GUEST_HEAP_BASE, heap_committed = GUEST_HEAP_BASE;
static uint32_t free_class[CLASSES + 1];
static uint32_t free_large;
static uint32_t heap_live;

static uint32_t round_up(uint32_t n) { return (n + CLASS_STEP - 1) & ~(uint32_t)(CLASS_STEP - 1); }

static uint32_t block_capacity(uint32_t p) { return rd32(p - HDR + 8); }

static uint32_t carve(uint32_t capacity) {
    uint32_t block = heap_top;
    if (capacity > GUEST_HEAP_BASE + GUEST_HEAP_SIZE - block - HDR) /* remaining space, no wrap */
        return 0;
    uint32_t end = block + HDR + capacity;
    while (heap_committed < end) {
        if (guest_map(heap_committed, 0x100000))
            return 0;
        heap_committed += 0x100000;
    }
    heap_top = end;
    wr32(block + 8, capacity);
    return block + HDR;
}

static uint32_t heap_alloc_locked(uint32_t size, int zero) {
    /* larger than the whole heap: refuse before round_up and carve can wrap around
       (Codex T10 found both overflows) */
    if (size > GUEST_HEAP_SIZE - HDR)
        return 0;
    uint32_t capacity = round_up(size ? size : 1), p = 0;
    if (capacity / CLASS_STEP <= CLASSES) {
        uint32_t k = capacity / CLASS_STEP;
        if (free_class[k]) {
            p = free_class[k];
            free_class[k] = rd32(p);
        }
    } else {
        /* first fit in the address-ordered list; the tail is split off when it can
           still hold a large block */
        uint32_t prev = 0;
        for (uint32_t q = free_large; q; prev = q, q = rd32(q)) {
            uint32_t have = block_capacity(q);
            if (have < capacity)
                continue;
            uint32_t next = rd32(q);
            if (have - capacity >= HDR + (CLASSES + 1) * CLASS_STEP) {
                uint32_t rest = q + capacity + HDR;
                wr32(rest - HDR + 8, have - capacity - HDR);
                wr32(rest - HDR + 4, 0);
                wr32(rest, next);
                next = rest;
                wr32(q - HDR + 8, capacity);
            }
            if (prev) wr32(prev, next); else free_large = next;
            p = q;
            break;
        }
    }
    if (!p)
        p = carve(capacity);
    if (!p) {
        trace("# heap exhausted: request %u bytes, live %u KiB", size, heap_live >> 10);
        return 0;
    }
    heap_live += block_capacity(p);
    wr32(p - HDR, size);
    wr32(p - HDR + 4, MAGIC);
    if (zero)
        memset(G(p), 0, block_capacity(p));
    return p;
}

static void heap_free_locked(uint32_t p) {
    if (!p || rd32(p - HDR + 4) != MAGIC)
        return;
    wr32(p - HDR + 4, 0);
    uint32_t capacity = block_capacity(p);
    heap_live -= capacity;
    if (capacity / CLASS_STEP <= CLASSES) {
        wr32(p, free_class[capacity / CLASS_STEP]);
        free_class[capacity / CLASS_STEP] = p;
    } else {
        /* address-ordered insert, merging with the neighbours that touch */
        uint32_t prev = 0, q = free_large;
        while (q && q < p) { prev = q; q = rd32(q); }
        if (q && p + capacity + HDR == q) {
            capacity += HDR + block_capacity(q);
            q = rd32(q);
        }
        if (prev && prev + block_capacity(prev) + HDR == p) {
            wr32(prev - HDR + 8, block_capacity(prev) + HDR + capacity);
            wr32(prev, q);
            return;
        }
        wr32(p - HDR + 8, capacity);
        wr32(p, q);
        if (prev) wr32(prev, p); else free_large = p;
    }
}

uint32_t heap_size(uint32_t p) {
    return p && rd32(p - HDR + 4) == MAGIC ? rd32(p - HDR) : 0xFFFFFFFFu;
}

static uint32_t heap_realloc_locked(uint32_t p, uint32_t size, int zero) {
    if (!p)
        return heap_alloc_locked(size, zero);
    uint32_t old = heap_size(p);
    if (size <= block_capacity(p)) {
        if (zero && size > old)
            memset(G(p + old), 0, size - old);
        wr32(p - HDR, size);
        return p;
    }
    uint32_t q = heap_alloc_locked(size, zero);
    if (!q)
        return 0;
    memcpy(G(q), G(p), old);
    heap_free_locked(p);
    return q;
}

/* The guest heap is shared by all guest threads. */
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t heap_alloc(uint32_t size, int zero) {
    pthread_mutex_lock(&heap_lock);
    uint32_t p = heap_alloc_locked(size, zero);
    pthread_mutex_unlock(&heap_lock);
    return p;
}
void heap_free(uint32_t p) {
    pthread_mutex_lock(&heap_lock);
    heap_free_locked(p);
    pthread_mutex_unlock(&heap_lock);
}
uint32_t heap_realloc(uint32_t p, uint32_t size, int zero) {
    pthread_mutex_lock(&heap_lock);
    uint32_t q = heap_realloc_locked(p, size, zero);
    pthread_mutex_unlock(&heap_lock);
    return q;
}

uint32_t heap_used(void) { return heap_top - GUEST_HEAP_BASE; }
uint32_t heap_live_bytes(void) { return heap_live; }

uint32_t guest_strdup(const char *s) {
    uint32_t p = heap_alloc((uint32_t)strlen(s) + 1, 0);
    if (p)
        strcpy(STR(p), s);
    return p;
}

/* -- virtual memory: bump allocation in its own region, committed on request -------- */
static uint32_t virtual_top = GUEST_VIRTUAL_BASE;

uint32_t virtual_used(void) { return virtual_top - GUEST_VIRTUAL_BASE; }

uint32_t virtual_alloc(uint32_t address, uint32_t size) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    if (address) {
        /* Commit inside an earlier reservation. */
        return guest_map(address, size) ? 0 : address;
    }
    if (virtual_top + size > GUEST_VIRTUAL_BASE + GUEST_VIRTUAL_SIZE || guest_map(virtual_top, size))
        return 0;
    uint32_t r = virtual_top;
    virtual_top += size;
    return r;
}

/* -- handles --------------------------------------------------------------------------- */
typedef struct { int type; void *host; uint32_t extra; } Handle;
static Handle handles[4096];
uint32_t last_error;

static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int type, void *host, uint32_t extra) {
    pthread_mutex_lock(&handle_lock);
    for (unsigned i = 1; i < 4096; i++) {
        if (!handles[i].type) {
            handles[i] = (Handle){type, host, extra};
            pthread_mutex_unlock(&handle_lock);
            return 0x1000u + i * 4;
        }
    }
    pthread_mutex_unlock(&handle_lock);
    return 0;
}

static Handle *slot(uint32_t h) {
    if (h < 0x1000u || ((h - 0x1000u) & 3))
        return NULL;
    unsigned i = (h - 0x1000u) / 4;
    return i < 4096 && handles[i].type ? &handles[i] : NULL;
}

void *handle_get(uint32_t h, int type) {
    Handle *s = slot(h);
    return s && s->type == type ? s->host : NULL;
}

void handle_close(uint32_t h) {
    Handle *s = slot(h);
    if (!s)
        return;
    if (s->type == H_FILE && s->host)
        fclose(s->host);
    if (s->type == H_RFILE && s->host)
        rfile_close(s->host);
    if (s->type == H_FIND && s->host)
        find_close(s->host);
    memset(s, 0, sizeof *s);
}

/* -- paths ------------------------------------------------------------------------------- */
char host_data_dir[1024] = ".";
const char *guest_exe_path = "C:\\th123\\th123.exe";

void path_to_host(const char *guest_path, char *out, size_t size) {
    const char *p = guest_path;
    /* the game joins its directory with '/' too ("C:\th123/configex123.ini") */
    static const char prefix[] = "C:\\th123";
    if (!_strnicmp(p, prefix, sizeof prefix - 1) && (p[8] == '\\' || p[8] == '/' || !p[8]))
        p += sizeof prefix - 1;
    else if (p[0] && p[1] == ':')
        p += 2; /* other absolute paths are kept under the data directory */
    while (*p == '\\' || *p == '/')
        p++;
    snprintf(out, size, "%s/%s", host_data_dir, p);
    for (char *q = out; *q; q++)
        if (*q == '\\')
            *q = '/';
}

/* -- imports ------------------------------------------------------------------------------ */
typedef struct { char name[64]; GuestFn fn; unsigned long long calls, us; } Fake; /* us: host time inside */
static Fake fakes[1024];
static unsigned fake_count;
static unsigned recent[64], recent_pos;

/* Every fake address dispatches here: count, remember, then run the implementation. */
static void api_dispatch(Cpu *c) {
    static int stats = -1;
    if (stats < 0) stats = host_stats();
    unsigned i = (c->current_target - GUEST_FAKE_BASE) / 16;
    if (!stats) { /* no timer reads: on the Vita each is a system call, ~22 000 calls a second */
        uint32_t prev = c->cur_fn; /* the sampling profiler charges host time to the import */
        c->cur_fn = c->current_target;
        fakes[i].fn(c);
        c->cur_fn = prev;
        return;
    }
    fakes[i].calls++;
    import_calls++;
    recent[recent_pos++ % 64] = i;
    uint64_t t0 = platform_ticks_us();
    fakes[i].fn(c);
    fakes[i].us += platform_ticks_us() - t0;
}

void trace_summary(void) {
    trace("# memory: heap %u KiB, VirtualAlloc %u KiB", heap_used() / 1024, virtual_used() / 1024);
    trace("# Win32 calls by function:");
    for (unsigned i = 0; i < fake_count; i++)
        if (fakes[i].calls)
            trace("#   %8llu %s (%llu ms)", fakes[i].calls, fakes[i].name, fakes[i].us / 1000);
    com_summary();
    trace("# last calls (oldest first):");
    unsigned n = recent_pos < 64 ? recent_pos : 64;
    for (unsigned k = 0; k < n; k++)
        trace("#   %s", fakes[recent[(recent_pos - n + k) % 64]].name);
}

static void unimplemented(Cpu *c) {
    const char *name = api_name(c->current_target);
    char why[160];
    snprintf(why, sizeof why, "import %s is not implemented", name ? name : "?");
    host_exit(c, 0xDEAD, why);
}

static GuestFn lookup(const char *name) {
    const ApiEntry *tables[] = {win32_threads, win32_d3d9, win32_media, win32_kernel32, win32_other};
    for (unsigned t = 0; t < 5; t++)
        for (const ApiEntry *e = tables[t]; e->name; e++)
            if (!strcmp(e->name, name))
                return e->fn;
    return NULL;
}

static uint32_t fake_for(const char *name, int create_unimplemented) {
    for (unsigned i = 0; i < fake_count; i++)
        if (!strcmp(fakes[i].name, name))
            return GUEST_FAKE_BASE + 16u * i;
    GuestFn fn = lookup(name);
    if (!fn && !create_unimplemented)
        return 0;
    if (fake_count == 1024)
        return 0;
    snprintf(fakes[fake_count].name, sizeof fakes[0].name, "%s", name);
    fakes[fake_count].fn = fn ? fn : unimplemented;
    uint32_t address = GUEST_FAKE_BASE + 16u * fake_count;
    guest_intercept(address, api_dispatch);
    fake_count++;
    return address;
}

const char *api_name(uint32_t fake) {
    if (fake < GUEST_FAKE_BASE || (fake - GUEST_FAKE_BASE) / 16 >= fake_count)
        return NULL;
    return fakes[(fake - GUEST_FAKE_BASE) / 16].name;
}

uint32_t api_address(const char *name) { return fake_for(name, 0); }

int imports_init(void) {
    uint32_t base = guest_image_base;
    uint32_t pe = base + rd32(base + 0x3C);
    uint32_t dir = rd32(pe + 24 + 104); /* optional header: import directory RVA */
    unsigned total = 0, implemented = 0;
    for (uint32_t d = base + dir; rd32(d + 12); d += 20) {
        uint32_t names = rd32(d) ? rd32(d) : rd32(d + 16);
        uint32_t iat = base + rd32(d + 16);
        for (unsigned i = 0;; i++) {
            uint32_t thunk = rd32(base + names + 4 * i);
            if (!thunk)
                break;
            char name[64];
            /* WS2_32 is imported by ordinal; the ones th123.exe uses get their names */
            static const struct { unsigned ordinal; const char *name; } ws2[] = {
                {2, "bind"}, {3, "closesocket"}, {8, "htonl"}, {9, "htons"}, {11, "inet_addr"},
                {15, "ntohs"}, {17, "recvfrom"}, {20, "sendto"}, {22, "shutdown"}, {23, "socket"},
                {52, "gethostbyname"}, {111, "WSAGetLastError"}, {115, "WSAStartup"}, {116, "WSACleanup"}};
            const char *named = NULL;
            if (thunk & 0x80000000u && !strcasecmp(STR(base + rd32(d + 12)), "WS2_32.dll"))
                for (unsigned k = 0; k < sizeof ws2 / sizeof ws2[0]; k++)
                    if (ws2[k].ordinal == (thunk & 0xFFFF)) named = ws2[k].name;
            if (named)
                snprintf(name, sizeof name, "%s", named);
            else if (thunk & 0x80000000u)
                snprintf(name, sizeof name, "%s#%u", STR(base + rd32(d + 12)), thunk & 0xFFFF);
            else
                snprintf(name, sizeof name, "%s", STR(base + thunk + 2));
            wr32(iat + 4 * i, fake_for(name, 1));
            total++;
            implemented += lookup(name) != NULL;
        }
    }
    trace("# imports: %u, implemented: %u", total, implemented);
    return 0;
}
