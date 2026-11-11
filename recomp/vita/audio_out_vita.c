/* Vita audio output: one thread mixes the DirectSound voices (audio_mix.c) and hands
 * 1024-frame periods to the main audio port; sceAudioOutOutput blocks until the port
 * can take the next one, which paces the mixing. */
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>

#include "../host/audio_mix.h"
#include "../host/win32.h"

enum { OUT_RATE = 48000, PERIOD = 1024 };

static void *audio_thread(void *arg) {
    int port = (int)(intptr_t)arg;
    platform_pin_helper("audio");
    static int16_t mix[2][PERIOD * 2];
    /* above the game's threads (threads.c maps TIME_CRITICAL to 120): a late period
       is an audible gap */
    int p = 96, r;
    while ((r = sceKernelChangeThreadPriority(0, p)) < 0 && p < 160)
        p += 4;
    trace("# audio: thread priority %d (%08x)", p, (unsigned)r);
    /* Health of the output, printed every 5 s while something is sounding: a period
       that takes much longer than its 21.3 ms means the thread did not get the CPU in
       time (an audible gap or a replayed buffer), and the peak/limiter counters show
       whether the mix itself reaches full scale. */
    uint64_t report_at = platform_ticks_us() + 5000000, periods = 0, late = 0, late_max = 0, mix_sum = 0, mix_max = 0;
    uint64_t last_end = 0;
    for (unsigned k = 0;; k ^= 1) {
        uint64_t t0 = platform_ticks_us();
        audio_mix(mix[k], PERIOD, OUT_RATE);
        uint64_t t1 = platform_ticks_us();
        sceAudioOutOutput(port, mix[k]);
        uint64_t t2 = platform_ticks_us();
        periods++;
        mix_sum += t1 - t0;
        if (t1 - t0 > mix_max) mix_max = t1 - t0;
        if (last_end && t2 - last_end > 26000) {
            late++;
            if (t2 - last_end > late_max) late_max = t2 - last_end;
        }
        last_end = t2;
        if (t2 >= report_at) {
            AudioStats s;
            audio_mix_stats(&s);
            if (s.max_voices)
                trace("# audio: %llu periods, %llu late (worst %llu us, nominal 21333), mix %llu us avg / %llu us max, "
                      "peak %d, %llu frames in the limiter zone, %llu past full scale, up to %d voices",
                      (unsigned long long)periods, (unsigned long long)late, (unsigned long long)late_max,
                      (unsigned long long)(mix_sum / periods), (unsigned long long)mix_max, s.peak,
                      (unsigned long long)s.limited, (unsigned long long)s.clipped, s.max_voices);
            periods = late = late_max = mix_sum = mix_max = 0;
            report_at = t2 + 5000000;
        }
    }
    return NULL;
}

/* nosound.txt: no port, no mixing; the voices' positions still advance in real time so the
   game's music refill, which follows them, keeps running */
static void *silent_thread(void *arg) {
    (void)arg;
    platform_pin_helper("audio clock");
    uint64_t next = platform_ticks_us();
    for (;;) {
        audio_advance(PERIOD, OUT_RATE);
        next += 1000000ull * PERIOD / OUT_RATE;
        uint64_t now = platform_ticks_us();
        if (next > now) sceKernelDelayThread((SceUInt32)(next - now));
        else if (now - next > 200000) next = now; /* fell far behind (a stall): do not catch up in a burst */
    }
    return NULL;
}

void audio_out_start(void) {
    static int started;
    if (started++)
        return;
    if (host_nosound()) {
        pthread_t id;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        if (pthread_create(&id, &attr, silent_thread, NULL)) trace("# audio: cannot start the silent clock");
        else trace("# audio: OFF (nosound.txt): no output, no mixing, no music decoding");
        pthread_attr_destroy(&attr);
        return;
    }
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, PERIOD, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        trace("# audio: sceAudioOutOpenPort failed (%08x)", (unsigned)port);
        return;
    }
    pthread_t id;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&id, &attr, audio_thread, (void *)(intptr_t)port))
        trace("# audio: cannot start the audio thread");
    else
        trace("# audio: Vita output %d Hz stereo, %d frames per period", OUT_RATE, PERIOD);
    pthread_attr_destroy(&attr);
}
