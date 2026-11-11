/* Guest CPU state and helpers used by C code generated from th123.exe.
 *
 * The guest is 32-bit x86. Guest addresses are 32-bit values; the host reaches
 * them through guest_base (G()). Integer flags are computed eagerly into single
 * bytes. The x87 FPU keeps an explicit control word: precision control (24/53/64
 * significand bits) and rounding control are applied by the runtime on every
 * operation instead of relying on the host FPU mode.
 */
#ifndef T123_GUEST_H
#define T123_GUEST_H

#include <float.h>
#include <setjmp.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
typedef long double x87_t;   /* 64-bit significand on x86 hosts */
#define X87_HAS_EXTENDED 1
#else
typedef double x87_t;        /* ARM: PC=64 degrades to 53 bits (documented) */
#define X87_HAS_EXTENDED 0
#endif

typedef union {
    uint32_t d;
    uint16_t w;
    struct { uint8_t l, h; } b;
} GuestReg;

typedef struct {
    x87_t st[8];
    uint16_t cw;   /* control word: PC bits 8-9, RC bits 10-11 */
    uint16_t sw;   /* status word without TOP; C0 bit 8, C1 9, C2 10, C3 14 */
    uint8_t top;
    uint8_t tag_empty; /* bitmask of empty physical registers */
    uint8_t host_rc;   /* RC the host FPU of this thread is set to (guest.c); it fits in
                          the padding, so the layout seen by generated code is unchanged */
} X87;

typedef struct Cpu {
    GuestReg eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, zf, sf, of, pf, df;
    uint8_t id_flag; /* EFLAGS.ID, toggled by the CRT to probe for cpuid */
    uint32_t fs_base;
    X87 fpu;
    /* Address of the call being dispatched (read by the fake-import and COM dispatchers).
       It lives here, not in GuestThread: on the Vita guest_thread() is pthread_once +
       pthread_getspecific, and guest_call stored it on every indirect call. */
    uint32_t current_target;
    /* Last guest function entered (one store per call; read by the sampling profiler
       of recomp/host/vprof.c when prof.txt exists, ignored otherwise). */
    volatile uint32_t cur_fn;
} Cpu;

/* Fixed Windows user-mode selectors, for code that stores segment registers. */
enum { SEL_CS = 0x1B, SEL_DS = 0x23, SEL_ES = 0x23, SEL_SS = 0x23, SEL_FS = 0x3B, SEL_GS = 0x00 };

typedef void (*GuestFn)(Cpu *c);

/* Debug builds (-DT123_TRACE_CALLS) remember the last guest functions entered, so a
   crash can be placed in the original code. */
/* Per-thread runtime state. VitaSDK's _Thread_local gives every thread the same
   address (measured in Vita3K), so the Vita keeps it behind a pthread key. */
typedef struct {
    uint32_t current_target;   /* address of the call being dispatched */
    uint32_t trace_ring[64];
    unsigned trace_pos;
    jmp_buf *fault_trap;
    char fault_text[256];      /* last guest_fault, for reports */
} GuestThread;
#ifdef __vita__
GuestThread *guest_thread(void);
#else
extern _Thread_local GuestThread guest_thread_state;
#define guest_thread() (&guest_thread_state)
#endif
#define guest_trace_ring (guest_thread()->trace_ring)
#define guest_trace_pos (guest_thread()->trace_pos)
#define guest_fault_trap (guest_thread()->fault_trap)
#define guest_fault_text (guest_thread()->fault_text)
#ifdef T123_TRACE_CALLS
#define T123_ENTER(a) (guest_trace_ring[guest_trace_pos++ & 63] = (a))
#else
#define T123_ENTER(a) (c->cur_fn = (a))
#endif

/* -- memory ------------------------------------------------------------------------ */
/* Guest addresses below GUEST_WINDOW are backed by host memory. On PC the host
   reserves the whole 4 GiB range (so stray accesses fault); on the Vita it is a
   single block of GUEST_WINDOW bytes. */
#define GUEST_WINDOW 0x08000000u
extern uint8_t *guest_base;
#define G(a) (guest_base + (uint32_t)(a))

int guest_mem_init(void);
extern int guest_mem_error; /* platform error code when guest_mem_init fails */
int guest_map(uint32_t address, uint32_t size);
int guest_load_pe(const char *path); /* copies headers and sections at ImageBase */
extern uint32_t guest_image_base, guest_image_size;
/* Set by the host before the game starts: 1 = wide 960x544 fight view. Only code lifted with
   recomp/lift_all.py --wide reads it; elsewhere it changes nothing. */
extern int t123_wide;

static inline uint8_t rd8(uint32_t a) { return *G(a); }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline void wr8(uint32_t a, uint8_t v) { *G(a) = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(G(a), &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(G(a), &v, 8); }

static inline void push32(Cpu *c, uint32_t v) { c->esp.d -= 4; wr32(c->esp.d, v); }
static inline uint32_t pop32(Cpu *c) { uint32_t v = rd32(c->esp.d); c->esp.d += 4; return v; }

/* -- calls ------------------------------------------------------------------------- */
typedef struct { uint32_t address; GuestFn fn; } GuestEntry;

void guest_register(const GuestEntry *table, unsigned count); /* lifted functions */
void guest_intercept(uint32_t address, GuestFn fn);           /* host replacements */
void guest_call(Cpu *c, uint32_t target);  /* return address already pushed */
void guest_call_import(Cpu *c, uint32_t iat_slot);
/* Host -> guest call (callbacks such as window procedures and thread entries):
   pushes argv right to left and a sentinel return address, returns eax. */
#define GUEST_RETURN_SENTINEL 0xFFFFFFF0u
uint32_t guest_invoke(Cpu *c, uint32_t target, unsigned argc, const uint32_t *argv);
/* Reports and exits, or longjmps to guest_fault_trap when a test harness set one. */
_Noreturn void guest_fault(Cpu *c, uint32_t address, const char *what);

/* -- integer flags ------------------------------------------------------------------ */
static inline uint8_t parity8(uint32_t r) { return (uint8_t)!__builtin_parity(r & 0xFF); }

#define T123_DEFINE_FLAGS(N, T, ST, MSB)                                                    \
static inline void flags_logic##N(Cpu *c, T r) {                                            \
    c->cf = 0; c->of = 0; c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r);     \
}                                                                                            \
static inline T op_add##N(Cpu *c, T a, T b) {                                                \
    T r = (T)(a + b);                                                                        \
    c->cf = r < a; c->of = (((a ^ r) & (b ^ r)) >> (MSB)) & 1;                               \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_adc##N(Cpu *c, T a, T b) {                                                \
    T carry = c->cf; T r = (T)(a + b + carry);                                               \
    c->cf = carry ? r <= a : r < a; c->of = (((a ^ r) & (b ^ r)) >> (MSB)) & 1;               \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_sub##N(Cpu *c, T a, T b) {                                                \
    T r = (T)(a - b);                                                                        \
    c->cf = a < b; c->of = (((a ^ b) & (a ^ r)) >> (MSB)) & 1;                               \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_sbb##N(Cpu *c, T a, T b) {                                                \
    T borrow = c->cf; T r = (T)(a - b - borrow);                                             \
    c->cf = borrow ? a <= b : a < b; c->of = (((a ^ b) & (a ^ r)) >> (MSB)) & 1;             \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_inc##N(Cpu *c, T a) {                                                     \
    T r = (T)(a + 1); c->of = r == (T)((T)1 << (MSB));                                       \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_dec##N(Cpu *c, T a) {                                                     \
    T r = (T)(a - 1); c->of = a == (T)((T)1 << (MSB));                                       \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_neg##N(Cpu *c, T a) {                                                     \
    T r = (T)(0 - a); c->cf = a != 0; c->of = a == (T)((T)1 << (MSB));                       \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_shl##N(Cpu *c, T a, uint8_t n) {                                          \
    n &= 31; if (!n) return a;                                                               \
    T r = n > (MSB) ? 0 : (T)(a << n);                                                       \
    c->cf = n <= (MSB) + 1 ? (a >> ((MSB) + 1 - n)) & 1 : 0;                                 \
    c->of = ((r >> (MSB)) & 1) ^ c->cf;                                                      \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_shr##N(Cpu *c, T a, uint8_t n) {                                          \
    n &= 31; if (!n) return a;                                                               \
    T r = n > (MSB) ? 0 : (T)(a >> n);                                                       \
    c->cf = n <= (MSB) + 1 ? (a >> (n - 1)) & 1 : 0;                                         \
    c->of = (a >> (MSB)) & 1;                                                                \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}                                                                                            \
static inline T op_sar##N(Cpu *c, T a, uint8_t n) {                                          \
    n &= 31; if (!n) return a;                                                               \
    ST s = (ST)a; uint8_t k = n > (MSB) ? (MSB) : n;                                         \
    T r = (T)(s >> k);                                                                       \
    c->cf = (uint8_t)(((ST)a >> (n > (MSB) ? (MSB) : n - 1)) & 1);                           \
    c->of = 0;                                                                               \
    c->zf = r == 0; c->sf = (r >> (MSB)) & 1; c->pf = parity8(r); return r;                  \
}

T123_DEFINE_FLAGS(8, uint8_t, int8_t, 7)
T123_DEFINE_FLAGS(16, uint16_t, int16_t, 15)
T123_DEFINE_FLAGS(32, uint32_t, int32_t, 31)

#define T123_DEFINE_ROTATES(N, T, BITS)                                                      \
static inline T op_rol##N(Cpu *c, T a, uint8_t n) {                                          \
    n &= 31; if (!n) return a;                                                               \
    uint8_t k = n % (BITS); T r = k ? (T)((a << k) | (a >> ((BITS) - k))) : a;               \
    c->cf = r & 1; c->of = ((r >> ((BITS) - 1)) & 1) ^ c->cf; return r;                      \
}                                                                                            \
static inline T op_ror##N(Cpu *c, T a, uint8_t n) {                                          \
    n &= 31; if (!n) return a;                                                               \
    uint8_t k = n % (BITS); T r = k ? (T)((a >> k) | (a << ((BITS) - k))) : a;               \
    c->cf = (r >> ((BITS) - 1)) & 1;                                                         \
    c->of = ((r >> ((BITS) - 1)) ^ (r >> ((BITS) - 2))) & 1; return r;                       \
}                                                                                            \
static inline T op_rcl##N(Cpu *c, T a, uint8_t n) {                                          \
    n = (uint8_t)((n & 31) % ((BITS) + 1)); if (!n) return a;                                \
    for (uint8_t i = 0; i < n; i++) {                                                        \
        uint8_t out = (a >> ((BITS) - 1)) & 1; a = (T)((a << 1) | c->cf); c->cf = out;       \
    }                                                                                        \
    c->of = ((a >> ((BITS) - 1)) & 1) ^ c->cf; return a;                                     \
}                                                                                            \
static inline T op_rcr##N(Cpu *c, T a, uint8_t n) {                                          \
    n = (uint8_t)((n & 31) % ((BITS) + 1)); if (!n) return a;                                \
    c->of = ((a >> ((BITS) - 1)) & 1) ^ c->cf; /* defined for n == 1 */                      \
    for (uint8_t i = 0; i < n; i++) {                                                        \
        uint8_t out = a & 1; a = (T)((a >> 1) | ((T)c->cf << ((BITS) - 1))); c->cf = out;    \
    }                                                                                        \
    return a;                                                                                \
}

T123_DEFINE_ROTATES(8, uint8_t, 8)
T123_DEFINE_ROTATES(16, uint16_t, 16)
T123_DEFINE_ROTATES(32, uint32_t, 32)

/* shld/shrd r/m32, r32, count. */
static inline uint32_t op_shld32(Cpu *c, uint32_t a, uint32_t b, uint8_t n) {
    n &= 31; if (!n) return a;
    uint32_t r = (a << n) | (b >> (32 - n));
    c->cf = (a >> (32 - n)) & 1; c->of = ((r ^ a) >> 31) & 1;
    c->zf = r == 0; c->sf = r >> 31; c->pf = parity8(r); return r;
}
static inline uint32_t op_shrd32(Cpu *c, uint32_t a, uint32_t b, uint8_t n) {
    n &= 31; if (!n) return a;
    uint32_t r = (a >> n) | (b << (32 - n));
    c->cf = (a >> (n - 1)) & 1; c->of = ((r ^ a) >> 31) & 1;
    c->zf = r == 0; c->sf = r >> 31; c->pf = parity8(r); return r;
}

/* imul r/m8: ax = al * src (signed). */
static inline void op_imul1_8(Cpu *c, uint8_t b) {
    int16_t p = (int16_t)((int8_t)c->eax.b.l * (int8_t)b);
    c->eax.w = (uint16_t)p;
    c->cf = c->of = p != (int8_t)p;
    c->zf = (uint8_t)p == 0; c->sf = ((uint16_t)p >> 7) & 1; c->pf = parity8((uint8_t)p);
}

/* xadd: dst = dst + src, src = old dst; `lock` makes the memory form atomic. */
static inline uint32_t op_xadd32_mem(Cpu *c, uint32_t ea, uint32_t src) {
    uint32_t old = __atomic_fetch_add((uint32_t *)G(ea), src, __ATOMIC_SEQ_CST);
    (void)op_add32(c, old, src);
    return old;
}

/* EFLAGS image for pushfd: CF PF ZF SF DF OF, IF and reserved bit 1 set, ID kept. */
static inline uint32_t get_eflags(Cpu *c) {
    return (uint32_t)c->cf | 2u | (uint32_t)c->pf << 2 | (uint32_t)c->zf << 6 |
           (uint32_t)c->sf << 7 | 1u << 9 | (uint32_t)c->df << 10 | (uint32_t)c->of << 11 |
           (uint32_t)c->id_flag << 21;
}
static inline void set_eflags(Cpu *c, uint32_t f) {
    c->cf = f & 1; c->pf = (f >> 2) & 1; c->zf = (f >> 6) & 1; c->sf = (f >> 7) & 1;
    c->df = (f >> 10) & 1; c->of = (f >> 11) & 1; c->id_flag = (f >> 21) & 1;
}

/* bt/bts with a register bit offset: for memory operands the offset addresses a
   bit string (signed, may leave the dword). */
static inline uint32_t bt_address(uint32_t ea, uint32_t offset) {
    return ea + (uint32_t)(((int32_t)offset >> 5) * 4);
}

void op_cpuid(Cpu *c);
void op_scas(Cpu *c, unsigned size, int rep, uint32_t address);  /* rep: 0, 1 repe, 2 repne */
void op_cmps(Cpu *c, unsigned size, int rep, uint32_t address);

/* imul r, r/m(, imm): truncated product; CF=OF when the signed result overflowed. */
static inline uint32_t op_imul32(Cpu *c, uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int32_t)b;
    uint32_t r = (uint32_t)p;
    c->cf = c->of = p != (int32_t)r;
    c->zf = r == 0; c->sf = r >> 31; c->pf = parity8(r); return r;
}
static inline uint16_t op_imul16(Cpu *c, uint16_t a, uint16_t b) {
    int32_t p = (int32_t)(int16_t)a * (int16_t)b;
    uint16_t r = (uint16_t)p;
    c->cf = c->of = p != (int16_t)r;
    c->zf = r == 0; c->sf = r >> 15; c->pf = parity8(r); return r;
}
/* One-operand forms on edx:eax. */
static inline void op_imul1_32(Cpu *c, uint32_t b) {
    int64_t p = (int64_t)(int32_t)c->eax.d * (int32_t)b;
    c->eax.d = (uint32_t)p; c->edx.d = (uint32_t)((uint64_t)p >> 32);
    c->cf = c->of = p != (int32_t)(uint32_t)p;
    c->zf = c->eax.d == 0; c->sf = c->eax.d >> 31; c->pf = parity8(c->eax.d);
}
static inline void op_mul1_32(Cpu *c, uint32_t b) {
    uint64_t p = (uint64_t)c->eax.d * b;
    c->eax.d = (uint32_t)p; c->edx.d = (uint32_t)(p >> 32);
    c->cf = c->of = c->edx.d != 0;
    c->zf = c->eax.d == 0; c->sf = c->eax.d >> 31; c->pf = parity8(c->eax.d);
}
void op_div1_32(Cpu *c, uint32_t b, uint32_t address);
void op_idiv1_32(Cpu *c, uint32_t b, uint32_t address);

/* Condition codes. */
#define CC_O(c)  ((c)->of)
#define CC_NO(c) (!(c)->of)
#define CC_B(c)  ((c)->cf)
#define CC_AE(c) (!(c)->cf)
#define CC_E(c)  ((c)->zf)
#define CC_NE(c) (!(c)->zf)
#define CC_BE(c) ((c)->cf | (c)->zf)
#define CC_A(c)  (!((c)->cf | (c)->zf))
#define CC_S(c)  ((c)->sf)
#define CC_NS(c) (!(c)->sf)
#define CC_P(c)  ((c)->pf)
#define CC_NP(c) (!(c)->pf)
#define CC_L(c)  ((c)->sf != (c)->of)
#define CC_GE(c) ((c)->sf == (c)->of)
#define CC_LE(c) ((c)->zf | ((c)->sf != (c)->of))
#define CC_G(c)  (!(c)->zf && (c)->sf == (c)->of)

/* sahf: SF ZF AF PF CF from AH (AF is not modelled). */
static inline void op_sahf(Cpu *c) {
    uint8_t ah = c->eax.b.h;
    c->sf = (ah >> 7) & 1; c->zf = (ah >> 6) & 1; c->pf = (ah >> 2) & 1; c->cf = ah & 1;
}

/* String operations; DF=1 steps backwards. */
void op_rep_movs(Cpu *c, unsigned size, int rep, uint32_t address);
void op_rep_stos(Cpu *c, unsigned size, int rep, uint32_t address);

/* -- x87 ----------------------------------------------------------------------------- */
enum { X87_ADD, X87_SUB, X87_SUBR, X87_MUL, X87_DIV, X87_DIVR };

void x87_reset(Cpu *c);                     /* fninit: CW 0x037F, empty stack */
void x87_set_cw(Cpu *c, uint16_t cw);       /* fldcw */
static inline uint16_t x87_get_cw(Cpu *c) { return c->fpu.cw; }
static inline uint16_t x87_get_sw(Cpu *c) { /* fnstsw, with TOP */
    return (uint16_t)((c->fpu.sw & ~0x3800) | ((c->fpu.top & 7) << 11));
}

/* The register stack accessors are inline: every x87 instruction uses them, and as
   calls into guest.c they were a large share of the profile. */
static inline x87_t x87_st(Cpu *c, unsigned i) { return c->fpu.st[(c->fpu.top + i) & 7]; }
static inline void x87_set_st(Cpu *c, unsigned i, x87_t v) {
    unsigned p = (c->fpu.top + i) & 7;
    c->fpu.st[p] = v;
    c->fpu.tag_empty &= (uint8_t)~(1u << p);
}
static inline void x87_push(Cpu *c, x87_t v) {
    c->fpu.top = (c->fpu.top - 1) & 7;
    unsigned p = c->fpu.top;
    if (!(c->fpu.tag_empty & (1u << p))) {
        c->fpu.sw |= 0x0241; /* stack fault: IE, SF, C1=1 (overflow) */
        v = (x87_t)NAN;      /* masked response: indefinite */
    }
    c->fpu.st[p] = v;
    c->fpu.tag_empty &= (uint8_t)~(1u << p);
}
static inline void x87_pop(Cpu *c) {
    c->fpu.tag_empty |= (uint8_t)(1u << c->fpu.top);
    c->fpu.top = (c->fpu.top + 1) & 7;
}

/* Hot x87 instructions are inline, with a fast path for the state the game is nearly
   always in: rounding to nearest (RC=0) with the host FPU in the same mode, and for
   arithmetic 24-bit precision (PC=0, what D3D9 sets). Every other state goes to the
   complete out-of-line version (*_slow in guest.c), so the results are the same as
   before; only the call, the mode tests and libm calls are gone from the common case.
   The calls were ~30 % of the active time of the fight thread (PC profile). */
#define X87_NEAREST(c)    (((c)->fpu.cw & 0x0C00) == 0 && (c)->fpu.host_rc == 0)
#define X87_NEAREST_24(c) (((c)->fpu.cw & 0x0F00) == 0 && (c)->fpu.host_rc == 0)

static inline x87_t x87_m32(uint32_t a) { float f; memcpy(&f, G(a), 4); return f; }
static inline x87_t x87_m64(uint32_t a) { double d; memcpy(&d, G(a), 8); return d; }
static inline void x87_fld_m32(Cpu *c, uint32_t a) { x87_push(c, x87_m32(a)); }
static inline void x87_fld_m64(Cpu *c, uint32_t a) { x87_push(c, x87_m64(a)); }
static inline void x87_fild(Cpu *c, int64_t v) { x87_push(c, (x87_t)v); }
void x87_fld_const_slow(Cpu *c, int which);
static inline void x87_fld_const(Cpu *c, int which) { /* 0 zero, 1 one, 2 pi, 3 l2e, 4 l2t, 5 lg2, 6 ln2 */
    if (which == 0) x87_push(c, 0.0);
    else if (which == 1) x87_push(c, 1.0);
    else x87_fld_const_slow(c, which);
}
void x87_fld_m80(Cpu *c, uint32_t a);
void x87_fld_st(Cpu *c, unsigned i);
void x87_fst_m32_slow(Cpu *c, uint32_t a, int pop);
void x87_fst_m64_slow(Cpu *c, uint32_t a, int pop);
static inline void x87_fst_m32_inl(Cpu *c, uint32_t a, int pop) {
    if (__builtin_expect(X87_NEAREST(c), 1)) {
        float f = (float)x87_st(c, 0); /* what to_float_rc gives under RC=0 */
        memcpy(G(a), &f, 4);
        if (pop) x87_pop(c);
    } else {
        x87_fst_m32_slow(c, a, pop);
    }
}
static inline void x87_fst_m64_inl(Cpu *c, uint32_t a, int pop) {
    if (__builtin_expect(X87_NEAREST(c), 1)) {
        double d = (double)x87_st(c, 0);
        memcpy(G(a), &d, 8);
        if (pop) x87_pop(c);
    } else {
        x87_fst_m64_slow(c, a, pop);
    }
}
void x87_fst_m80(Cpu *c, uint32_t a, int pop);
static inline void x87_fst_st(Cpu *c, unsigned i, int pop) {
    x87_set_st(c, i, x87_st(c, 0));
    if (pop) x87_pop(c);
}
void x87_fist(Cpu *c, uint32_t a, unsigned size, int pop, int truncate);
void x87_arith_slow(Cpu *c, int op, unsigned dst, x87_t src, int pop);
double x87_round24_slow(Cpu *c, double v);
/* st(dst) = st(dst) op src */
static inline void x87_arith_inl(Cpu *c, int op, unsigned dst, x87_t src, int pop) {
    if (__builtin_expect(X87_NEAREST_24(c), 1)) {
        double da = (double)x87_st(c, dst), db = (double)src, r;
        switch (op) {
        case X87_ADD: r = da + db; break;
        case X87_SUB: r = da - db; break;
        case X87_SUBR: r = db - da; break;
        case X87_MUL: r = da * db; break;
        case X87_DIV: r = da / db; break;
        default: r = db / da; break;
        }
        /* round to 24 bits keeping the extended exponent: inside the normal float range,
           or zero/infinity/NaN, that is a plain conversion to float under RC=0 */
        double av = fabs(r);
        if (__builtin_expect((av >= (double)FLT_MIN && av <= (double)FLT_MAX) || av == 0 || !(av <= DBL_MAX), 1))
            r = (double)(float)r;
        else
            r = x87_round24_slow(c, r);
        x87_set_st(c, dst, r);
        if (pop) x87_pop(c);
    } else {
        x87_arith_slow(c, op, dst, src, pop);
    }
}
/* Variant switch. With -DT123_X87_INLINE_BIG the three bodies above are expanded at each
   of the ~230 000 call sites of the generated code (the Vita text grew from 18.8 to
   26.5 MiB, and its build from 10 to 58 minutes). Without it they are ordinary functions
   in guest.c that run the same fast path: one call per instruction, and a code size close
   to the original on a CPU with a 32 KiB instruction cache. */
#ifdef T123_X87_INLINE_BIG
#define x87_fst_m32 x87_fst_m32_inl
#define x87_fst_m64 x87_fst_m64_inl
#define x87_arith x87_arith_inl
#else
void x87_fst_m32(Cpu *c, uint32_t a, int pop);
void x87_fst_m64(Cpu *c, uint32_t a, int pop);
void x87_arith(Cpu *c, int op, unsigned dst, x87_t src, int pop);
#endif
void x87_compare(Cpu *c, x87_t b, int pops, int unordered_ok);   /* st0 vs b -> C0 C2 C3 */
void x87_comi(Cpu *c, unsigned i, int pop);                      /* fcomi/fucomi -> EFLAGS */
void x87_fxch(Cpu *c, unsigned i);
void x87_unary(Cpu *c, int which); /* 0 chs 1 abs 2 sqrt 3 rndint 4 sin 5 cos 6 tst 7 xam */
void x87_binary_special(Cpu *c, int which); /* 0 fpatan 1 fyl2x 2 fscale 3 fprem 4 f2xm1 5 fptan 6 fprem1 */
void x87_fnstenv(Cpu *c, uint32_t a);
void x87_fldenv(Cpu *c, uint32_t a);
void x87_fnsave(Cpu *c, uint32_t a);
void x87_frstor(Cpu *c, uint32_t a);
x87_t x87_round_pc(Cpu *c, x87_t v);        /* apply PC with the current RC */

#endif
