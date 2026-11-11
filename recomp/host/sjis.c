#include "sjis.h"
#include <limits.h>

#ifdef T123_GBK
/* Chinese build: the game's text (the 汉化 data) is CP936/GBK, so the "ANSI code page"
   these conversions stand for is 936 instead of 932 (kernel32.c reports the same). */
#include "gbk_table.h"
static int lead(uint8_t b) { return b >= 0x81 && b <= 0xfe; }
static uint16_t single(uint8_t b) {
    if (b < 0x80) return b;
    return b == 0x80 ? 0x20ac : 0x30fb;
}
static uint16_t pair(uint8_t a, uint8_t b) {
    if (b < 0x40 || b == 0x7f || b == 0xff) return 0x30fb;
    uint16_t u = gbk_decode[(a - 0x81u) * 191u + (b - 0x40u)];
    return u ? u : 0x30fb;
}
static uint16_t encode(uint16_t u, int *fallback) {
    if (u < 0x80) return u;
    if (u == 0x20ac) return 0x80;
    size_t lo = 0, hi = sizeof gbk_encode / sizeof gbk_encode[0];
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        if (gbk_encode[mid].unicode < u) lo = mid+1; else hi = mid;
    }
    if (lo < sizeof gbk_encode / sizeof gbk_encode[0] && gbk_encode[lo].unicode == u)
        return gbk_encode[lo].bytes;
    *fallback = 1;
    return '?';
}
#else
#include "sjis_table.h"
static int lead(uint8_t b) {
    return (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
}
static uint16_t single(uint8_t b) {
    if (b <= 0x80) return b;
    if (b == 0xa0) return 0xf8f0;
    if (b >= 0xa1 && b <= 0xdf) return (uint16_t)(0xff61 + b - 0xa1);
    if (b >= 0xfd) return (uint16_t)(0xf8f1 + b - 0xfd);
    return 0x30fb;
}
static uint16_t pair(uint8_t a, uint8_t b) {
    if (b < 0x40 || b == 0x7f || b > 0xfc) return 0x30fb;
    unsigned row = a <= 0x9f ? a - 0x81u : a - 0xe0u + 31u;
    unsigned col = b <= 0x7e ? b - 0x40u : b - 0x80u + 63u;
    uint16_t u = sjis_decode[row * 188u + col];
    return u ? u : 0x30fb;
}
static uint16_t encode(uint16_t u, int *fallback) {
    if (u <= 0x80) return u;
    if (u >= 0xff61 && u <= 0xff9f) return (uint16_t)(0xa1 + u - 0xff61);
    if (u == 0xf8f0) return 0xa0;
    if (u >= 0xf8f1 && u <= 0xf8f3) return (uint16_t)(0xfd + u - 0xf8f1);
    size_t lo = 0, hi = sizeof sjis_encode / sizeof sjis_encode[0];
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        if (sjis_encode[mid].unicode < u) lo = mid+1; else hi = mid;
    }
    if (lo < sizeof sjis_encode / sizeof sjis_encode[0] && sjis_encode[lo].unicode == u)
        return sjis_encode[lo].bytes;
    *fallback = 1;
    return '?';
}
#endif
int sjis_to_utf16(const uint8_t *in, int in_len, uint16_t *out, int out_len) {
    if (!in || !in_len || out_len < 0) return 0;
    if (in_len < 0) {
        size_t n=0;
        while (in[n]) { if (n >= INT_MAX-1u) return 0; ++n; }
        in_len=(int)n+1;
    }
    int count=0;
    for (int i=0;i<in_len;++i) {
        if (lead(in[i]) && i+1<in_len && in[i+1]) ++i;
        ++count;
    }
    if (!out || !out_len) return count;
    if (out_len < count) return 0;
    int n=0;
    for (int i=0;i<in_len;++i) {
        uint8_t a=in[i]; uint16_t u=single(a);
        if (lead(a) && i+1<in_len && in[i+1]) u=pair(a,in[++i]);
        out[n++]=u;
    }
    return count;
}
int utf16_to_sjis(const uint16_t *in, int in_len, uint8_t *out, int out_len,
                  int *used_default) {
    if (used_default) *used_default=0;
    if (!in || !in_len || out_len<0) return 0;
    if (in_len<0) {
        size_t n=0;
        while (in[n]) { if (n >= INT_MAX-1u) return 0; ++n; }
        in_len=(int)n+1;
    }
    int count=0, fallback=0;
    for (int i=0;i<in_len;++i) {
        uint16_t b=encode(in[i],&fallback);
        int width=b>255 ? 2 : 1;
        if (count>INT_MAX-width) return 0;
        count+=width;
    }
    if (used_default) *used_default=fallback;
    if (!out || !out_len) return count;
    if (out_len<count) return 0;
    int n=0;
    for (int i=0;i<in_len;++i) {
        uint16_t b=encode(in[i],&fallback);
        if (b>255) out[n++]=(uint8_t)(b>>8);
        out[n++]=(uint8_t)b;
    }
    return count;
}
