/* The C side of module_c_byvalue_arm64: C functions taking and returning the
 * structs arm64 Apple's C ABI tells apart (homogeneous float aggregates of up
 * to four members, nested, in an array and over 16 bytes; five floats, which
 * are not one; small integer structs), chars and shorts the caller widens, and
 * arguments that run out of registers and are packed on the stack; and C
 * calling Cone's C-named functions the same way. Compiled by the platform's C
 * compiler, so every call across meets C's own convention. */

#include <stdint.h>

typedef struct { float x, y; } Vec2;              /* 8 bytes: [2 x float] */
typedef struct { Vec2 lo, hi; } Quad;             /* nested: [4 x float] */
typedef struct { float v[4]; } Arr4;              /* an array: [4 x float] */
typedef struct { double a, b, c; } Dbl3;          /* 24 bytes: [3 x double] */
typedef struct { double a, b, c, d; } Dbl4;       /* 32 bytes: [4 x double] */
typedef struct { float a, b, c, d, e; } Flt5;     /* 20 bytes, five floats: memory */
typedef struct { uint8_t r, g, b; } Rgb;          /* 3 bytes: i64, returned i24 */
typedef struct { int32_t a, b, c; } Int3;         /* 12 bytes: [2 x i64] */

/* Arguments */
int64_t quad_code(Quad q) { return (int64_t)(q.lo.x * 1000 + q.lo.y * 100 + q.hi.x * 10 + q.hi.y); }
int64_t arr4_code(Arr4 a) { return (int64_t)(a.v[0] * 1000 + a.v[1] * 100 + a.v[2] * 10 + a.v[3]); }
int64_t dbl3_code(Dbl3 v) { return (int64_t)(v.a * 100 + v.b * 10 + v.c); }
int64_t dbl4_code(Dbl4 v) { return (int64_t)(v.a * 1000 + v.b * 100 + v.c * 10 + v.d); }
int64_t flt5_code(Flt5 v) { return (int64_t)(v.a * 10000 + v.b * 1000 + v.c * 100 + v.d * 10 + v.e); }
int64_t rgb_code(Rgb c) { return c.r * 100 + c.g * 10 + c.b; }
int64_t int3_code(Int3 v) { return v.a * 100 + v.b * 10 + v.c; }
int64_t narrow_code(int8_t a, uint8_t b, int16_t c, uint16_t d) { return ((int64_t)a * 1000 + b) * 100000 + (int64_t)c * 100000 + d; }

/* The registers run out: eight doubles fill the SIMD registers, so the Dbl4
 * goes on the stack whole; eight integers fill the integer registers, so the
 * char, the short and the Int3 that follow go on the stack, which Apple packs
 * by their natural size and alignment */
int64_t eight_then_dbl4(double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8, Dbl4 v) {
    return (int64_t)(d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8) * 10000 + dbl4_code(v);
}
int64_t eight_then_narrow(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h,
        int8_t i, int16_t j, uint8_t k, Int3 v) {
    return (a + b + c + d + e + f + g + h) * 100000000 + (int64_t)i * 1000000 + (int64_t)j * 1000 + k * 100 + int3_code(v) % 100;
}

/* Results */
Quad quad_make(int32_t n) { Quad q = { { n, n + 1 }, { n + 2, n + 3 } }; return q; }
Arr4 arr4_make(int32_t n) { Arr4 a = { { n, n + 1, n + 2, n + 3 } }; return a; }
Dbl3 dbl3_make(int32_t n) { Dbl3 v = { n, n * 2, n * 3 }; return v; }
Dbl4 dbl4_make(int32_t n) { Dbl4 v = { n, n + 1, n + 2, n + 3 }; return v; }
Flt5 flt5_make(int32_t n) { Flt5 v = { n, n + 1, n + 2, n + 3, n + 4 }; return v; }
Rgb rgb_make(int32_t n) { Rgb c = { n, n + 1, n + 2 }; return c; }
Int3 int3_make(int32_t n) { Int3 v = { n, n + 1, n + 2 }; return v; }
int8_t narrow_neg(int8_t a) { return (int8_t)-a; }
uint16_t narrow_twice(uint16_t a) { return (uint16_t)(a * 2); }

/* Cone's C-named functions, called from C */
Dbl4 cone_dbl4(Dbl4 v, double k);
Quad cone_quad(Quad q);
Flt5 cone_flt5(Flt5 v);
Rgb cone_rgb(Rgb c);
int16_t cone_narrow(int8_t a, uint8_t b, int16_t c);
int64_t cone_eight_then_narrow(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h,
        int8_t i, int16_t j, uint8_t k, Int3 v);

int64_t c_via_cone_dbl4(void) { Dbl4 v = { 1, 2, 3, 4 }; return dbl4_code(cone_dbl4(v, 2)); }
int64_t c_via_cone_quad(void) { Quad q = { { 1, 2 }, { 3, 4 } }; return quad_code(cone_quad(q)); }
int64_t c_via_cone_flt5(void) { Flt5 v = { 1, 2, 3, 4, 5 }; return flt5_code(cone_flt5(v)); }
int64_t c_via_cone_rgb(void) { Rgb c = { 1, 2, 3 }; return rgb_code(cone_rgb(c)); }
int64_t c_via_cone_narrow(void) { return cone_narrow(-5, 200, -300); }
int64_t c_via_cone_eight(void) { Int3 v = { 1, 2, 3 }; return cone_eight_then_narrow(1, 1, 1, 1, 1, 1, 1, 1, -7, -42, 9, v); }

/* A callback Cone hands C: C builds the struct itself and calls it */
typedef Dbl4 (*Dbl4Fn)(Dbl4, double);
int64_t c_apply_dbl4(Dbl4Fn f) { Dbl4 v = { 4, 3, 2, 1 }; return dbl4_code(f(v, 1)); }
typedef Quad (*QuadFn)(Quad);
int64_t c_apply_quad(QuadFn f) { Quad q = { { 5, 6 }, { 7, 8 } }; return quad_code(f(q)); }
