/* Native vorbisfile in place of the translated one. The game calls six vorbisfile
 * functions (cdecl, found from the call sites in 0x418bb0, 0x418d10, 0x4032a0 and
 * 0x402b60); the translated decoder spent much of a fight's CPU in x87 helpers.
 *
 * 0x8107a0  ov_open_callbacks(datasource, vf, initial, ibytes, callbacks by value)
 * 0x8106a0  ov_clear(vf)
 * 0x8125c0  ov_info(vf, link)            returns a vorbis_info * the game reads
 * 0x812600  ov_read(vf, buffer, length, bigendianp, word, sgned, bitstream *)
 * 0x8124d0  ov_time_tell(vf)             double in st(0)
 * 0x8123d0  ov_time_seek(vf, double)
 *
 * The guest OggVorbis_File is left untouched; a host decoder is kept per guest vf
 * address. The callbacks stay the game's own (it reads from its archives), called
 * through guest_invoke with a guest-memory bounce buffer. */
#include "win32.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>

#define MAX_STREAMS 16
#define BOUNCE 0x8000u

typedef struct {
    uint32_t vf;                 /* guest OggVorbis_File address, 0 = free slot */
    uint32_t datasource, read, seek, close, tell;
    uint32_t bounce;             /* guest buffer for read callbacks */
    uint32_t info;               /* guest vorbis_info copy */
    Cpu *cpu;                    /* caller of the current ov_* call */
    uint64_t silent_bytes;       /* nosound.txt: PCM "delivered" so far (silence) */
    OggVorbis_File file;
} Stream;
static int silent_mode;          /* nosound.txt: the headers are parsed but nothing is decoded */

static Stream streams[MAX_STREAMS];
static uint64_t decode_us, decoded_bytes; /* for vorbis_stats (heartbeat) */
static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;

static Stream *find_stream(uint32_t vf) {
    pthread_mutex_lock(&streams_lock);
    Stream *s = NULL;
    for (unsigned i = 0; i < MAX_STREAMS && !s; i++)
        if (streams[i].vf == vf) s = &streams[i];
    pthread_mutex_unlock(&streams_lock);
    return s;
}

static uint32_t arg(Cpu *c, unsigned n) { return rd32(c->esp.d + 4 + 4 * n); }

static size_t cb_read(void *ptr, size_t size, size_t nmemb, void *source) {
    Stream *s = source;
    size_t want = size * nmemb, done = 0;
    if (!size) return 0;
    while (done < want) {
        uint32_t chunk = want - done < BOUNCE ? (uint32_t)(want - done) : BOUNCE;
        uint32_t argv[4] = {s->bounce, 1, chunk, s->datasource};
        uint32_t got = guest_invoke(s->cpu, s->read, 4, argv);
        if (got > chunk) got = chunk;
        memcpy((uint8_t *)ptr + done, G(s->bounce), got);
        done += got;
        if (got < chunk) break;
    }
    return done / size;
}

static int cb_seek(void *source, ogg_int64_t offset, int whence) {
    Stream *s = source;
    uint64_t o = (uint64_t)offset;
    uint32_t argv[4] = {s->datasource, (uint32_t)o, (uint32_t)(o >> 32), (uint32_t)whence};
    return (int)guest_invoke(s->cpu, s->seek, 4, argv);
}

static int cb_close(void *source) {
    Stream *s = source;
    return s->close ? (int)guest_invoke(s->cpu, s->close, 1, &s->datasource) : 0;
}

static long cb_tell(void *source) {
    Stream *s = source;
    return (long)(int32_t)guest_invoke(s->cpu, s->tell, 1, &s->datasource);
}

static void ret_eax(Cpu *c, uint32_t v) { c->eax.d = v; c->esp.d += 4; }

/* The decoder runs in the host's default floating-point mode, whatever the guest thread left:
   - Rounding: the runtime leaves each thread's host FPU in the guest's last x87 rounding mode
     (guest.c, WITH_RC), and the game's CRT switches to directed modes (truncation for its
     float->int). libvorbis in such a mode decodes its short blocks (the transients) to
     noise and its long blocks off by one: heard on the Vita as loud noise in the music from
     the title screen on. Measured on PC (title music, 60 s): with round-to-nearest around
     open, read and seek the mix matches a plain libvorbis decode (before: 6% of it noise,
     up to +44 dB above 10 kHz).
   - Denormals (Vita): the tail of a track or a fade produces denormal values (a core dump
     showed the music thread dying on a VFP `vmul.f32` inside ov_read's float-to-16-bit loop);
     with flush-to-zero they read as zero (inaudible) and the unit does not trap on them.
   The guest's mode is restored after each call, so its own arithmetic keeps its behaviour. */
#ifdef __vita__
static inline uint32_t fp_enter(void) {
    uint32_t old;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(old));
    __asm__ volatile("vmsr fpscr, %0" : : "r"((old & ~(3u << 22)) | (1u << 24))); /* RMode nearest, FZ */
    return old;
}
static inline void fp_leave(uint32_t old) { __asm__ volatile("vmsr fpscr, %0" : : "r"(old)); }
#else
#include <fenv.h>
static inline uint32_t fp_enter(void) {
    int old = fegetround();
    if (old != FE_TONEAREST) fesetround(FE_TONEAREST);
    return (uint32_t)old;
}
static inline void fp_leave(uint32_t old) {
    if ((int)old != FE_TONEAREST) fesetround((int)old);
}
#endif

static void native_open(Cpu *c) {
    uint32_t datasource = arg(c, 0), vf = arg(c, 1), initial = arg(c, 2), ibytes = arg(c, 3);
    Stream *s = find_stream(vf);
    if (s) { ov_clear(&s->file); s->vf = 0; }
    pthread_mutex_lock(&streams_lock);
    for (unsigned i = 0; i < MAX_STREAMS && !s; i++)
        if (!streams[i].vf) { s = &streams[i]; s->vf = vf; }
    pthread_mutex_unlock(&streams_lock);
    if (!s) { trace("# vorbis: more than %d streams", MAX_STREAMS); ret_eax(c, (uint32_t)OV_EFAULT); return; }
    s->datasource = datasource;
    s->read = arg(c, 4); s->seek = arg(c, 5); s->close = arg(c, 6); s->tell = arg(c, 7);
    if (!s->bounce) s->bounce = heap_alloc(BOUNCE, 0);
    if (!s->info) s->info = heap_alloc(32, 1);
    s->cpu = c;
    ov_callbacks cb = {cb_read, s->seek ? cb_seek : NULL, cb_close, s->tell ? cb_tell : NULL};
    uint32_t fp = fp_enter();
    int r = ov_open_callbacks(s, &s->file, initial ? (const char *)G(initial) : NULL, (long)ibytes, cb);
    fp_leave(fp);
    if (r < 0) {
        trace("# vorbis: ov_open_callbacks(%08x) failed %d", vf, r);
        s->vf = 0;
    }
    unsigned live = 0;
    for (unsigned i = 0; i < MAX_STREAMS; i++) live += streams[i].vf != 0;
    trace("# vorbis: open %08x -> %d, %u streams open", vf, r, live);
    ret_eax(c, (uint32_t)r);
}

static void native_clear(Cpu *c) {
    Stream *s = find_stream(arg(c, 0));
    if (s) {
        s->cpu = c;
        ov_clear(&s->file);
        pthread_mutex_lock(&streams_lock);
        s->vf = 0;
        pthread_mutex_unlock(&streams_lock);
    }
    ret_eax(c, 0);
}

static void native_info(Cpu *c) {
    Stream *s = find_stream(arg(c, 0));
    vorbis_info *vi = s ? ov_info(&s->file, (int)arg(c, 1)) : NULL;
    if (!vi) { ret_eax(c, 0); return; }
    uint32_t fields[8] = {(uint32_t)vi->version, (uint32_t)vi->channels, (uint32_t)vi->rate,
                          (uint32_t)vi->bitrate_upper, (uint32_t)vi->bitrate_nominal,
                          (uint32_t)vi->bitrate_lower, (uint32_t)vi->bitrate_window, 0};
    memcpy(G(s->info), fields, sizeof fields);
    ret_eax(c, s->info);
}

static void native_read(Cpu *c) {
    Stream *s = find_stream(arg(c, 0));
    if (!s) { ret_eax(c, (uint32_t)OV_EINVAL); return; }
    s->cpu = c;
    if (silent_mode) { /* silence of the size asked for, without decoding */
        uint32_t len = arg(c, 2);
        memset(G(arg(c, 1)), 0, len);
        s->silent_bytes += len;
        if (arg(c, 6)) wr32(arg(c, 6), 0);
        ret_eax(c, len);
        return;
    }
    int bitstream = 0;
    uint64_t t0 = platform_ticks_us();
    uint32_t fp = fp_enter();
    long r = ov_read(&s->file, (char *)G(arg(c, 1)), (int)arg(c, 2), (int)arg(c, 3), (int)arg(c, 4),
                     (int)arg(c, 5), &bitstream);
    fp_leave(fp);
    decode_us += platform_ticks_us() - t0;
    if (r > 0) decoded_bytes += (uint64_t)r;
    if (arg(c, 6)) wr32(arg(c, 6), (uint32_t)bitstream);
    ret_eax(c, (uint32_t)r);
}

static double silent_rate(Stream *s) { /* bytes of 16-bit PCM per second of this stream */
    vorbis_info *vi = ov_info(&s->file, -1);
    return vi ? (double)vi->rate * vi->channels * 2.0 : 44100.0 * 2 * 2;
}

static void native_time_tell(Cpu *c) {
    Stream *s = find_stream(arg(c, 0));
    double t = (double)OV_EINVAL;
    if (s) t = silent_mode ? (double)s->silent_bytes / silent_rate(s) : ov_time_tell(&s->file);
    x87_push(c, t);
    c->esp.d += 4;
}

static void native_time_seek(Cpu *c) {
    Stream *s = find_stream(arg(c, 0));
    if (!s) { ret_eax(c, (uint32_t)OV_EINVAL); return; }
    double t;
    uint64_t bits = rd64(c->esp.d + 8);
    memcpy(&t, &bits, sizeof t);
    s->cpu = c;
    if (silent_mode) { /* the position is only a number */
        double rate = silent_rate(s);
        s->silent_bytes = t > 0 ? (uint64_t)(t * rate) & ~(uint64_t)3 : 0;
        ret_eax(c, 0);
        return;
    }
    uint32_t fp = fp_enter();
    int seek_result = ov_time_seek(&s->file, t);
    fp_leave(fp);
    ret_eax(c, (uint32_t)seek_result);
}

/* Decode time and PCM bytes since the last call. */
void vorbis_stats(uint64_t *us, uint64_t *bytes) {
    *us = decode_us; *bytes = decoded_bytes;
    decode_us = decoded_bytes = 0;
}

void vorbis_init(void) {
    /* Native libvorbis is the default on both platforms. On the Vita guest-vorbis.txt in
       the data directory switches to the game's translated decoder (more CPU); on PC
       T123_GUEST_VORBIS=1 or a guest-vorbis.txt does. */
    if (host_nosound()) { /* no music at all: only the headers are read */
        silent_mode = 1;
        trace("# vorbis: silent (nosound.txt): headers only, no decoding");
        goto register_native;
    }
#ifdef __vita__
    /* native libvorbis is the default again (the music sounded better and costs less CPU);
       guest-vorbis.txt switches to the game's translated decoder */
    FILE *guest = fopen("ux0:data/touhou123vita/guest-vorbis.txt", "rb");
    if (guest) fclose(guest);
    if (guest) {
        trace("# vorbis: translated decoder (guest-vorbis.txt)");
        return;
    }
#else
    const char *off = getenv("T123_GUEST_VORBIS");
    FILE *flag = fopen("guest-vorbis.txt", "rb");
    if (flag) fclose(flag);
    if ((off && *off == '1') || flag) {
        trace("# vorbis: translated decoder (native one switched off)");
        return;
    }
#endif
    trace("# vorbis: native libvorbis");
register_native:
    guest_intercept(0x008107a0u, native_open);
    guest_intercept(0x008106a0u, native_clear);
    guest_intercept(0x008125c0u, native_info);
    guest_intercept(0x00812600u, native_read);
    guest_intercept(0x008124d0u, native_time_tell);
    guest_intercept(0x008123d0u, native_time_seek);
}
