/* USER32, GDI32 (fonts and glyphs), WINMM, OLE32, IMM32 and WS2_32. Windows are
 * handles and their events come from the renderer (render_pump_events); MessageBoxA
 * texts are written to the trace. */
#include "glyph.h"
#include "net.h"
#include "render.h"
#include "sjis.h"
#include "win32.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#define API(name) static void name(Cpu *c)

/* -- WINMM -------------------------------------------------------------------------- */
API(timeBeginPeriod) { ret_stdcall(c, 0, 1); }
API(timeGetTime) { ret_stdcall(c, (uint32_t)(platform_ticks_us() / 1000u), 0); }

/* -- USER32 ------------------------------------------------------------------------- */
enum { FAKE_HWND = 0x00010010u, FAKE_HDC = 0x00020020u, FAKE_HICON = 0x00030030u };
static uint32_t window_proc;

API(RegisterClassExA) {
    uint32_t wc = ARG(c, 0);
    window_proc = rd32(wc + 8);
    trace("RegisterClassExA(%s, wndproc=%08x)", rd32(wc + 40) >= 0x10000 ? STR(rd32(wc + 40)) : "#atom",
          window_proc);
    ret_stdcall(c, 0xC001, 1);
}
API(CreateWindowExA) {
    trace("CreateWindowExA(class=%s, title=%s, style=%08x, %dx%d)",
          ARG(c, 1) >= 0x10000 ? STR(ARG(c, 1)) : "#atom", ARG(c, 2) ? STR(ARG(c, 2)) : "",
          ARG(c, 3), (int)ARG(c, 6), (int)ARG(c, 7));
    ret_stdcall(c, FAKE_HWND, 12);
}
API(ShowWindow) { ret_stdcall(c, 0, 2); }
API(UpdateWindow) { ret_stdcall(c, 1, 1); }
API(SetWindowPos) { ret_stdcall(c, 1, 7); }
API(SetWindowTextA) { ret_stdcall(c, 1, 2); }
API(GetWindowRect) {
    uint32_t r = ARG(c, 1);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, 640); wr32(r + 12, 480);
    ret_stdcall(c, 1, 2);
}
API(GetWindowLongA) { ret_stdcall(c, 0, 2); }
API(GetWindowInfo) {
    uint32_t p = ARG(c, 1);
    memset(G(p + 4), 0, 56);
    wr32(p + 4 + 8, 640); wr32(p + 4 + 12, 480);   /* rcWindow */
    wr32(p + 20 + 8, 640); wr32(p + 20 + 12, 480); /* rcClient */
    ret_stdcall(c, 1, 2);
}
API(SetRect) {
    uint32_t r = ARG(c, 0);
    wr32(r, ARG(c, 1)); wr32(r + 4, ARG(c, 2)); wr32(r + 8, ARG(c, 3)); wr32(r + 12, ARG(c, 4));
    ret_stdcall(c, 1, 5);
}
API(GetSystemMetrics) {
    uint32_t i = ARG(c, 0);
    ret_stdcall(c, i == 0 ? 960 : i == 1 ? 544 : 0, 1); /* screen: the Vita's 960x544 */
}
API(LoadCursorA) { ret_stdcall(c, FAKE_HICON, 2); }
API(LoadIconA) { ret_stdcall(c, FAKE_HICON, 2); }
API(LoadImageA) { ret_stdcall(c, FAKE_HICON, 6); }
API(LoadStringA) {
    if (ARG(c, 3)) wr8(ARG(c, 2), 0);
    ret_stdcall(c, 0, 4);
}
API(MessageBoxA) {
    trace("MessageBoxA(title=\"%s\"): %s", ARG(c, 2) ? STR(ARG(c, 2)) : "",
          ARG(c, 1) ? STR(ARG(c, 1)) : "");
    ret_stdcall(c, 1 /* IDOK */, 4);
}
/* Messages: SDL events are polled here; only closing the window produces a message
   (WM_QUIT), which ends the game's loop the way the original window would. */
/* Synthetic window messages (the Vita keyboard in vita/ime_vita.c): PeekMessageA hands
   them to the game's own message loop and DispatchMessageA runs its window procedure,
   so the text is processed on the game's thread like a real keyboard's. */
enum { POST_QUEUE = 512 };
static struct { uint32_t msg, wparam, lparam; } post_queue[POST_QUEUE];
static unsigned post_head, post_tail;
static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
void host_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam) {
    pthread_mutex_lock(&post_lock);
    if ((post_tail + 1) % POST_QUEUE != post_head) {
        post_queue[post_tail].msg = msg; post_queue[post_tail].wparam = wparam; post_queue[post_tail].lparam = lparam;
        post_tail = (post_tail + 1) % POST_QUEUE;
    }
    pthread_mutex_unlock(&post_lock);
}

/* Copies the first queued message into the guest's MSG; removes it when asked. */
static int take_message(uint32_t msg, int remove) {
    int have = 0;
    pthread_mutex_lock(&post_lock);
    if (post_head != post_tail) {
        memset(G(msg), 0, 28);                       /* MSG: hwnd, message, wParam, lParam, time, pt */
        wr32(msg, FAKE_HWND);
        wr32(msg + 4, post_queue[post_head].msg);
        wr32(msg + 8, post_queue[post_head].wparam);
        wr32(msg + 12, post_queue[post_head].lparam);
        wr32(msg + 16, (uint32_t)(platform_ticks_us() / 1000u));
        if (remove) post_head = (post_head + 1) % POST_QUEUE;
        have = 1;
    }
    pthread_mutex_unlock(&post_lock);
    return have;
}

API(PeekMessageA) {
    uint32_t msg = ARG(c, 0);
    if (render_pump_events()) {
        memset(G(msg), 0, 28);
        wr32(msg + 4, 0x0012 /* WM_QUIT */);
        ret_stdcall(c, 1, 5);
        return;
    }
    ret_stdcall(c, (uint32_t)take_message(msg, (ARG(c, 4) & 1) /* PM_REMOVE */), 5);
}
/* The game's main loop (0x407cf0) calls PeekMessageA with PM_NOREMOVE to see whether
   anything is waiting and then GetMessageA to take it. Returns 1 for a message, 0 for
   WM_QUIT; waits for one when the queue is empty, like the real one. */
API(GetMessageA) { /* (msg, hwnd, min, max) */
    uint32_t msg = ARG(c, 0);
    for (;;) {
        if (render_pump_events()) {
            memset(G(msg), 0, 28);
            wr32(msg + 4, 0x0012 /* WM_QUIT */);
            ret_stdcall(c, 0, 4);
            return;
        }
        if (take_message(msg, 1)) {
            ret_stdcall(c, 1, 4);
            return;
        }
        platform_sleep_ms(1);
    }
}
API(DispatchMessageA) {
    uint32_t msg = ARG(c, 0), result = 0;
    if (window_proc && rd32(msg + 4) != 0x0012 /* WM_QUIT is not dispatched */) {
        uint32_t argv[4] = {rd32(msg), rd32(msg + 4), rd32(msg + 8), rd32(msg + 12)};
        result = guest_invoke(c, window_proc, 4, argv);
    }
    ret_stdcall(c, result, 1);
}
API(TranslateMessage) { ret_stdcall(c, 0, 1); }
API(DefWindowProcA) { ret_stdcall(c, 0, 4); }
API(PostQuitMessage) { ret_stdcall(c, 0, 1); }
API(SendMessageA) { ret_stdcall(c, 0, 4); }
API(ShowCursor) { ret_stdcall(c, 0, 1); }
API(GetDC) { ret_stdcall(c, FAKE_HDC, 1); }
API(ReleaseDC) { ret_stdcall(c, 1, 2); }
API(OpenClipboard) { ret_stdcall(c, 0, 1); }
API(CloseClipboard) { ret_stdcall(c, 1, 0); }
API(GetClipboardData) { ret_stdcall(c, 0, 1); }
API(ToAscii) { ret_stdcall(c, 0, 5); }

/* wsprintfA(buffer, format, ...): cdecl with guest varargs. Supports the conversions
   wsprintf has (%d %i %u %x %X %c %s %%, flags '-' '0' '#', width, 'l'/'h'). */
API(wsprintfA) {
    uint32_t out = ARG(c, 0);
    const char *f = STR(ARG(c, 1));
    unsigned arg = 2;
    char buf[1025];
    size_t n = 0;
    while (*f && n < 1024) {
        if (*f != '%') { buf[n++] = *f++; continue; }
        char spec[16] = "%";
        size_t k = 1;
        f++;
        while (*f && strchr("-0#", *f) && k < 6) spec[k++] = *f++;
        while (*f >= '0' && *f <= '9' && k < 10) spec[k++] = *f++;
        while (*f == 'l' || *f == 'h') f++;
        char conv = *f ? *f++ : 0;
        char piece[512];
        if (conv == 's') {
            uint32_t s = ARG(c, arg++);
            spec[k++] = 's'; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, s ? STR(s) : "(null)");
        } else if (conv == 'c') {
            spec[k++] = 'c'; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, (int)(uint8_t)ARG(c, arg++));
        } else if (conv == 'd' || conv == 'i') {
            spec[k++] = 'd'; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, (int32_t)ARG(c, arg++));
        } else if (conv == 'u' || conv == 'x' || conv == 'X') {
            spec[k++] = conv; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, ARG(c, arg++));
        } else {
            snprintf(piece, sizeof piece, "%c", conv ? conv : '%');
        }
        for (const char *p = piece; *p && n < 1024; p++) buf[n++] = *p;
    }
    buf[n] = 0;
    memcpy(G(out), buf, n + 1);
    ret_cdecl(c, (uint32_t)n);
}

/* -- GDI32 ---------------------------------------------------------------------------- */
/* Fonts: the game asks GDI for MS Gothic glyphs (docs/FONT_RENDERING.md, T5); they
   come from Noto Sans Mono CJK JP through FreeType (glyph.c, T8). A font handle
   remembers height and weight; the DC's selected font picks the size. The game draws
   text from one thread at a time per DC, so a small table keyed by DC is enough. */
enum { FONT_BASE = 0x00040100u, MAX_FONTS = 256, MAX_DCS = 16 };
static struct { int height, weight; } fonts[MAX_FONTS];
static unsigned font_count;
static struct { uint32_t dc, font; } selected[MAX_DCS];
static pthread_mutex_t font_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t *dc_font(uint32_t dc) {
    for (unsigned i = 0; i < MAX_DCS; i++)
        if (selected[i].dc == dc) return &selected[i].font;
    for (unsigned i = 0; i < MAX_DCS; i++)
        if (!selected[i].dc) { selected[i].dc = dc; selected[i].font = 0; return &selected[i].font; }
    return &selected[0].font;
}
static int font_of(uint32_t handle, int *height, int *weight) {
    unsigned i = handle - FONT_BASE;
    if (handle < FONT_BASE || i >= font_count) return 0;
    *height = fonts[i].height;
    *weight = fonts[i].weight;
    return 1;
}

static int glyphs_ready(void) {
    static int state; /* 0 untried, 1 ok, -1 failed */
    if (state) return state > 0;
    char path[1200];
#ifdef T123_GBK
    const char *name = "NotoSansMonoCJKsc-Regular.otf", *env = getenv("T123_FONT"); /* simplified Chinese forms */
#else
    const char *name = "NotoSansMonoCJKjp-Regular.otf", *env = getenv("T123_FONT");
#endif
    const char *candidates[4] = {env, "%s/%s", "%s/../local/fonts/%s", "app0:assets/%s"}; /* last: the VPK's copy */
    state = -1;
    for (unsigned k = 0; k < 4 && state < 0; k++) {
        if (!candidates[k]) continue;
        if (k == 0) snprintf(path, sizeof path, "%s", env);
        else if (k == 3) snprintf(path, sizeof path, candidates[k], name);
        else snprintf(path, sizeof path, candidates[k], host_data_dir, name);
        if (glyph_init(path) == 0) state = 1;
    }
    trace(state > 0 ? "# glyphs: %s" : "# glyphs: no font found (set T123_FONT or copy %s next to the DAT files)",
          state > 0 ? path : name);
    return state > 0;
}

API(CreateFontA) {
    int height = (int)ARG(c, 0), weight = (int)ARG(c, 4);
    trace("CreateFontA(height=%d, weight=%d, charset=%u, face=%s)", height, weight,
          ARG(c, 8), ARG(c, 13) ? STR(ARG(c, 13)) : "");
    pthread_mutex_lock(&font_lock);
    uint32_t h = 0;
    if (font_count < MAX_FONTS) {
        fonts[font_count].height = height;
        fonts[font_count].weight = weight ? weight : 400;
        h = FONT_BASE + font_count++;
    }
    pthread_mutex_unlock(&font_lock);
    ret_stdcall(c, h, 14);
}
API(SelectObject) {
    uint32_t dc = ARG(c, 0), object = ARG(c, 1), previous = object;
    int hgt, wgt;
    if (font_of(object, &hgt, &wgt)) {
        pthread_mutex_lock(&font_lock);
        uint32_t *slot = dc_font(dc);
        previous = *slot ? *slot : 0x00040050u;
        *slot = object;
        pthread_mutex_unlock(&font_lock);
    }
    ret_stdcall(c, previous, 2);
}
API(DeleteObject) { ret_stdcall(c, 1, 1); }
API(GetStockObject) { ret_stdcall(c, 0x00040050u, 1); }

static void current_font(uint32_t dc, int *height, int *weight) {
    pthread_mutex_lock(&font_lock);
    uint32_t f = *dc_font(dc);
    pthread_mutex_unlock(&font_lock);
    if (!font_of(f, height, weight)) { *height = 16; *weight = 400; }
}

API(GetTextMetricsA) {
    /* TEXTMETRICA from the requested size (Noto: ascender 0.88 of the em) */
    int height, weight;
    current_font(ARG(c, 0), &height, &weight);
    int cell = height < 0 ? -height * 1448 / 1000 : height ? height : 16; /* em -> cell (1160+288)/1000 */
    int ascent = cell * 1160 / 1448;
    uint32_t tm = ARG(c, 1);
    memset(G(tm), 0, 56);
    wr32(tm + 0, (uint32_t)cell);
    wr32(tm + 4, (uint32_t)ascent);
    wr32(tm + 8, (uint32_t)(cell - ascent));
    wr32(tm + 12, (uint32_t)(height < 0 ? cell + height : 0)); /* internal leading */
    wr32(tm + 20, (uint32_t)((cell * 1000 / 1448) / 2));        /* half-width average */
    wr32(tm + 24, (uint32_t)(cell * 1000 / 1448));
    wr32(tm + 28, (uint32_t)weight);
    wr8(tm + 45, 0x20); wr8(tm + 46, 0xFC); wr8(tm + 47, 0x3F); wr8(tm + 48, 0x20); /* first/last/default/break */
#ifdef T123_GBK
    wr8(tm + 52, 134);                                           /* GB2312_CHARSET */
#else
    wr8(tm + 52, 128);                                           /* SHIFTJIS_CHARSET */
#endif
    ret_stdcall(c, 1, 2);
}
API(GetGlyphOutlineA) {
    uint32_t dc = ARG(c, 0), ch = ARG(c, 1), format = ARG(c, 2), gm = ARG(c, 3), size = ARG(c, 4), buf = ARG(c, 5);
    if (format != 5 /* GGO_GRAY4_BITMAP, the only one the game uses */ || !glyphs_ready()) {
        static int reported;
        if (!reported++) trace("GetGlyphOutlineA(format=%u): unsupported or no font", format);
        ret_stdcall(c, 0xFFFFFFFFu /* GDI_ERROR */, 7);
        return;
    }
    /* the A version takes the ANSI code page's code (Shift-JIS, or GBK with T123_GBK), lead byte in bits 8-15 */
    uint8_t mb[2];
    int n = 0;
    if (ch > 0xFF) mb[n++] = (uint8_t)(ch >> 8);
    mb[n++] = (uint8_t)ch;
    uint16_t wide = '?';
    sjis_to_utf16(mb, n, &wide, 1);
    int height, weight;
    current_font(dc, &height, &weight);
    GlyphMetrics m;
    uint32_t r = glyph_gray4(height, weight, wide, &m, size && buf ? (uint8_t *)G(buf) : NULL, size && buf ? size : 0);
    /* GDI reports blank glyphs (spaces) as a 1x1 box with no bits; the game divides
       the size by the box height (0x411e6a) and its loading thread died on 0x0. */
    if (r != 0xFFFFFFFFu && (!m.black_box_x || !m.black_box_y)) {
        m.black_box_x = m.black_box_y = 1;
        r = 0;
    }
    if (gm) {
        wr32(gm + 0, m.black_box_x); wr32(gm + 4, m.black_box_y);
        wr32(gm + 8, (uint32_t)m.origin_x); wr32(gm + 12, (uint32_t)m.origin_y);
        wr16(gm + 16, (uint16_t)m.cell_inc_x); wr16(gm + 18, (uint16_t)m.cell_inc_y);
    }
    ret_stdcall(c, r, 7);
}

/* -- IMM32 ----------------------------------------------------------------------------- */
/* IME: the game reads the finished text of a composition with
   ImmGetCompositionStringA(himc, GCS_RESULTSTR, buffer, size) when it gets
   WM_IME_ENDCOMPOSITION (0x40ead0). The Vita keyboard supplies that text here and then
   posts the message, which makes the game insert it at its cursor, whole and
   Shift-JIS-aware. Every other part of a composition is reported empty. */
enum { FAKE_HIMC = 0x00040040u };
static uint8_t ime_result[1024];
static unsigned ime_result_len;
void host_ime_set_result(const uint8_t *sjis, size_t n) {
    if (n > sizeof ime_result) n = sizeof ime_result;
    memcpy(ime_result, sjis, n);
    ime_result_len = (unsigned)n;
}
API(ImmGetContext) { ret_stdcall(c, FAKE_HIMC, 1); }
API(ImmReleaseContext) { ret_stdcall(c, 1, 2); }
API(ImmGetCompositionStringA) { /* (himc, index, buffer, size) */
    uint32_t index = ARG(c, 1), buf = ARG(c, 2), size = ARG(c, 3), n = 0;
    if (index == 0x800 /* GCS_RESULTSTR */) {
        n = ime_result_len;
        if (buf && size) {
            if (n > size) n = size;
            memcpy(G(buf), ime_result, n);
        }
    }
    ret_stdcall(c, n, 4);
}
API(ImmSetCandidateWindow) { ret_stdcall(c, 1, 2); }

/* -- OLE32 (CoCreateInstance is in media.c) ---------------------------------------- */
API(CoInitialize) { ret_stdcall(c, 0, 1); }
API(CoUninitialize) { ret_stdcall(c, 0, 0); }

/* -- WS2_32 --------------------------------------------------------------------------------
   Without adhoc.txt or wifi.txt (see net.h) every socket call fails with WSAENETDOWN, so
   the network menu reports an error instead of the process stopping on an unimplemented
   import. With one of them the calls go to net_*(), which on the Vita are SceNet sockets. */
enum { WS_BASE = 0x400, WSAENETDOWN = 10050, WSAENOTSOCK = 10038, WSAEINTR = 10004 };
static unsigned ws_error = WSAENETDOWN;
static int ws_socket(uint32_t s) { return s >= WS_BASE ? (int)(s - WS_BASE) : -1; }
API(WSAStartup) {
    memset(G(ARG(c, 1)), 0, 400);
    wr16(ARG(c, 1), 0x0202);
    wr16(ARG(c, 1) + 2, 0x0202);
    ret_stdcall(c, 0, 2);
}
API(WSACleanup) { ret_stdcall(c, 0, 0); }
API(WSAGetLastError) { ret_stdcall(c, ws_error, 0); }
API(socket) { /* (af, type, protocol) */
    if (!net_enabled()) { trace("socket(): networking is off (no adhoc.txt or wifi.txt)"); ws_error = WSAENETDOWN; ret_stdcall(c, 0xFFFFFFFFu, 3); return; }
    int s = net_open();
    if (s < 0) ws_error = WSAENETDOWN;
    ret_stdcall(c, s < 0 ? 0xFFFFFFFFu : (uint32_t)(WS_BASE + s), 3);
}
API(bind) { /* (s, sockaddr_in*, len): family, port (network order), address */
    int r = net_enabled() ? net_bind(ws_socket(ARG(c, 0)), rd16(ARG(c, 1) + 2)) : -1;
    if (r) ws_error = WSAENETDOWN;
    ret_stdcall(c, r ? 0xFFFFFFFFu : 0, 3);
}
API(closesocket) {
    if (net_enabled()) net_close(ws_socket(ARG(c, 0)));
    ret_stdcall(c, 0, 1);
}
API(shutdown) {
    if (net_enabled()) net_shutdown(ws_socket(ARG(c, 0)));
    ret_stdcall(c, 0, 2);
}
API(sendto) { /* (s, buf, len, flags, to, tolen) */
    int n = net_enabled() ? net_send(ws_socket(ARG(c, 0)), G(ARG(c, 1)), (int)ARG(c, 2), rd32(ARG(c, 4) + 4), rd16(ARG(c, 4) + 2)) : -1;
    if (n < 0) ws_error = WSAENETDOWN;
    ret_stdcall(c, n < 0 ? 0xFFFFFFFFu : (uint32_t)n, 6);
}
API(recvfrom) { /* (s, buf, len, flags, from, fromlen*) */
    uint32_t addr = 0;
    uint16_t port = 0;
    int n = net_enabled() ? net_recv(ws_socket(ARG(c, 0)), G(ARG(c, 1)), (int)ARG(c, 2), &addr, &port) : -1;
    if (n < 0) { ws_error = WSAEINTR; ret_stdcall(c, 0xFFFFFFFFu, 6); return; }
    if (ARG(c, 4)) {
        memset(G(ARG(c, 4)), 0, 16);
        wr16(ARG(c, 4), 2 /* AF_INET */);
        wr16(ARG(c, 4) + 2, port);
        wr32(ARG(c, 4) + 4, addr);
        if (ARG(c, 5)) wr32(ARG(c, 5), 16);
    }
    ret_stdcall(c, (uint32_t)n, 6);
}
API(gethostbyname) { ret_stdcall(c, 0, 1); }
static uint16_t swap16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }
API(htons) { ret_stdcall(c, swap16((uint16_t)ARG(c, 0)), 1); }
API(ntohs) { ret_stdcall(c, swap16((uint16_t)ARG(c, 0)), 1); }
API(htonl) { uint32_t v = ARG(c, 0); ret_stdcall(c, v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24, 1); }
API(inet_addr) {
    unsigned a, b, d, e;
    char tail;
    uint32_t r = sscanf(STR(ARG(c, 0)), "%u.%u.%u.%u%c", &a, &b, &d, &e, &tail) == 4 && (a | b | d | e) < 256
        ? a | b << 8 | d << 16 | e << 24 : 0xFFFFFFFFu; /* network order, INADDR_NONE */
    ret_stdcall(c, r, 1);
}


/* -- IMAGEHLP ---------------------------------------------------------------------------- */
/* The game saves a replay in replay\<players>\<date>\<time - ...>.rep and calls this with
   the directory part first; as a stub that did nothing, CreateFileA of the .rep always
   failed and no replay was ever saved. Every directory of the path is created (the part
   after the last separator is a file name and is left alone), as the real one does. */
API(MakeSureDirectoryPathExists) {
    char path[1200];
    const char *guest = STR(ARG(c, 0));
    path_to_host(guest, path, sizeof path);
    size_t start = strlen(host_data_dir) + 1;
    int ok = 1, made = 0;
    int first_errno = 0;
    for (char *q = path + (start < strlen(path) ? start : strlen(path)); *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        struct stat st;
        if (stat(path, &st) != 0) {
            if (MKDIR(path) == 0) made++;
            else if (stat(path, &st) != 0) { ok = 0; if (!first_errno) first_errno = errno; }
        }
        *q = '/';
    }
    trace("MakeSureDirectoryPathExists(%s) = %d (%d created%s%s)", guest, ok, made,
          ok ? "" : ", error: ", ok ? "" : strerror(first_errno));
    ret_stdcall(c, (uint32_t)ok, 1);
}

#define E(name) {#name, name}
const ApiEntry win32_other[] = {
    E(timeBeginPeriod), E(timeGetTime),
    E(RegisterClassExA), E(CreateWindowExA), E(ShowWindow), E(UpdateWindow), E(SetWindowPos),
    E(SetWindowTextA), E(GetWindowRect), E(GetWindowLongA), E(GetWindowInfo), E(SetRect),
    E(GetSystemMetrics), E(LoadCursorA), E(LoadIconA), E(LoadImageA), E(LoadStringA), E(MessageBoxA),
    E(PeekMessageA), E(GetMessageA), E(DispatchMessageA), E(TranslateMessage), E(DefWindowProcA),
    E(PostQuitMessage), E(SendMessageA), E(ShowCursor), E(GetDC), E(ReleaseDC), E(OpenClipboard),
    E(CloseClipboard), E(GetClipboardData), E(ToAscii), E(wsprintfA),
    E(CreateFontA), E(SelectObject), E(DeleteObject), E(GetStockObject), E(GetTextMetricsA),
    E(GetGlyphOutlineA),
    E(ImmGetContext), E(ImmReleaseContext), E(ImmGetCompositionStringA), E(ImmSetCandidateWindow),
    E(CoInitialize), E(CoUninitialize),
    E(WSAStartup), E(WSACleanup), E(WSAGetLastError), E(socket), E(bind), E(closesocket),
    E(shutdown), E(sendto), E(recvfrom), E(gethostbyname), E(htons), E(ntohs), E(htonl), E(inet_addr),
    E(MakeSureDirectoryPathExists),
    {NULL, NULL},
};
