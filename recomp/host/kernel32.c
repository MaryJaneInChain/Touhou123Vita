/* KERNEL32 for the recompiled game: the subset the MSVC 2005 CRT start-up and the
 * game's own initialisation need. Portable C (no host Win32 calls), so it can move
 * to the Vita unchanged. Threads are not started yet (CreateThread is recorded). */
#include "win32.h"
#include "sjis.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#define file_seek fseeko64
#define file_tell ftello64
#else
#define mkdir_host(p) mkdir(p, 0755)
#define file_seek(f, o, w) fseek(f, (long)(o), w) /* game files are below 2 GiB */
#define file_tell(f) ((int64_t)ftell(f))
#endif

enum { TLS_SLOTS = 64, TIB_TLS = 0xE10 };
static uint8_t tls_used[TLS_SLOTS];
static uint32_t command_line, environment_a, environment_w;

#define API(name) static void name(Cpu *c)

/* -- process, time ------------------------------------------------------------------ */
API(GetCurrentProcessId) { ret_stdcall(c, 0x1000, 0); }
API(GetCurrentThreadId) { ret_stdcall(c, 0x1004, 0); }
API(GetCurrentProcess) { ret_stdcall(c, 0xFFFFFFFFu, 0); }
API(IsDebuggerPresent) { ret_stdcall(c, 0, 0); }
API(SetUnhandledExceptionFilter) { ret_stdcall(c, 0, 1); }
API(GetLastError) { ret_stdcall(c, last_error, 0); }
API(SetLastError) { last_error = ARG(c, 0); ret_stdcall(c, 0, 1); }
API(ExitProcess) { host_exit(c, ARG(c, 0), "ExitProcess"); }
API(TerminateProcess) { host_exit(c, ARG(c, 1), "TerminateProcess"); }
API(Sleep) { ret_stdcall(c, 0, 1); }

API(GetTickCount) { ret_stdcall(c, (uint32_t)(platform_ticks_us() / 1000u), 0); }
API(QueryPerformanceCounter) {
    wr64(ARG(c, 0), platform_ticks_us()); /* 1 MHz counter */
    ret_stdcall(c, 1, 1);
}
API(GetSystemTimeAsFileTime) {
    /* FILETIME: 100 ns since 1601. */
    uint64_t t = ((uint64_t)time(NULL) + 11644473600ull) * 10000000ull;
    wr64(ARG(c, 0), t);
    ret_stdcall(c, 0, 1);
}
API(GetTimeZoneInformation) {
    memset(G(ARG(c, 0)), 0, 172);
    wr32(ARG(c, 0), (uint32_t)-540); /* Bias: UTC+9, the game's home time zone */
    ret_stdcall(c, 0 /* TIME_ZONE_ID_UNKNOWN */, 1);
}

/* -- heap and virtual memory ------------------------------------------------------- */
enum { HEAP_HANDLE = 0x00E00000u, HEAP_ZERO = 0x8 };
API(HeapCreate) { ret_stdcall(c, HEAP_HANDLE, 3); }
API(HeapDestroy) { ret_stdcall(c, 1, 1); }
API(GetProcessHeap) { ret_stdcall(c, HEAP_HANDLE, 0); }
API(HeapAlloc) { ret_stdcall(c, heap_alloc(ARG(c, 2), ARG(c, 1) & HEAP_ZERO), 3); }
static void check_heap_pointer(const char *api, uint32_t p) {
    if (p && heap_size(p) == 0xFFFFFFFFu) {
        uint32_t w[8];
        for (int k = 0; k < 8; k++) w[k] = rd32(p - 16 + 4 * k);
        trace("# %s(%08x): not a live heap block; header %08x %08x %08x %08x | %08x %08x %08x %08x", api, p,
              w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}
API(HeapFree) { check_heap_pointer("HeapFree", ARG(c, 2)); heap_free(ARG(c, 2)); ret_stdcall(c, 1, 3); }
API(HeapReAlloc) { ret_stdcall(c, heap_realloc(ARG(c, 2), ARG(c, 3), ARG(c, 1) & HEAP_ZERO), 4); }
API(HeapSize) { check_heap_pointer("HeapSize", ARG(c, 2)); ret_stdcall(c, heap_size(ARG(c, 2)), 3); }
API(VirtualAlloc) { ret_stdcall(c, virtual_alloc(ARG(c, 0), ARG(c, 1)), 4); }
API(VirtualFree) { ret_stdcall(c, 1, 3); }

/* -- TLS: the slots live in the guest TIB, as Windows lays them out ---------------- */
API(TlsAlloc) {
    for (unsigned i = 0; i < TLS_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            wr32(c->fs_base + TIB_TLS + 4 * i, 0);
            ret_stdcall(c, i, 0);
            return;
        }
    }
    ret_stdcall(c, 0xFFFFFFFFu, 0);
}
API(TlsFree) { if (ARG(c, 0) < TLS_SLOTS) tls_used[ARG(c, 0)] = 0; ret_stdcall(c, 1, 1); }
API(TlsGetValue) {
    uint32_t i = ARG(c, 0);
    last_error = 0;
    ret_stdcall(c, i < TLS_SLOTS ? rd32(c->fs_base + TIB_TLS + 4 * i) : 0, 1);
}
API(TlsSetValue) {
    uint32_t i = ARG(c, 0);
    if (i < TLS_SLOTS)
        wr32(c->fs_base + TIB_TLS + 4 * i, ARG(c, 1));
    ret_stdcall(c, i < TLS_SLOTS, 2);
}

/* -- modules -------------------------------------------------------------------------- */
API(GetModuleHandleA) {
    uint32_t name = ARG(c, 0);
    /* Only modules the game imports exist; others (mscoree.dll, probed by the CRT)
       are absent, as on a system without them. */
    const char *s = name ? STR(name) : NULL;
    uint32_t h = !s ? guest_image_base
               : !_stricmp(s, "kernel32.dll") || !_stricmp(s, "kernel32") ? FAKE_MODULE_KERNEL32
               : !_stricmp(s, "user32.dll") || !_stricmp(s, "user32") ? FAKE_MODULE_OTHER
               : 0;
    trace("GetModuleHandleA(%s) = %08x", name ? STR(name) : "NULL", h);
    ret_stdcall(c, h, 1);
}
API(LoadLibraryA) {
    trace("LoadLibraryA(%s)", STR(ARG(c, 0)));
    ret_stdcall(c, FAKE_MODULE_OTHER, 1);
}
API(FreeLibrary) { ret_stdcall(c, 1, 1); }
API(GetProcAddress) {
    uint32_t name = ARG(c, 1);
    uint32_t r = 0;
    if (name >= 0x10000) {
        r = api_address(STR(name));
        trace("GetProcAddress(%08x, %s) = %08x", ARG(c, 0), STR(name), r);
    }
    if (!r)
        last_error = 127; /* ERROR_PROC_NOT_FOUND */
    ret_stdcall(c, r, 2);
}
API(GetModuleFileNameA) {
    uint32_t buf = ARG(c, 1), n = ARG(c, 2);
    size_t len = strlen(guest_exe_path);
    if (n) {
        snprintf(STR(buf), n, "%s", guest_exe_path);
    }
    ret_stdcall(c, (uint32_t)(len < n ? len : n), 3);
}
API(GetCommandLineA) {
    if (!command_line)
        command_line = guest_strdup(guest_exe_path);
    ret_stdcall(c, command_line, 0);
}
API(GetStartupInfoA) {
    memset(G(ARG(c, 0)), 0, 68);
    wr32(ARG(c, 0), 68);
    ret_stdcall(c, 0, 1);
}
API(GetVersionExA) {
    uint32_t p = ARG(c, 0);
    wr32(p + 4, 5);    /* Windows XP SP3: 5.1.2600, VER_PLATFORM_WIN32_NT */
    wr32(p + 8, 1);
    wr32(p + 12, 2600);
    wr32(p + 16, 2);
    strcpy(STR(p + 20), "Service Pack 3");
    ret_stdcall(c, 1, 1);
}

/* -- environment ------------------------------------------------------------------------ */
API(GetEnvironmentStringsA) {
    if (!environment_a)
        environment_a = heap_alloc(2, 1);
    ret_stdcall(c, environment_a, 0);
}
API(GetEnvironmentStringsW) {
    if (!environment_w)
        environment_w = heap_alloc(4, 1);
    ret_stdcall(c, environment_w, 0);
}
API(FreeEnvironmentStrings) { ret_stdcall(c, 1, 1); }
API(SetEnvironmentVariableA) { ret_stdcall(c, 1, 2); }

/* -- synchronisation (single thread for now) --------------------------------------- */
API(CriticalSection1) { ret_stdcall(c, 0, 1); }
API(InitializeCriticalSectionAndSpinCount) { ret_stdcall(c, 1, 2); }
API(TryEnterCriticalSection) { ret_stdcall(c, 1, 1); }
API(InterlockedIncrement) {
    uint32_t p = ARG(c, 0);
    ret_stdcall(c, __atomic_add_fetch((uint32_t *)G(p), 1, __ATOMIC_SEQ_CST), 1);
}
API(InterlockedDecrement) {
    uint32_t p = ARG(c, 0);
    ret_stdcall(c, __atomic_sub_fetch((uint32_t *)G(p), 1, __ATOMIC_SEQ_CST), 1);
}
API(InterlockedExchange) {
    ret_stdcall(c, __atomic_exchange_n((uint32_t *)G(ARG(c, 0)), ARG(c, 1), __ATOMIC_SEQ_CST), 2);
}
API(InterlockedCompareExchange) {
    uint32_t expected = ARG(c, 2);
    __atomic_compare_exchange_n((uint32_t *)G(ARG(c, 0)), &expected, ARG(c, 1), 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    ret_stdcall(c, expected, 3);
}
API(CreateEventA) {
    trace("CreateEventA(manual=%u, initial=%u, %s)", ARG(c, 1), ARG(c, 2),
          ARG(c, 3) ? STR(ARG(c, 3)) : "NULL");
    ret_stdcall(c, handle_new(H_EVENT, NULL, ARG(c, 2)), 4);
}
API(CreateMutexA) {
    trace("CreateMutexA(%s)", ARG(c, 2) ? STR(ARG(c, 2)) : "NULL");
    last_error = 0;
    ret_stdcall(c, handle_new(H_MUTEX, NULL, 0), 3);
}
API(SetEvent) { ret_stdcall(c, 1, 1); }
API(WaitForSingleObject) {
    trace("WaitForSingleObject(%08x, %d)", ARG(c, 0), (int32_t)ARG(c, 1));
    ret_stdcall(c, 0 /* WAIT_OBJECT_0 */, 2);
}
API(CreateThread) {
    trace("CreateThread(start=%08x, param=%08x, flags=%08x): not started (single-threaded bring-up)",
          ARG(c, 2), ARG(c, 3), ARG(c, 4));
    if (ARG(c, 5))
        wr32(ARG(c, 5), 0x2000);
    ret_stdcall(c, handle_new(H_THREAD, NULL, ARG(c, 2)), 6);
}
API(SetThreadPriority) { ret_stdcall(c, 1, 2); }
API(GetExitCodeThread) { wr32(ARG(c, 1), 259 /* STILL_ACTIVE */); ret_stdcall(c, 1, 2); }
API(CloseHandle) { handle_close(ARG(c, 0)); ret_stdcall(c, 1, 1); }

/* -- code pages: the game runs under code page 932 (Shift-JIS) ------------------------ */
/* T123_GBK: the Chinese (汉化) data is GBK, so the system looks like a Chinese one (936)
   instead; the game's CRT takes its multibyte lead-byte ranges from GetACP/GetCPInfo. */
#ifdef T123_GBK
#define ANSI_CP 936
#else
#define ANSI_CP 932
#endif
API(GetACP) { ret_stdcall(c, ANSI_CP, 0); }
API(GetOEMCP) { ret_stdcall(c, ANSI_CP, 0); }
API(IsValidCodePage) { ret_stdcall(c, 1, 1); }
API(GetCPInfo) {
    uint32_t p = ARG(c, 1);
    memset(G(p), 0, 20);
    if (ARG(c, 0) == ANSI_CP || ARG(c, 0) == 0) {
        wr32(p, 2);              /* MaxCharSize */
        wr8(p + 4, '?');         /* DefaultChar */
#ifdef T123_GBK
        const uint8_t lead[] = {0x81, 0xFE, 0, 0};
#else
        const uint8_t lead[] = {0x81, 0x9F, 0xE0, 0xFC};
#endif
        memcpy(G(p + 6), lead, 4);
    } else {
        wr32(p, 1);
        wr8(p + 4, '?');
    }
    ret_stdcall(c, 1, 2);
}
#ifdef T123_GBK
API(GetUserDefaultLCID) { ret_stdcall(c, 0x0804 /* zh-CN */, 0); }
#else
API(GetUserDefaultLCID) { ret_stdcall(c, 0x0411 /* ja-JP */, 0); }
#endif

/* The game runs on a Japanese system: CP_ACP, CP_OEMCP, CP_THREAD_ACP and 932 are
   all Shift-JIS (sjis.c, Codex T7, validated against Windows). Other code pages are
   reported once and converted the same way. */
static int is_cp932(uint32_t cp) {
    static int reported;
    if (cp == 0 || cp == 1 || cp == 3 || cp == ANSI_CP)
        return 1;
    if (!reported++)
        trace("# code page %u converted as CP932", cp);
    return 1;
}

API(MultiByteToWideChar) {
    uint32_t src = ARG(c, 2), dst = ARG(c, 4), cap = ARG(c, 5);
    int32_t len = (int32_t)ARG(c, 3);
    (void)is_cp932(ARG(c, 0));
    int n = sjis_to_utf16((const uint8_t *)G(src), len, cap ? (uint16_t *)G(dst) : NULL, (int)cap);
    if (!n && len) last_error = cap ? 122 /* ERROR_INSUFFICIENT_BUFFER */ : 87;
    ret_stdcall(c, (uint32_t)n, 6);
}
API(WideCharToMultiByte) {
    uint32_t src = ARG(c, 2), dst = ARG(c, 4), cap = ARG(c, 5), used = ARG(c, 7);
    int32_t len = (int32_t)ARG(c, 3);
    (void)is_cp932(ARG(c, 0));
    int defaulted = 0;
    int n = utf16_to_sjis((const uint16_t *)G(src), len, cap ? (uint8_t *)G(dst) : NULL, (int)cap, &defaulted);
    if (used) wr32(used, (uint32_t)defaulted);
    if (!n && len) last_error = cap ? 122 : 87;
    ret_stdcall(c, (uint32_t)n, 8);
}
API(GetStringTypeW) {
    /* CT_CTYPE1 classification for ASCII; other characters are reported as alpha. */
    uint32_t src = ARG(c, 1), out = ARG(c, 3);
    int32_t len = (int32_t)ARG(c, 2);
    if (len < 0)
        for (len = 0; rd16(src + 2 * len); len++) {}
    for (int32_t i = 0; i < len; i++) {
        uint16_t ch = rd16(src + 2 * i), t = 0;
        if (ch < 0x80) {
            if (ch >= 'A' && ch <= 'Z') t |= 0x101;  /* UPPER | ALPHA */
            if (ch >= 'a' && ch <= 'z') t |= 0x102;  /* LOWER | ALPHA */
            if (ch >= '0' && ch <= '9') t |= 0x004 | 0x080; /* DIGIT | XDIGIT */
            if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) t |= 0x080;
            if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x008; /* SPACE */
            if (ch == ' ' || ch == '\t') t |= 0x040; /* BLANK */
            if (ch < 32 || ch == 127) t |= 0x020;  /* CNTRL */
            if (ch > 32 && ch < 127 && !(t & 0x107)) t |= 0x010; /* PUNCT */
        } else {
            t = 0x100;
        }
        wr16(out + 2 * i, t);
    }
    ret_stdcall(c, 1, 4);
}
API(LCMapStringW) {
    /* Used by the CRT for case tables: identity copy of the source. */
    uint32_t src = ARG(c, 2), dst = ARG(c, 4), cap = ARG(c, 5);
    int32_t len = (int32_t)ARG(c, 3);
    if (len < 0) {
        for (len = 0; rd16(src + 2 * len); len++) {}
        len++;
    }
    uint32_t flags = ARG(c, 1);
    if (cap) {
        if ((uint32_t)len > cap) { ret_stdcall(c, 0, 6); return; }
        for (int32_t i = 0; i < len; i++) {
            uint16_t ch = rd16(src + 2 * i);
            if ((flags & 0x100) && ch >= 'A' && ch <= 'Z') ch += 32;   /* LCMAP_LOWERCASE */
            if ((flags & 0x200) && ch >= 'a' && ch <= 'z') ch -= 32;   /* LCMAP_UPPERCASE */
            wr16(dst + 2 * i, ch);
        }
    }
    ret_stdcall(c, (uint32_t)len, 6);
}

/* -- files ------------------------------------------------------------------------------- */
enum { STD_INPUT = 0xFFFFFFF6u, STD_OUTPUT = 0xFFFFFFF5u, STD_ERROR = 0xFFFFFFF4u };
API(GetStdHandle) { ret_stdcall(c, ARG(c, 0) == STD_INPUT ? 0 : handle_new(H_STD, NULL, ARG(c, 0)), 1); }
API(SetStdHandle) { ret_stdcall(c, 1, 2); }
API(GetFileType) { ret_stdcall(c, handle_get(ARG(c, 0), H_FILE) || handle_get(ARG(c, 0), H_RFILE) ? 1 : 2 /* CHAR */, 1); }
API(SetHandleCount) { ret_stdcall(c, ARG(c, 0), 1); }

/* Read-only files (the DAT archives) bypass stdio. Measured on the Vita's card:
     - each open costs ~10 ms and the game reopens th123a.dat over a thousand times,
       so idle files stay open and are handed back on the next open of the same path;
     - stdio dropped and refilled its buffer on every fseek (SetFilePointer ~7 ms), so
       a seek only moves the position here, small reads come from one cached block and
       large reads go straight to the card. */
enum { RFILE_SLOTS = 8, RFILE_BLOCK = 32 * 1024 };
typedef struct {
    char path[256];
    int fd, in_use;
    int64_t size, pos, block_off;
    uint32_t block_len;
    uint8_t block[RFILE_BLOCK];
} RFile;
static RFile *rfiles[RFILE_SLOTS];
static unsigned rfile_next;
static pthread_mutex_t rfile_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef __vita__
#include <psp2/io/fcntl.h>
static int rf_open(const char *path) { int fd = sceIoOpen(path, SCE_O_RDONLY, 0); return fd < 0 ? -1 : fd; }
static void rf_close(int fd) { sceIoClose(fd); }
static int64_t rf_size(int fd) { return sceIoLseek(fd, 0, SCE_SEEK_END); }
static int32_t rf_read_at(int fd, void *dst, uint32_t n, int64_t off) { return sceIoPread(fd, dst, n, off); }
#elif !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
static int rf_open(const char *path) { return open(path, O_RDONLY); }
static void rf_close(int fd) { close(fd); }
static int64_t rf_size(int fd) { return lseek(fd, 0, SEEK_END); }
static int32_t rf_read_at(int fd, void *dst, uint32_t n, int64_t off) { return (int32_t)pread(fd, dst, n, off); }
#else
#include <fcntl.h>
#include <io.h>
static int rf_open(const char *path) { return _open(path, _O_RDONLY | _O_BINARY); }
static void rf_close(int fd) { _close(fd); }
static int64_t rf_size(int fd) { return _lseeki64(fd, 0, SEEK_END); }
static int32_t rf_read_at(int fd, void *dst, uint32_t n, int64_t off) {
    return _lseeki64(fd, off, SEEK_SET) < 0 ? -1 : _read(fd, dst, n);
}
#endif

static RFile *rfile_open(const char *path) {
    RFile *r = NULL;
    pthread_mutex_lock(&rfile_lock);
    for (unsigned i = 0; i < RFILE_SLOTS && !r; i++)
        if (rfiles[i] && !rfiles[i]->in_use && !strcmp(rfiles[i]->path, path)) {
            r = rfiles[i];
            r->in_use = 1;
        }
    pthread_mutex_unlock(&rfile_lock);
    if (r) {
        r->pos = 0; /* the cached block stays valid: the file did not change */
        return r;
    }
    int fd = rf_open(path);
    if (fd < 0 || !(r = calloc(1, sizeof *r))) {
        if (fd >= 0) rf_close(fd);
        return NULL;
    }
    snprintf(r->path, sizeof r->path, "%s", path);
    r->fd = fd;
    r->size = rf_size(fd);
    r->block_off = -1;
    r->in_use = 1;
    if (strlen(path) < sizeof r->path) {
        pthread_mutex_lock(&rfile_lock);
        for (unsigned k = 0; k < RFILE_SLOTS; k++) {
            unsigned i = (rfile_next + k) % RFILE_SLOTS;
            if (rfiles[i] && rfiles[i]->in_use)
                continue;
            if (rfiles[i]) { /* evict an idle one */
                rf_close(rfiles[i]->fd);
                free(rfiles[i]);
            }
            rfiles[i] = r;
            rfile_next = i + 1;
            break;
        }
        pthread_mutex_unlock(&rfile_lock);
    }
    return r;
}

/* handle_close for H_RFILE: idle files stay open for reuse. */
void rfile_close(void *p) {
    RFile *r = p;
    pthread_mutex_lock(&rfile_lock);
    for (unsigned i = 0; i < RFILE_SLOTS; i++)
        if (rfiles[i] == r) {
            r->in_use = 0;
            pthread_mutex_unlock(&rfile_lock);
            return;
        }
    pthread_mutex_unlock(&rfile_lock);
    rf_close(r->fd);
    free(r);
}

static uint32_t rfile_read(RFile *r, uint8_t *dst, uint32_t n) {
    uint32_t done = 0;
    while (done < n && r->pos < r->size) {
        int64_t in_block = r->pos - r->block_off;
        if (r->block_off >= 0 && in_block >= 0 && in_block < r->block_len) {
            uint32_t k = r->block_len - (uint32_t)in_block;
            if (k > n - done) k = n - done;
            memcpy(dst + done, r->block + in_block, k);
            done += k;
            r->pos += k;
        } else if (n - done >= RFILE_BLOCK) {
            int32_t k = rf_read_at(r->fd, dst + done, n - done, r->pos);
            if (k <= 0) break;
            done += (uint32_t)k;
            r->pos += k;
        } else {
            int32_t k = rf_read_at(r->fd, r->block, RFILE_BLOCK, r->pos);
            if (k <= 0) break;
            r->block_off = r->pos;
            r->block_len = (uint32_t)k;
        }
    }
    return done;
}

API(CreateFileA) {
    char path[1200];
    const char *name = STR(ARG(c, 0));
    path_to_host(name, path, sizeof path);
    uint32_t access = ARG(c, 1), disposition = ARG(c, 4);
    const char *mode = (access & 0x40000000u) ? (disposition == 3 || disposition == 4 ? "r+b" : "w+b")
                                               : "rb";
    uint32_t h = 0xFFFFFFFFu;
    if (!(access & 0x40000000u)) {
        RFile *r = rfile_open(path);
        if (r) h = handle_new(H_RFILE, r, 0);
    } else {
        FILE *f = fopen(path, mode);
        if (!f && disposition == 4)
            f = fopen(path, "w+b");
        if (f) h = handle_new(H_FILE, f, 0);
    }
    last_error = h != 0xFFFFFFFFu ? 0 : 2; /* ERROR_FILE_NOT_FOUND */
    trace("CreateFileA(%s, access=%08x, disposition=%u) = %08x (%s)", name, access, disposition, h, path);
    ret_stdcall(c, h, 7);
}
API(ReadFile) {
    RFile *r = handle_get(ARG(c, 0), H_RFILE);
    FILE *f = r ? NULL : handle_get(ARG(c, 0), H_FILE);
    uint32_t n = r ? rfile_read(r, (uint8_t *)G(ARG(c, 1)), ARG(c, 2))
               : f ? (uint32_t)fread(G(ARG(c, 1)), 1, ARG(c, 2), f) : 0;
    if (ARG(c, 3))
        wr32(ARG(c, 3), n);
    ret_stdcall(c, r || f, 5);
}
API(WriteFile) {
    uint32_t h = ARG(c, 0), n = ARG(c, 2);
    FILE *f = handle_get(h, H_FILE);
    if (f)
        n = (uint32_t)fwrite(G(ARG(c, 1)), 1, n, f);
    else if (handle_get(h, H_STD) || !h)
        trace("WriteFile(std): %.*s", (int)n, STR(ARG(c, 1)));
    if (ARG(c, 3))
        wr32(ARG(c, 3), n);
    ret_stdcall(c, 1, 5);
}
API(SetFilePointer) {
    RFile *r = handle_get(ARG(c, 0), H_RFILE);
    FILE *f = r ? NULL : handle_get(ARG(c, 0), H_FILE);
    int64_t distance = (int32_t)ARG(c, 1), pos;
    if (ARG(c, 2))
        distance = (int64_t)(((uint64_t)rd32(ARG(c, 2)) << 32) | ARG(c, 1));
    uint32_t method = ARG(c, 3);
    if (r) {
        pos = (method == 0 ? 0 : method == 1 ? r->pos : r->size) + distance;
        if (pos < 0) { ret_stdcall(c, 0xFFFFFFFFu, 4); return; }
        r->pos = pos; /* no I/O: the next read decides what to fetch */
    } else {
        if (!f || file_seek(f, distance, (int)method)) {
            ret_stdcall(c, 0xFFFFFFFFu, 4);
            return;
        }
        pos = file_tell(f);
    }
    if (ARG(c, 2))
        wr32(ARG(c, 2), (uint32_t)((uint64_t)pos >> 32));
    ret_stdcall(c, (uint32_t)pos, 4);
}
API(GetFileSize) {
    RFile *r = handle_get(ARG(c, 0), H_RFILE);
    FILE *f = r ? NULL : handle_get(ARG(c, 0), H_FILE);
    if (!r && !f) { ret_stdcall(c, 0xFFFFFFFFu, 2); return; }
    int64_t size;
    if (r) {
        size = r->size;
    } else {
        int64_t here = file_tell(f);
        file_seek(f, 0, SEEK_END);
        size = file_tell(f);
        file_seek(f, here, SEEK_SET);
    }
    if (ARG(c, 1))
        wr32(ARG(c, 1), (uint32_t)((uint64_t)size >> 32));
    ret_stdcall(c, (uint32_t)size, 2);
}
API(FlushFileBuffers) {
    FILE *f = handle_get(ARG(c, 0), H_FILE);
    if (f) fflush(f);
    ret_stdcall(c, 1, 1);
}
API(GetFileAttributesA) {
    char path[1200];
    struct stat st;
    path_to_host(STR(ARG(c, 0)), path, sizeof path);
    uint32_t r = stat(path, &st) ? 0xFFFFFFFFu : (st.st_mode & S_IFDIR) ? 0x10 : 0x80;
    trace("GetFileAttributesA(%s) = %08x", STR(ARG(c, 0)), r);
    ret_stdcall(c, r, 1);
}
/* FindFirstFileA / FindNextFileA: directory listing with * and ? (case-insensitive),
   used by the replay and profile menus. WIN32_FIND_DATAA: attributes at 0, size at
   28 (high) and 32 (low), name at 44. */
typedef struct { DIR *dir; char dir_path[1024], pattern[260]; } FindState;

static int wild_match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++)
                if (wild_match(p + 1, s)) return 1;
            return 0;
        }
        if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))) return 0;
    }
    return !*s;
}

static int find_next(FindState *f, uint32_t out) {
    struct dirent *e;
    while (f->dir && (e = readdir(f->dir))) {
        if (!wild_match(f->pattern, e->d_name)) continue;
        char full[1300];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir_path, e->d_name);
        int is_dir = stat(full, &st) == 0 && (st.st_mode & S_IFDIR);
        memset(G(out), 0, 320);
        wr32(out, is_dir ? 0x10 : 0x80);
        wr32(out + 32, is_dir ? 0 : (uint32_t)st.st_size);
        snprintf(STR(out + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}

API(FindFirstFileA) {
    char path[1200];
    path_to_host(STR(ARG(c, 0)), path, sizeof path);
    FindState *f = calloc(1, sizeof *f);
    char *slash = strrchr(path, '/');
    snprintf(f->pattern, sizeof f->pattern, "%s", slash ? slash + 1 : path);
    if (slash) { *slash = 0; snprintf(f->dir_path, sizeof f->dir_path, "%s", path); }
    else snprintf(f->dir_path, sizeof f->dir_path, ".");
    f->dir = opendir(f->dir_path);
    uint32_t h = 0xFFFFFFFFu;
    if (find_next(f, ARG(c, 1))) {
        h = handle_new(H_FIND, f, 0);
    } else {
        if (f->dir) closedir(f->dir);
        free(f);
        last_error = 2; /* ERROR_FILE_NOT_FOUND */
    }
    trace("FindFirstFileA(%s) = %08x", STR(ARG(c, 0)), h);
    ret_stdcall(c, h, 2);
}
API(FindNextFileA) {
    FindState *f = handle_get(ARG(c, 0), H_FIND);
    int ok = f && find_next(f, ARG(c, 1));
    if (!ok) last_error = 18; /* ERROR_NO_MORE_FILES */
    ret_stdcall(c, (uint32_t)ok, 2);
}
API(FindClose) { handle_close(ARG(c, 0)); ret_stdcall(c, 1, 1); }

/* handle_close for H_FIND */
void find_close(void *p) {
    FindState *f = p;
    if (f->dir) closedir(f->dir);
    free(f);
}

API(CreateDirectoryA) {
    char path[1200];
    path_to_host(STR(ARG(c, 0)), path, sizeof path);
    ret_stdcall(c, mkdir_host(path) == 0, 2);
}
API(DeleteFileA) {
    char path[1200];
    path_to_host(STR(ARG(c, 0)), path, sizeof path);
    ret_stdcall(c, remove(path) == 0, 1);
}
/* CopyFileA(src, dst, failIfExists) and MoveFileA(src, dst): the game copies and
   renames its replay and profile files (it ended a session with "import CopyFileA is
   not implemented" the first time it saved one). */
static int copy_host_file(const char *from, const char *to, int fail_if_exists) {
    if (fail_if_exists) {
        FILE *probe = fopen(to, "rb");
        if (probe) { fclose(probe); last_error = 80; /* ERROR_FILE_EXISTS */ return 0; }
    }
    FILE *in = fopen(from, "rb");
    if (!in) { last_error = 2; return 0; }
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); last_error = 5; return 0; }
    static uint8_t chunk[32768];
    size_t n;
    int ok = 1;
    while ((n = fread(chunk, 1, sizeof chunk, in)) > 0)
        if (fwrite(chunk, 1, n, out) != n) { ok = 0; break; }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) last_error = 29; /* ERROR_WRITE_FAULT */
    return ok;
}
API(CopyFileA) {
    char from[1200], to[1200];
    path_to_host(STR(ARG(c, 0)), from, sizeof from);
    path_to_host(STR(ARG(c, 1)), to, sizeof to);
    int ok = copy_host_file(from, to, (int)ARG(c, 2));
    trace("CopyFileA(%s, %s, %u) = %d", STR(ARG(c, 0)), STR(ARG(c, 1)), ARG(c, 2), ok);
    ret_stdcall(c, (uint32_t)ok, 3);
}
API(MoveFileA) {
    char from[1200], to[1200];
    path_to_host(STR(ARG(c, 0)), from, sizeof from);
    path_to_host(STR(ARG(c, 1)), to, sizeof to);
    int ok = 0;
    FILE *probe = fopen(to, "rb");
    if (probe) { fclose(probe); last_error = 183; /* ERROR_ALREADY_EXISTS */ }
    else if (rename(from, to) == 0) ok = 1;
    else if (copy_host_file(from, to, 1)) { remove(from); ok = 1; }
    trace("MoveFileA(%s, %s) = %d", STR(ARG(c, 0)), STR(ARG(c, 1)), ok);
    ret_stdcall(c, (uint32_t)ok, 2);
}
/* The clipboard is empty (GetClipboardData returns 0), so a locked global never really
   happens: the handle stands for its own pointer. */
API(GlobalLock) { ret_stdcall(c, ARG(c, 0), 1); }
API(GlobalUnlock) { ret_stdcall(c, 0, 1); }

/* CT_CTYPE1 classification of single-byte characters (ASCII; the rest is not
   classified), and ordinal string comparison with NORM_IGNORECASE. */
static uint16_t ctype1(unsigned ch) {
    uint16_t t = 0;
    if (ch < 128) {
        if (ch >= 'A' && ch <= 'Z') t |= 0x1 | 0x100;
        if (ch >= 'a' && ch <= 'z') t |= 0x2 | 0x100;
        if (ch >= '0' && ch <= '9') t |= 0x4 | 0x80;
        if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) t |= 0x80;
        if (ch == ' ' || (ch >= 9 && ch <= 13)) t |= 0x8;
        if (ch == ' ' || ch == 9) t |= 0x40;
        if (ch < 32 || ch == 127) t |= 0x20;
        if (ch > 32 && ch < 127 && !(t & (0x100 | 0x4))) t |= 0x10;
    }
    return t;
}
API(GetStringTypeExA) { /* (locale, infoType, src, cch, out) */
    uint32_t type = ARG(c, 1), src = ARG(c, 2), out = ARG(c, 4);
    int32_t cch = (int32_t)ARG(c, 3);
    if (cch < 0) cch = (int32_t)strlen(STR(src)) + 1;
    if (type != 1) { last_error = 87; ret_stdcall(c, 0, 5); return; } /* only CT_CTYPE1 */
    for (int32_t i = 0; i < cch; i++) wr16(out + 2u * (uint32_t)i, ctype1(*(const uint8_t *)G(src + (uint32_t)i)));
    ret_stdcall(c, 1, 5);
}
static int fold(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static uint32_t compare_units(uint32_t flags, const void *a, int32_t na, const void *b, int32_t nb, int wide) {
    for (int32_t i = 0; ; i++) {
        int ea = wide ? i >= na : i >= na || ((const uint8_t *)a)[i] == 0 ? 1 : 0;
        int eb = wide ? i >= nb : i >= nb || ((const uint8_t *)b)[i] == 0 ? 1 : 0;
        if (ea || eb) return ea && eb ? 2 : ea ? 1 : 3;
        int x = wide ? ((const uint16_t *)a)[i] : ((const uint8_t *)a)[i];
        int y = wide ? ((const uint16_t *)b)[i] : ((const uint8_t *)b)[i];
        if (flags & 1 /* NORM_IGNORECASE */) { x = fold(x); y = fold(y); }
        if (x != y) return x < y ? 1 : 3;
    }
}
API(CompareStringA) { /* (locale, flags, s1, n1, s2, n2) */
    const char *a = STR(ARG(c, 2)), *b = STR(ARG(c, 4));
    int32_t na = (int32_t)ARG(c, 3), nb = (int32_t)ARG(c, 5);
    ret_stdcall(c, compare_units(ARG(c, 1), a, na < 0 ? (int32_t)strlen(a) : na, b, nb < 0 ? (int32_t)strlen(b) : nb, 0), 6);
}
API(CompareStringW) {
    const uint16_t *a = (const uint16_t *)G(ARG(c, 2)), *b = (const uint16_t *)G(ARG(c, 4));
    int32_t na = (int32_t)ARG(c, 3), nb = (int32_t)ARG(c, 5);
    if (na < 0) for (na = 0; a[na]; na++) {}
    if (nb < 0) for (nb = 0; b[nb]; nb++) {}
    ret_stdcall(c, compare_units(ARG(c, 1), a, na, b, nb, 1), 6);
}
API(SetCurrentDirectoryA) {
    trace("SetCurrentDirectoryA(%s)", STR(ARG(c, 0)));
    ret_stdcall(c, 1, 1);
}
API(GetCurrentDirectoryA) {
    const char *dir = "C:\\th123";
    uint32_t n = ARG(c, 0);
    if (n > strlen(dir))
        strcpy(STR(ARG(c, 1)), dir);
    ret_stdcall(c, (uint32_t)strlen(dir), 2);
}
API(lstrlenA) { ret_stdcall(c, ARG(c, 0) ? (uint32_t)strlen(STR(ARG(c, 0))) : 0, 1); }

/* GetPrivateProfile*: the game reads its .ini; values come from the user's copy when
   present, otherwise the defaults the game passes. */
static int profile_lookup(const char *file, const char *section, const char *key, char *out, size_t n) {
    char path[1200], line[512], current[256] = "";
    path_to_host(file, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = 0; snprintf(current, sizeof current, "%s", p + 1); }
        } else if (!_stricmp(current, section)) {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                char *k = p, *v = eq + 1;
                for (char *t = k + strlen(k); t > k && (t[-1] == ' ' || t[-1] == '\t'); ) *--t = 0;
                v[strcspn(v, "\r\n")] = 0;
                if (!_stricmp(k, key)) { snprintf(out, n, "%s", v); found = 1; }
            }
        }
    }
    fclose(f);
    return found;
}
API(GetPrivateProfileIntA) {
    char value[256];
    uint32_t r = ARG(c, 2);
    if (profile_lookup(STR(ARG(c, 3)), STR(ARG(c, 0)), STR(ARG(c, 1)), value, sizeof value))
        r = (uint32_t)strtol(value, NULL, 10);
    trace("GetPrivateProfileIntA([%s] %s, %s) = %d", STR(ARG(c, 0)), STR(ARG(c, 1)), STR(ARG(c, 3)), (int)r);
    ret_stdcall(c, r, 4);
}
API(GetPrivateProfileStringA) {
    char value[256];
    uint32_t buf = ARG(c, 3), n = ARG(c, 4);
    const char *def = ARG(c, 2) ? STR(ARG(c, 2)) : "";
    if (!ARG(c, 0) || !ARG(c, 1) ||
        !profile_lookup(STR(ARG(c, 5)), STR(ARG(c, 0)), STR(ARG(c, 1)), value, sizeof value))
        snprintf(value, sizeof value, "%s", def);
    if (n)
        snprintf(STR(buf), n, "%s", value);
    trace("GetPrivateProfileStringA([%s] %s) = \"%s\"", ARG(c, 0) ? STR(ARG(c, 0)) : "",
          ARG(c, 1) ? STR(ARG(c, 1)) : "", value);
    ret_stdcall(c, n ? (uint32_t)strlen(STR(buf)) : 0, 6);
}

/* -- console (CRT error output only) ------------------------------------------------- */
API(GetConsoleCP) { ret_stdcall(c, ANSI_CP, 0); }
API(GetConsoleOutputCP) { ret_stdcall(c, ANSI_CP, 0); }
API(GetConsoleMode) { ret_stdcall(c, 0, 2); }

/* -- exceptions: C++ throw/unwind are not supported yet -------------------------------- */
/* The CRT reaches this on a fatal error (stack cookie, invalid parameter, abort):
   report the thread and its last guest functions, then stop. */
API(UnhandledExceptionFilter) {
    uint32_t record = ARG(c, 0) ? rd32(ARG(c, 0)) : 0;
    trace("# UnhandledExceptionFilter on thread %u: exception %08x at %08x", rd32(c->fs_base + 0x24),
          record ? rd32(record) : 0, record ? rd32(record + 12) : 0);
    trace("# last guest functions entered on this thread (oldest first):");
    for (unsigned k = 0; k < 64; k++) {
        uint32_t f = guest_trace_ring[(guest_trace_pos + k) & 63];
        if (f) trace("#   %08x", f);
    }
    host_exit(c, 0xC0000409u, "UnhandledExceptionFilter: fatal CRT error in the guest");
}
API(RaiseException) {
    trace("RaiseException(code=%08x, flags=%u, args=%u)", ARG(c, 0), ARG(c, 1), ARG(c, 2));
    host_exit(c, 0xE06D7363u, "RaiseException (C++ throw): SEH unwinding is not implemented");
}

#define E(name) {#name, name}
const ApiEntry win32_kernel32[] = {
    E(GetCurrentProcessId), E(GetCurrentThreadId), E(GetCurrentProcess), E(IsDebuggerPresent),
    E(SetUnhandledExceptionFilter), E(GetLastError), E(SetLastError), E(ExitProcess),
    E(TerminateProcess), E(Sleep), E(GetTickCount), E(QueryPerformanceCounter),
    E(GetSystemTimeAsFileTime), E(GetTimeZoneInformation),
    E(HeapCreate), E(HeapDestroy), E(GetProcessHeap), E(HeapAlloc), E(HeapFree), E(HeapReAlloc),
    E(HeapSize), E(VirtualAlloc), E(VirtualFree),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue),
    E(GetModuleHandleA), E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress), E(GetModuleFileNameA),
    E(GetCommandLineA), E(GetStartupInfoA), E(GetVersionExA),
    {"GetEnvironmentStrings", GetEnvironmentStringsA}, E(GetEnvironmentStringsW),
    {"FreeEnvironmentStringsA", FreeEnvironmentStrings}, {"FreeEnvironmentStringsW", FreeEnvironmentStrings},
    E(SetEnvironmentVariableA),
    {"InitializeCriticalSection", CriticalSection1}, {"DeleteCriticalSection", CriticalSection1},
    {"EnterCriticalSection", CriticalSection1}, {"LeaveCriticalSection", CriticalSection1},
    E(InitializeCriticalSectionAndSpinCount),
    E(TryEnterCriticalSection), E(InterlockedIncrement), E(InterlockedDecrement), E(InterlockedExchange),
    E(InterlockedCompareExchange), E(CreateEventA), E(CreateMutexA), E(SetEvent), E(WaitForSingleObject),
    E(CreateThread), E(SetThreadPriority), E(GetExitCodeThread), E(CloseHandle),
    E(GetACP), E(GetOEMCP), E(IsValidCodePage), E(GetCPInfo), E(GetUserDefaultLCID),
    E(MultiByteToWideChar), E(WideCharToMultiByte), E(GetStringTypeW), E(LCMapStringW),
    E(GetStdHandle), E(SetStdHandle), E(GetFileType), E(SetHandleCount), E(CreateFileA), E(ReadFile),
    E(WriteFile), E(SetFilePointer), E(GetFileSize), E(FlushFileBuffers), E(GetFileAttributesA),
    E(FindFirstFileA), E(FindNextFileA), E(FindClose),
    E(CreateDirectoryA), E(DeleteFileA), E(SetCurrentDirectoryA), E(GetCurrentDirectoryA), E(lstrlenA),
    E(CopyFileA), E(MoveFileA), E(GlobalLock), E(GlobalUnlock), E(GetStringTypeExA),
    {"GetStringTypeA", GetStringTypeExA}, E(CompareStringA), E(CompareStringW),
    E(GetPrivateProfileIntA), E(GetPrivateProfileStringA),
    E(GetConsoleCP), E(GetConsoleOutputCP), E(GetConsoleMode), E(RaiseException), E(UnhandledExceptionFilter),
    {NULL, NULL},
};
