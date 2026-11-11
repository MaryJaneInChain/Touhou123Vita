/* Direct3D 9 and D3DX for the recompiled game, limited to what docs/COM_USAGE.md
 * (Codex T3) measured: device, swap chain, textures/surfaces, DrawPrimitiveUP,
 * render/sampler/stage state, effects. Rendering goes through render.h; this file
 * owns the COM surface and the guest-visible data (caps, descriptors, locks). */
#include "com.h"
#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <zlib.h>
#include <string.h>

#define API(name) static void name(Cpu *c)

static ComClass d3d9_class = COM_CLASS(IDirect3D9);
static ComClass device_class = COM_CLASS(IDirect3DDevice9);
static ComClass swapchain_class = COM_CLASS(IDirect3DSwapChain9);
static ComClass texture_class = COM_CLASS(IDirect3DTexture9);
static ComClass surface_class = COM_CLASS(IDirect3DSurface9);
static ComClass effect_class = COM_CLASS(ID3DXEffect);

static uint32_t device_object, swapchain_object;
unsigned long long d3d_frames, d3d_draws;
#ifdef __vita__
static unsigned long long frame_limit = 0; /* play until START+SELECT (render_gl.c) */
#else
static unsigned long long frame_limit = 300;
#endif

/* -- textures --------------------------------------------------------------------- */
RenderTexture textures[RENDER_MAX_TEXTURES];
static unsigned texture_count = 1; /* index 0 = none */
static unsigned format_bpp(uint32_t format);
uint64_t texture_live, texture_peak; /* bytes of texture storage */
void texture_census(void) {
    struct { uint32_t w, h, f, n; } kinds[64];
    unsigned nk = 0;
    for (unsigned i = 1; i < RENDER_MAX_TEXTURES; i++) {
        RenderTexture *t = &textures[i];
        if (!t->data) continue;
        unsigned k = 0;
        while (k < nk && !(kinds[k].w == t->width && kinds[k].h == t->height && kinds[k].f == t->format)) k++;
        if (k == nk && nk < 64) { kinds[nk].w = t->width; kinds[nk].h = t->height; kinds[nk].f = t->format; kinds[nk++].n = 0; }
        if (k < nk) kinds[k].n++;
    }
    /* how much would an 8-bit palette (lossless when <= 256 distinct colours) save? */
    uint64_t total = 0, paletted = 0, drawn = 0, compressed = 0, us_z1 = 0;
    static uint32_t seen[512];
    static uint8_t used[512];
    for (unsigned i = 1; i < RENDER_MAX_TEXTURES; i++) {
        RenderTexture *t = &textures[i];
        if (!t->data || t->format != 21) continue; /* PC layout: raw pixels */
        uint64_t bytes = (uint64_t)t->pitch * t->height;
        total += bytes;
        if (t->drawn) drawn += bytes;
#ifndef T123_PACKED_TEXTURES
        uLongf zlen = compressBound((uLong)bytes);
        static uint8_t *zbuf; static uLongf zcap;
        if (zcap < zlen) { free(zbuf); zbuf = malloc(zlen); zcap = zlen; }
        uint64_t t0 = platform_ticks_us();
        compress2(zbuf, &zlen, t->data, (uLong)bytes, 1);
        us_z1 += platform_ticks_us() - t0;
        compressed += zlen;

#endif
        unsigned n = 0, ok = 1;
        memset(used, 0, sizeof used);
        const uint32_t *p = (const uint32_t *)t->data;
        for (uint64_t j = 0; ok && j < bytes / 4; j++) {
            uint32_t v = p[j], h = (v * 2654435761u) >> 23;
            while (used[h] && seen[h] != v) h = (h + 1) & 511;
            if (!used[h]) { used[h] = 1; seen[h] = v; if (++n > 256) ok = 0; }
        }
        if (ok) paletted += bytes;
    }
    trace("# ARGB textures drawn at least once: %llu KiB; zlib level 1 of all: %llu KiB in %llu ms",
          (unsigned long long)(drawn >> 10), (unsigned long long)(compressed >> 10), (unsigned long long)(us_z1 / 1000));
    trace("# ARGB textures: %llu KiB, of which %llu KiB have <= 256 colours (P8 would need %llu KiB)",
          (unsigned long long)(total >> 10), (unsigned long long)(paletted >> 10),
          (unsigned long long)((total - paletted + paletted / 4) >> 10));
    for (unsigned k = 0; k < nk; k++)
        trace("# live textures: %4u x %ux%u format %u = %u KiB", kinds[k].n, kinds[k].w, kinds[k].h, kinds[k].f,
              kinds[k].n * kinds[k].w * kinds[k].h * format_bpp(kinds[k].f) >> 10);
}
static unsigned free_texture;      /* released slots, linked through next_free */
/* textures are created and released from the loading thread and the main thread */
static pthread_mutex_t texture_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned format_bpp(uint32_t format) {
    switch (format) {
    case 21: case 22: return 4;          /* A8R8G8B8, X8R8G8B8 */
    case 23: case 24: case 25: case 26:  /* R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4 */
        return 2;
    case 28: case 50: return 1;          /* A8, L8 */
    default: return 4;
    }
}

static uint32_t texture_new_locked(uint32_t w, uint32_t h, uint32_t format) {
    unsigned i = free_texture;
    if (i)
        free_texture = textures[i].next_free;
    else if (texture_count == RENDER_MAX_TEXTURES)
        return 0;
    else
        i = texture_count++;
    RenderTexture *t = &textures[i];
    memset(t, 0, sizeof *t);
    t->width = w;
    t->height = h;
    t->format = format;
    t->pitch = w * format_bpp(format);
#ifdef T123_PACKED_TEXTURES
    t->data = NULL; /* no packed data yet = all zero */
    if (0) {
#else
    t->data = platform_texture_alloc((size_t)h * t->pitch);
    if (!t->data) { /* report instead of crashing on the first LockRect */
#endif
        trace("# out of texture memory: %ux%u format %u (%u bytes)", w, h, format, h * t->pitch);
        t->next_free = free_texture;
        free_texture = i;
        return 0;
    }
#ifndef T123_PACKED_TEXTURES
    t->stored = (uint64_t)h * t->pitch;
    texture_live += (uint64_t)h * t->pitch;
    if (texture_live > texture_peak) texture_peak = texture_live;
#endif
    t->dirty = 1;
    return com_new(&texture_class, i, 0);
}
static uint32_t texture_new(uint32_t w, uint32_t h, uint32_t format) {
    pthread_mutex_lock(&texture_lock);
    uint32_t o = texture_new_locked(w, h, format);
    pthread_mutex_unlock(&texture_lock);
    return o;
}

/* Texture contents. On the Vita the game keeps ~155 MiB of 32-bit textures alive but
   draws only ~12 MiB of them in the first minute (measured on PC); zlib level 1
   packs all of them in ~16 MiB, so the Vita keeps them deflated and inflates on use. */
#ifdef T123_PACKED_TEXTURES
/* Deflated into 64 KiB pieces, then copied into one block of the exact size. A block
   for the worst case (the raw size, up to 4 MiB) could not be found once texture
   storage was fragmented ("cannot store texture" in long sessions on the Vita), while
   the result is typically a tenth of it.
   Layout: a PackHeader, then raw deflate data (no zlib wrapper) ended with Z_FULL_FLUSH
   every `rows` rows (~64 KiB), so every segment can be inflated on its own: uploading a
   large texture spreads its segments over worker threads (render_texture_rows). On the
   Vita inflating was 71% of the upload time and a 256x256-or-larger texture took ~27 ms,
   more than a frame. A sequential inflate reads straight through the flush points. */
enum { PIECE = 64 * 1024, MAX_PIECES = 80, SEGMENT_BYTES = 64 * 1024, PACK_MAGIC = 0x47455331 /* "1SEG" */ };
typedef struct { uint32_t magic, segments, rows, offset[]; } PackHeader; /* offset[segments + 1], into the data after the header */
static uint32_t pack_header_bytes(uint32_t segments) { return (uint32_t)sizeof(PackHeader) + 4 * (segments + 1); }

static uint8_t *deflate_texture(const uint8_t *src, uint32_t pitch, uint32_t height, uLongf *zlen) {
    uint32_t rows = SEGMENT_BYTES / pitch;
    rows = rows < 2 ? 2 : rows & ~1u; /* even: half-resolution uploads average row pairs */
    uint32_t segments = (height + rows - 1) / rows;
    uint32_t *offset = malloc(4 * (segments + 1));
    uint8_t *pieces[MAX_PIECES];
    unsigned count = 0;
    z_stream z;
    memset(&z, 0, sizeof z);
    *zlen = 0;
    if (!offset || deflateInit2(&z, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(offset);
        return NULL;
    }
    int r = Z_OK, ok = 1;
    for (uint32_t seg = 0; seg < segments && ok; seg++) {
        uint32_t y = seg * rows, n = height - y < rows ? height - y : rows;
        int flush = seg + 1 == segments ? Z_FINISH : Z_FULL_FLUSH;
        offset[seg] = (uint32_t)z.total_out;
        z.next_in = (Bytef *)src + (size_t)y * pitch;
        z.avail_in = (uInt)((size_t)n * pitch);
        for (;;) {
            if (!z.avail_out) {
                if (count == MAX_PIECES || !(pieces[count] = malloc(PIECE))) { ok = 0; break; }
                z.next_out = pieces[count++];
                z.avail_out = PIECE;
            }
            r = deflate(&z, flush);
            if (r == Z_STREAM_END || (flush == Z_FULL_FLUSH && !z.avail_in && z.avail_out)) break;
            if (r != Z_OK && r != Z_BUF_ERROR) { ok = 0; break; }
        }
    }
    offset[segments] = (uint32_t)z.total_out;
    uint32_t head = pack_header_bytes(segments);
    uint8_t *packed = NULL;
    if (ok && r == Z_STREAM_END && (packed = platform_texture_alloc(head + z.total_out))) {
        PackHeader *h = (PackHeader *)packed;
        h->magic = PACK_MAGIC;
        h->segments = segments;
        h->rows = rows;
        memcpy(h->offset, offset, 4 * (segments + 1));
        for (unsigned k = 0; k < count; k++) {
            uLong at = (uLong)k * PIECE, n = z.total_out - at < PIECE ? z.total_out - at : PIECE;
            memcpy(packed + head + at, pieces[k], n);
        }
    }
    *zlen = head + z.total_out;
    deflateEnd(&z);
    for (unsigned k = 0; k < count; k++)
        free(pieces[k]);
    free(offset);
    return packed;
}
/* Inflates `len` bytes of raw deflate data into exactly `out` bytes; 0 on success. */
static int inflate_raw(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t out) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return -1;
    z.next_in = (Bytef *)src;
    z.avail_in = len;
    z.next_out = dst;
    z.avail_out = out;
    int r = Z_OK;
    while (z.avail_out && r == Z_OK) r = inflate(&z, Z_SYNC_FLUSH);
    inflateEnd(&z);
    return z.avail_out ? -1 : 0;
}
/* Under texture_lock: replace the stored pixels of t. */
static void install_packed(RenderTexture *t, uint8_t *packed, uLongf zlen, uint32_t w, uint32_t h) {
    if (!packed)
        trace("# cannot store texture %ux%u (%u packed bytes)", w, h, (unsigned)zlen);
    texture_live -= t->stored;
    platform_texture_free(t->data);
    t->data = packed;
    t->stored = packed ? zlen : 0;
    texture_live += t->stored;
    if (texture_live > texture_peak) texture_peak = texture_live;
}

/* Compressing on the game's thread cost 25-30 s of the Vita's load (UnlockRect), so
   it runs on two worker threads (the other A9 cores). Until a job is done the texture
   reads from the job's uncompressed copy (t->raw); the copies in flight are bounded
   and past that UnlockRect compresses in place as before. A job only installs its
   result if the texture's generation is unchanged (not rewritten or released). */
enum { WORKERS = 2, MAX_JOBS = 256, PENDING_LIMIT = 8u << 20 };
typedef struct { unsigned index; uint32_t gen; uint8_t *raw; uint32_t bytes; } PackJob;
static PackJob jobs[MAX_JOBS];
static unsigned job_head, job_tail, pending_bytes;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;

static void *pack_worker(void *arg) {
    (void)arg;
    platform_pin_helper("texture packer");
    for (;;) {
        pthread_mutex_lock(&texture_lock);
        while (job_head == job_tail)
            pthread_cond_wait(&job_ready, &texture_lock);
        PackJob job = jobs[job_head++ % MAX_JOBS];
        RenderTexture *t = &textures[job.index];
        int current = t->gen == job.gen;
        uint32_t w = t->width, h = t->height, pitch = t->pitch;
        pthread_mutex_unlock(&texture_lock);
        uLongf zlen = 0;
        uint8_t *packed = current ? deflate_texture(job.raw, pitch, h, &zlen) : NULL;
        pthread_mutex_lock(&texture_lock);
        if (current && t->gen == job.gen) {
            install_packed(t, packed, zlen, w, h);
            t->raw = NULL;
        } else {
            platform_texture_free(packed);
        }
        pending_bytes -= job.bytes;
        pthread_mutex_unlock(&texture_lock);
        platform_texture_free(job.raw);
    }
    return NULL;
}

static void texture_store(RenderTexture *t, const uint8_t *src) {
    static int started;
    uLong bytes = (uLong)t->pitch * t->height;
    uint8_t *raw = NULL;
    pthread_mutex_lock(&texture_lock);
    if (!started) {
        started = 1;
        for (unsigned k = 0; k < WORKERS; k++) {
            pthread_t id;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 128 * 1024);
            pthread_create(&id, &attr, pack_worker, NULL);
            pthread_attr_destroy(&attr);
        }
    }
    static uint32_t generations;
    t->gen = ++generations; /* unique across slot reuse: earlier jobs are now stale */
    static int sync_only = -1; /* T123_PACK_SYNC=1: compress in place (diagnosis) */
    if (sync_only < 0) sync_only = getenv("T123_PACK_SYNC") != NULL;
    if (!sync_only && pending_bytes + bytes <= PENDING_LIMIT && job_tail - job_head < MAX_JOBS && (raw = platform_texture_alloc(bytes))) {
        memcpy(raw, src, bytes);
        jobs[job_tail++ % MAX_JOBS] = (PackJob){(unsigned)(t - textures), t->gen, raw, (uint32_t)bytes};
        pending_bytes += bytes;
        t->raw = raw;
        pthread_cond_signal(&job_ready);
        pthread_mutex_unlock(&texture_lock);
        return;
    }
    t->raw = NULL;
    uint32_t gen = t->gen;
    pthread_mutex_unlock(&texture_lock);
    uLongf zlen = 0;
    uint8_t *packed = deflate_texture(src, t->pitch, t->height, &zlen);
    pthread_mutex_lock(&texture_lock);
    if (t->gen == gen)
        install_packed(t, packed, zlen, t->width, t->height);
    else
        platform_texture_free(packed);
    pthread_mutex_unlock(&texture_lock);
}
/* Under texture_lock. */
static void texture_fetch(RenderTexture *t, uint8_t *dst) {
    uLongf bytes = (uLongf)t->pitch * t->height;
    if (t->raw)
        memcpy(dst, t->raw, bytes); /* compression still pending: the job's copy is current */
    else if (!t->data || ((PackHeader *)t->data)->magic != PACK_MAGIC ||
             inflate_raw(t->data + pack_header_bytes(((PackHeader *)t->data)->segments),
                         (uint32_t)(t->stored - pack_header_bytes(((PackHeader *)t->data)->segments)), dst, (uint32_t)bytes))
        memset(dst, 0, (size_t)t->pitch * t->height);
}
#else
static void texture_store(RenderTexture *t, const uint8_t *src) { memcpy(t->data, src, (size_t)t->pitch * t->height); }
static void texture_fetch(RenderTexture *t, uint8_t *dst) { memcpy(dst, t->data, (size_t)t->pitch * t->height); }
#endif

void render_texture_pixels(RenderTexture *t, uint8_t *out) {
    pthread_mutex_lock(&texture_lock);
    texture_fetch(t, out);
    pthread_mutex_unlock(&texture_lock);
}

unsigned long long texture_parallel_uploads; /* uploads inflated by the segment workers (render.h) */
#ifdef T123_PACKED_TEXTURES
/* Parallel inflate of a segmented texture: two worker threads inflate the next segments into
   a ring of buffers while the caller converts and uploads the ones done, in order. Used by one
   upload at a time (render_texture_rows runs under the texture lock). */
enum { RING = 3, SEG_WORKERS = 2, RING_BYTES = SEGMENT_BYTES }; /* a segment is at most 64 KiB (rows = 64 KiB / pitch) */
typedef struct { const uint8_t *src; uint32_t len; uint8_t *dst; uint32_t out; int state; /* 0 free 1 queued 2 taken 3 done */ } SegJob;
static SegJob seg_jobs[RING];
static uint8_t seg_ring[RING][RING_BYTES];
static pthread_mutex_t seg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seg_queued = PTHREAD_COND_INITIALIZER, seg_done = PTHREAD_COND_INITIALIZER;

static void *seg_worker(void *arg) {
    (void)arg;
    platform_pin_helper("texture inflater");
    pthread_mutex_lock(&seg_lock);
    for (;;) {
        SegJob *j = NULL;
        for (unsigned k = 0; k < RING && !j; k++)
            if (seg_jobs[k].state == 1) j = &seg_jobs[k];
        if (!j) { pthread_cond_wait(&seg_queued, &seg_lock); continue; }
        j->state = 2;
        pthread_mutex_unlock(&seg_lock);
        if (inflate_raw(j->src, j->len, j->dst, j->out)) memset(j->dst, 0, j->out);
        pthread_mutex_lock(&seg_lock);
        j->state = 3;
        pthread_cond_broadcast(&seg_done);
    }
    return NULL;
}

/* 1 if t was sent through sink segment by segment */
static int rows_parallel(RenderTexture *t, size_t size, void (*sink)(void *, const uint8_t *, uint32_t, uint32_t), void *ctx) {
    const PackHeader *h = (const PackHeader *)t->data;
    if (t->raw || !t->data || h->magic != PACK_MAGIC || h->segments < 2) return 0;
    if ((size_t)h->rows * t->pitch > size || (size_t)h->rows * t->pitch > RING_BYTES) return 0;
    static int workers; /* 0 not started, 1 running, -1 failed */
    if (!workers) {
        workers = 1;
        for (unsigned k = 0; k < SEG_WORKERS; k++) {
            pthread_t id;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 64 * 1024);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&id, &attr, seg_worker, NULL)) workers = -1;
            pthread_attr_destroy(&attr);
        }
    }
    if (workers < 0) return 0;
    const uint8_t *data = t->data + pack_header_bytes(h->segments);
    uint32_t segs = h->segments, rows = h->rows, next = 0;
#define SEG_POST(k) do { SegJob *j_ = &seg_jobs[(k) % RING]; uint32_t y_ = (k) * rows; \
        j_->src = data + h->offset[k]; j_->len = h->offset[(k) + 1] - h->offset[k]; j_->dst = seg_ring[(k) % RING]; \
        j_->out = (t->height - y_ < rows ? t->height - y_ : rows) * t->pitch; j_->state = 1; } while (0)
    pthread_mutex_lock(&seg_lock);
    for (; next < segs && next < RING; next++) SEG_POST(next);
    pthread_cond_broadcast(&seg_queued);
    for (uint32_t k = 0; k < segs; k++) {
        SegJob *j = &seg_jobs[k % RING];
        while (j->state != 3) pthread_cond_wait(&seg_done, &seg_lock);
        pthread_mutex_unlock(&seg_lock);
        uint32_t y = k * rows;
        sink(ctx, j->dst, y, t->height - y < rows ? t->height - y : rows);
        pthread_mutex_lock(&seg_lock);
        j->state = 0;
        if (next < segs) { SEG_POST(next); next++; pthread_cond_broadcast(&seg_queued); }
    }
    pthread_mutex_unlock(&seg_lock);
#undef SEG_POST
    texture_parallel_uploads++;
    return 1;
}
#endif

/* The stored pixels in bands of whole rows through `buf` (any size of at least one
   row): the upload no longer needs the whole texture in one piece, which for a
   1024x1024 texture (4 MiB) failed on the Vita once memory was fragmented. */
void render_texture_rows(RenderTexture *t, uint8_t *buf, size_t size,
                         void (*sink)(void *ctx, const uint8_t *rows, uint32_t y, uint32_t n), void *ctx) {
    uint32_t pitch = t->pitch, rows_per = (uint32_t)(size / pitch);
    if (!rows_per) return;
    pthread_mutex_lock(&texture_lock);
#ifdef T123_PACKED_TEXTURES
    if (rows_parallel(t, size, sink, ctx)) {
        pthread_mutex_unlock(&texture_lock);
        return;
    }
    z_stream z;
    const PackHeader *h = (const PackHeader *)t->data;
    int inflating = !t->raw && t->data && h->magic == PACK_MAGIC;
    if (inflating) {
        memset(&z, 0, sizeof z);
        z.next_in = t->data + pack_header_bytes(h->segments);
        z.avail_in = (uInt)(t->stored - pack_header_bytes(h->segments));
        inflating = inflateInit2(&z, -MAX_WBITS) == Z_OK;
    }
#endif
    for (uint32_t y = 0; y < t->height; y += rows_per) {
        uint32_t n = t->height - y < rows_per ? t->height - y : rows_per;
        size_t bytes = (size_t)n * pitch;
#ifdef T123_PACKED_TEXTURES
        if (t->raw) {
            memcpy(buf, t->raw + (size_t)y * pitch, bytes);
        } else if (inflating) {
            z.next_out = buf;
            z.avail_out = (uInt)bytes;
            int r = Z_OK;
            while (z.avail_out && r == Z_OK) r = inflate(&z, Z_SYNC_FLUSH);
            if (z.avail_out)
                memset(z.next_out, 0, z.avail_out);
        } else {
            memset(buf, 0, bytes);
        }
#else
        memcpy(buf, t->data + (size_t)y * pitch, bytes);
#endif
        sink(ctx, buf, y, n);
    }
#ifdef T123_PACKED_TEXTURES
    if (inflating)
        inflateEnd(&z);
#endif
    pthread_mutex_unlock(&texture_lock);
}

/* The game streams textures while loading: a released texture gives back its pixels
   and its slot, or the guest heap runs out during boot. */
static uint32_t texture_release(uint32_t o) {
    uint32_t n = rd32(o + 4);
    if (n) wr32(o + 4, --n);
    if (!n) {
        unsigned i = com_index(o);
        RenderTexture *t = &textures[i];
        /* GL lock before the texture lock, never inside it: drawing holds the GL lock
           and then reads the pixels under the texture lock (a deadlock seen on PC). */
        render_texture_free(t);
        pthread_mutex_lock(&texture_lock);
        heap_free(t->pixels);
        texture_live -= t->stored;
        platform_texture_free(t->data);
        memset(t, 0, sizeof *t);
        t->next_free = free_texture;
        free_texture = i;
        pthread_mutex_unlock(&texture_lock);
        heap_free(o);
    }
    return n;
}
API(Texture_Release) { ret_stdcall(c, texture_release(ARG(c, 0)), 1); }
API(Surface_Release) {
    uint32_t o = ARG(c, 0), n = rd32(o + 4);
    if (n) wr32(o + 4, --n);
    if (!n) { texture_release(rd32(o + 12)); heap_free(o); }
    ret_stdcall(c, n, 1);
}

/* -- IDirect3D9 ------------------------------------------------------------------------- */
API(GetAdapterDisplayMode) {
    uint32_t m = ARG(c, 2);
    wr32(m, 1280); wr32(m + 4, 720); wr32(m + 8, 60); wr32(m + 12, 22 /* X8R8G8B8 */);
    ret_stdcall(c, S_OK, 3);
}

static void fill_caps(uint32_t p) {
    memset(G(p), 0, 304);
    wr32(p + 0, 1);                                   /* DeviceType HAL */
    for (uint32_t off = 8; off <= 84; off += 4)       /* capability bit fields: all */
        wr32(p + off, 0xFFFFFFFFu);
    /* TextureCaps without SQUAREONLY (0x20) and POW2 (0x2): with them the game made a
       32x128 portrait 128x128 and spaced the character select strip by the texture
       width, so only every fourth portrait was on screen */
    wr32(p + 60, 0xFFFFFFFFu & ~0x22u);
    wr32(p + 88, 4096); wr32(p + 92, 4096);           /* MaxTextureWidth/Height */
    wr32(p + 100, 8192); wr32(p + 104, 8192);
    wr32(p + 108, 16);
    for (uint32_t off = 136; off <= 144; off += 4) wr32(p + off, 0xFFFFFFFFu);
    wr32(p + 148, 8); wr32(p + 152, 8);               /* texture stages */
    wr32(p + 156, 0xFFFFFFFFu);
    wr32(p + 180, 0xFFFFF); wr32(p + 184, 0xFFFFFF); wr32(p + 188, 16); wr32(p + 192, 255);
    wr32(p + 196, 0xFFFE0200u);                       /* vs_2_0 */
    wr32(p + 200, 256);
    wr32(p + 204, 0xFFFF0200u);                       /* ps_2_0 */
    float max_value = 65504.0f;
    memcpy(G(p + 208), &max_value, 4);
}

API(D3D9_GetDeviceCaps) { fill_caps(ARG(c, 3)); ret_stdcall(c, S_OK, 4); }

API(CreateDevice) {
    uint32_t params = ARG(c, 5), out = ARG(c, 6);
    uint32_t w = rd32(params), h = rd32(params + 4);
    trace("IDirect3D9::CreateDevice(type=%u, flags=%08x, backbuffer %ux%u, windowed=%u)",
          ARG(c, 2), ARG(c, 4), w, h, rd32(params + 32));
    if (render_init(w ? w : 640, h ? h : 480)) {
        ret_stdcall(c, E_FAIL, 7);
        return;
    }
    device_object = com_new(&device_class, 0, 0);
    swapchain_object = com_new(&swapchain_class, 0, 0);
    wr32(out, device_object);
    ret_stdcall(c, S_OK, 7);
}

/* -- IDirect3DDevice9 ------------------------------------------------------------------ */
API(TestCooperativeLevel) { ret_stdcall(c, S_OK, 1); }
API(Device_GetDeviceCaps) { fill_caps(ARG(c, 1)); ret_stdcall(c, S_OK, 2); }
API(GetSwapChain) {
    wr32(ARG(c, 2), swapchain_object);
    wr32(swapchain_object + 4, rd32(swapchain_object + 4) + 1);
    ret_stdcall(c, S_OK, 3);
}
API(Reset) { trace("IDirect3DDevice9::Reset"); ret_stdcall(c, S_OK, 2); }
API(BeginScene) { ret_stdcall(c, S_OK, 1); }
API(EndScene) { ret_stdcall(c, S_OK, 1); }
API(Clear) {
    render_clear(ARG(c, 3), ARG(c, 4), *(float *)G(c->esp.d + 4 + 4 * 5), ARG(c, 6));
    ret_stdcall(c, S_OK, 7);
}
/* T123_DRAWLOG=n logs presented frame n; T123_DRAWLOG_EVERY=k also every k-th frame */
static int drawlog_active(void) {
    static long long frame = -2, every = -2;
    if (frame == -2) frame = getenv("T123_DRAWLOG") ? atoll(getenv("T123_DRAWLOG")) : -1;
    if (every == -2) every = getenv("T123_DRAWLOG_EVERY") ? atoll(getenv("T123_DRAWLOG_EVERY")) : 0;
    return (long long)d3d_frames == frame || (every > 0 && d3d_frames % every == 0);
}
API(SetRenderState) {
    if (drawlog_active()) trace("#   rs %u = %08x", ARG(c, 1), ARG(c, 2));
    render_state(ARG(c, 1), ARG(c, 2));
    ret_stdcall(c, S_OK, 3);
}
API(GetRenderState) { wr32(ARG(c, 2), render_get_state(ARG(c, 1))); ret_stdcall(c, S_OK, 3); }
static uint32_t drawlog_texture; /* texture bound to stage 0, for T123_DRAWLOG */
API(SetTexture) {
    uint32_t t = ARG(c, 2);
    if (ARG(c, 1) == 0) drawlog_texture = t;
    if (drawlog_active() && ARG(c, 1) != 0)
        trace("#   texture stage %u = %08x (%ux%u fmt %u)", ARG(c, 1), t, t ? textures[com_index(t)].width : 0,
              t ? textures[com_index(t)].height : 0, t ? textures[com_index(t)].format : 0);
    render_texture(ARG(c, 1), t ? &textures[com_index(t)] : NULL);
    ret_stdcall(c, S_OK, 3);
}
API(SetTextureStageState) {
    if (drawlog_active()) trace("#   tss %u %u = %08x", ARG(c, 1), ARG(c, 2), ARG(c, 3));
    if (getenv("T123_DRAWLOG")) { /* each distinct combination once */
        static uint32_t seen[256][3];
        static unsigned seen_n;
        unsigned k = 0;
        while (k < seen_n && !(seen[k][0] == ARG(c, 1) && seen[k][1] == ARG(c, 2) && seen[k][2] == ARG(c, 3))) k++;
        if (k == seen_n && seen_n < 256) {
            seen[seen_n][0] = ARG(c, 1); seen[seen_n][1] = ARG(c, 2); seen[seen_n][2] = ARG(c, 3); seen_n++;
            trace("# tss first use: stage %u type %u = %08x (frame %llu)", ARG(c, 1), ARG(c, 2), ARG(c, 3), d3d_frames);
        }
    }
    render_stage_state(ARG(c, 1), ARG(c, 2), ARG(c, 3)); ret_stdcall(c, S_OK, 4); }
API(SetSamplerState) { render_sampler_state(ARG(c, 1), ARG(c, 2), ARG(c, 3)); ret_stdcall(c, S_OK, 4); }
API(SetFVF) {
    if (drawlog_active()) trace("#   fvf %08x", ARG(c, 1));
    render_fvf(ARG(c, 1));
    ret_stdcall(c, S_OK, 2);
}
API(DrawPrimitiveUP) {
    d3d_draws++;
    /* T123_DRAWLOG=n: every draw of presented frame n, vertices as x,y u,v colour
       (assumes the XYZRHW|DIFFUSE|TEX1 layout for 28-byte strides) */
    if (drawlog_active()) {
        uint32_t type = ARG(c, 1), count = ARG(c, 2), data = ARG(c, 3), stride = ARG(c, 4);
        char line[600];
        const RenderTexture *bound = drawlog_texture ? &textures[com_index(drawlog_texture)] : NULL;
        int n = snprintf(line, sizeof line, "# draw type %u count %u stride %u tex %ux%u fmt %u:", type, count,
                         stride, bound ? bound->width : 0, bound ? bound->height : 0, bound ? bound->format : 0);
        unsigned verts = type == 5 || type == 6 ? count + 2 : type == 4 ? count * 3 : count;
        for (unsigned v = 0; v < verts && v < 6 && n < (int)sizeof line - 80; v++) {
            uint32_t p = data + v * stride;
            n += snprintf(line + n, sizeof line - n, " (%.0f,%.0f", *(float *)G(p), *(float *)G(p + 4));
            if (stride == 28)
                n += snprintf(line + n, sizeof line - n, " z%.3f w%.3f %.3f,%.3f %08x", *(float *)G(p + 8),
                              *(float *)G(p + 12),*(float *)G(p + 20), *(float *)G(p + 24), rd32(p + 16));
            n += snprintf(line + n, sizeof line - n, ")");
        }
        trace("%s [frame %llu]", line, (unsigned long long)d3d_frames);
        if (bound && bound->format == 25 && bound->height <= 512) {
            static uint8_t px[512 * 1024 * 4];
            if ((size_t)bound->pitch * bound->height <= sizeof px) {
                render_texture_pixels((RenderTexture *)bound, px);
                unsigned opaque = 0, cols = 0;
                for (uint32_t x = 0; x < bound->width; x++) {
                    unsigned here = 0;
                    for (uint32_t y = 0; y < bound->height; y++)
                        here += px[(size_t)y * bound->pitch + x * 2 + 1] >> 7;
                    opaque += here;
                    cols += here != 0;
                }
                static const uint32_t rs[] = {7, 14, 15, 19, 20, 22, 23, 24, 25, 27, 137, 168, 171, 174, 206, 207, 208, 209};
                char st[400];
                int k = 0;
                for (unsigned i = 0; i < sizeof rs / sizeof *rs; i++)
                    k += snprintf(st + k, sizeof st - k, " %u=%x", rs[i], render_get_state(rs[i]));
                trace("#   states%s", st);
                trace("#   texture pitch %u: %u opaque pixels in %u of %u columns, dirty %d", bound->pitch, opaque,
                      cols, bound->width, bound->dirty);            }
        }
    }
    render_draw_up(ARG(c, 1), ARG(c, 2), ARG(c, 3), ARG(c, 4));
    ret_stdcall(c, S_OK, 5);
}

/* -- IDirect3DSwapChain9 ------------------------------------------------------------------ */
API(Present) {
    render_present();
    d3d_frames++;
    if (d3d_frames == 1 || d3d_frames % 60 == 0)
        trace("# frame %llu: %llu draws so far (%llu ms)", d3d_frames, d3d_draws, (unsigned long long)(platform_ticks_us() / 1000));
    /* loading screens draw one quad per frame; the title draws ~27 */
    static unsigned long long last_draws, busy_seen;
    if (!busy_seen && d3d_frames > 60 && d3d_draws - last_draws >= 10) {
        busy_seen = d3d_frames;
        trace("# first busy frame (>= 10 draws, title reached): frame %llu at %llu ms", d3d_frames, (unsigned long long)(platform_ticks_us() / 1000));
    }
    last_draws = d3d_draws;
    if (frame_limit && d3d_frames >= frame_limit) {
        char why[96];
        snprintf(why, sizeof why, "frame limit reached (%llu frames presented)", d3d_frames);
        host_exit(c, 0, why);
    }
    ret_stdcall(c, render_quit_requested() ? 0x88760868u /* D3DERR_DEVICELOST */ : S_OK, 6);
}

/* -- textures and surfaces -------------------------------------------------------------- */
API(GetSurfaceLevel) {
    uint32_t t = ARG(c, 0);
    uint32_t s = com_new(&surface_class, com_index(t), 4);
    wr32(s + 12, t);              /* the container stays alive while the level is held */
    wr32(t + 4, rd32(t + 4) + 1);
    wr32(ARG(c, 2), s);
    ret_stdcall(c, S_OK, 3);
}
static void write_desc(uint32_t d, RenderTexture *t) {
    wr32(d, t->format); wr32(d + 4, 3 /* D3DRTYPE_TEXTURE */); wr32(d + 8, 0); wr32(d + 12, 1 /* MANAGED */);
    wr32(d + 16, 0); wr32(d + 20, 0); wr32(d + 24, t->width); wr32(d + 28, t->height);
}
API(Surface_GetDesc) { write_desc(ARG(c, 1), &textures[com_index(ARG(c, 0))]); ret_stdcall(c, S_OK, 2); }
API(Texture_GetLevelDesc) { write_desc(ARG(c, 2), &textures[com_index(ARG(c, 0))]); ret_stdcall(c, S_OK, 3); }
API(Texture_LockRect) {
    RenderTexture *t = &textures[com_index(ARG(c, 0))];
    if (!t->pixels) { /* guest staging copy for the duration of the lock */
        t->pixels = heap_alloc(t->pitch * t->height, 0);
        render_texture_pixels(t, G(t->pixels));
    }
    uint32_t lock = ARG(c, 2), rect = ARG(c, 3), bits = t->pixels;
    if (rect)
        bits += rd32(rect + 4) * t->pitch + rd32(rect) * format_bpp(t->format);
    wr32(lock, t->pitch);
    wr32(lock + 4, bits);
    ret_stdcall(c, S_OK, 5);
}
API(Texture_UnlockRect) {
    RenderTexture *t = &textures[com_index(ARG(c, 0))];
    if (t->pixels) {
        texture_store(t, G(t->pixels));
        heap_free(t->pixels);
        t->pixels = 0;
    }
    t->dirty = 1;
    ret_stdcall(c, S_OK, 2);
}

/* -- ID3DXEffect: parameters are recorded, passes draw with the fixed pipeline ---------- */
API(Effect_Begin) {
    if (drawlog_active()) trace("#   effect %08x Begin", ARG(c, 0));
    if (ARG(c, 1)) wr32(ARG(c, 1), 1);
    ret_stdcall(c, S_OK, 3);
}
static void effect_log(Cpu *c, const char *what, unsigned args) {
    if (drawlog_active()) trace("#   effect %08x %s(%08x, %08x)", ARG(c, 0), what, ARG(c, 1), ARG(c, 2));
    ret_stdcall(c, S_OK, args);
}
API(Effect_SetTexture) { effect_log(c, "SetTexture", 3); }
API(Effect_BeginPass) { effect_log(c, "BeginPass", 2); }
API(Effect_EndPass) { effect_log(c, "EndPass", 1); }
API(Effect_SetTechnique) { effect_log(c, "SetTechnique", 2); }

/* -- D3DX imports ------------------------------------------------------------------------ */
API(D3DXCreateTexture) {
    uint32_t w = ARG(c, 1), h = ARG(c, 2), format = ARG(c, 5);
    uint32_t t = texture_new(w, h, format);
    wr32(ARG(c, 7), t);
    ret_stdcall(c, t ? S_OK : E_FAIL, 8);
}
API(D3DXCreateEffect) {
    trace("D3DXCreateEffect(size=%u)", ARG(c, 2));    wr32(ARG(c, 7), com_new(&effect_class, 0, 0));
    if (ARG(c, 8)) wr32(ARG(c, 8), 0);
    ret_stdcall(c, S_OK, 9);
}
static float rdf(uint32_t a) { float f; memcpy(&f, G(a), 4); return f; }
static void wrf(uint32_t a, float f) { memcpy(G(a), &f, 4); }
API(D3DXVec2Normalize) {
    uint32_t out = ARG(c, 0), v = ARG(c, 1);
    float x = rdf(v), y = rdf(v + 4), l = sqrtf(x * x + y * y);
    wrf(out, l ? x / l : 0); wrf(out + 4, l ? y / l : 0);
    ret_stdcall(c, out, 2);
}
API(D3DXVec2Hermite) {
    uint32_t out = ARG(c, 0), v1 = ARG(c, 1), t1 = ARG(c, 2), v2 = ARG(c, 3), t2 = ARG(c, 4);
    float s = rdf(c->esp.d + 4 + 4 * 5), s2 = s * s, s3 = s2 * s;
    float h1 = 2 * s3 - 3 * s2 + 1, h2 = -2 * s3 + 3 * s2, h3 = s3 - 2 * s2 + s, h4 = s3 - s2;
    for (int k = 0; k < 2; k++)
        wrf(out + 4 * k, h1 * rdf(v1 + 4 * k) + h2 * rdf(v2 + 4 * k) + h3 * rdf(t1 + 4 * k) + h4 * rdf(t2 + 4 * k));
    ret_stdcall(c, out, 6);
}
API(Direct3DCreate9) {
    trace("Direct3DCreate9(SDK %u)", ARG(c, 0));
    ret_stdcall(c, com_new(&d3d9_class, 0, 0), 1);
}

static void bind_all(void) {
    com_bind(&d3d9_class, "GetAdapterDisplayMode", GetAdapterDisplayMode);
    com_bind(&d3d9_class, "GetDeviceCaps", D3D9_GetDeviceCaps);
    com_bind(&d3d9_class, "CreateDevice", CreateDevice);
    com_bind(&device_class, "TestCooperativeLevel", TestCooperativeLevel);
    com_bind(&device_class, "GetDeviceCaps", Device_GetDeviceCaps);
    com_bind(&device_class, "GetSwapChain", GetSwapChain);
    com_bind(&device_class, "Reset", Reset);
    com_bind(&device_class, "BeginScene", BeginScene);
    com_bind(&device_class, "EndScene", EndScene);
    com_bind(&device_class, "Clear", Clear);
    com_bind(&device_class, "SetRenderState", SetRenderState);
    com_bind(&device_class, "GetRenderState", GetRenderState);
    com_bind(&device_class, "SetTexture", SetTexture);
    com_bind(&device_class, "SetTextureStageState", SetTextureStageState);
    com_bind(&device_class, "SetSamplerState", SetSamplerState);
    com_bind(&device_class, "SetFVF", SetFVF);
    com_bind(&device_class, "DrawPrimitiveUP", DrawPrimitiveUP);
    com_bind(&swapchain_class, "Present", Present);
    com_bind(&texture_class, "Release", Texture_Release);
    com_bind(&texture_class, "GetSurfaceLevel", GetSurfaceLevel);
    com_bind(&texture_class, "GetLevelDesc", Texture_GetLevelDesc);
    com_bind(&texture_class, "LockRect", Texture_LockRect);
    com_bind(&texture_class, "UnlockRect", Texture_UnlockRect);
    com_bind(&surface_class, "GetDesc", Surface_GetDesc);
    com_bind(&surface_class, "Release", Surface_Release);
    com_bind(&effect_class, "Begin", Effect_Begin);
    com_bind(&effect_class, "SetTexture", Effect_SetTexture);
    com_bind(&effect_class, "BeginPass", Effect_BeginPass);
    com_bind(&effect_class, "EndPass", Effect_EndPass);
    com_bind(&effect_class, "SetTechnique", Effect_SetTechnique);
    const char *effect_ok[] = {"SetFloat", "SetVector", "SetInt", "SetBool", "SetMatrix",
                               "CommitChanges", "End", "OnLostDevice", "OnResetDevice"};
    for (unsigned i = 0; i < sizeof effect_ok / sizeof *effect_ok; i++)
        com_bind_ok(&effect_class, effect_ok[i]);
}

void d3d9_init(void) {
    static int done;
    if (!done) {
        done = 1;
        bind_all();
    }
}

void d3d9_set_frame_limit(unsigned long long frames) { frame_limit = frames; }

#define E(name) {#name, name}
const ApiEntry win32_d3d9[] = {
    E(Direct3DCreate9), E(D3DXCreateTexture), E(D3DXCreateEffect), E(D3DXVec2Normalize), E(D3DXVec2Hermite),
    {NULL, NULL},
};
