/* The C side of module_c_byvalue_sysv: C functions taking and returning the
 * struct classes System V x86-64 tells apart (mixed integer and float
 * eightbytes, floats only, nested, over 16 bytes) and running out of argument
 * registers, and C calling Cone's C-named functions the same way. Compiled by
 * the platform's C compiler, so every call across meets C's own convention. */

#include <stdint.h>

typedef struct { int32_t a, b; } Ext2;           /* 8 bytes, INTEGER */
typedef struct { float x, y; } Vec2;             /* 8 bytes, two floats: one SSE eightbyte */
typedef struct { float x, y, z; } Vec3;          /* 12 bytes: SSE, SSE */
typedef struct { int32_t a; double d; } IntDbl;  /* 16 bytes: INTEGER, SSE */
typedef struct { double d; int64_t n; } DblInt;  /* 16 bytes: SSE, INTEGER */
typedef struct { double a, b; } DblDbl;          /* 16 bytes: SSE, SSE */
typedef struct { float f; int32_t n; } FltInt;   /* 8 bytes: a float and an integer share an eightbyte, INTEGER */
typedef struct { Vec2 v; int32_t k; float w; } Nest;  /* 16 bytes: SSE, INTEGER */
typedef struct { int16_t s[5]; } Short5;         /* 10 bytes: INTEGER, INTEGER */
typedef struct { int64_t a, b, c; } Big3;        /* 24 bytes: memory */
typedef struct { double x[5]; } Big5;            /* 40 bytes: memory */
typedef struct { float x, y, z, w; } Vec4;       /* 16 bytes: SSE, SSE */
typedef struct { int64_t lo, hi; } Big;          /* 16 bytes: INTEGER, INTEGER */

/* Arguments */
int64_t intdbl_code(IntDbl v) { return v.a * 1000 + (int64_t)(v.d * 4); }
int64_t dblint_code(DblInt v) { return (int64_t)(v.d * 4) * 1000 + v.n; }
int64_t dbldbl_code(DblDbl v) { return (int64_t)(v.a * 4) * 1000 + (int64_t)(v.b * 4); }
int64_t fltint_code(FltInt v) { return (int64_t)(v.f * 4) * 1000 + v.n; }
int64_t nest_code(Nest v) { return (int64_t)(v.v.x + v.v.y * 10) * 100000 + v.k * 100 + (int64_t)v.w; }
int64_t short5_code(Short5 v) { return v.s[0] + v.s[1] * 10 + v.s[2] * 100 + v.s[3] * 1000 + v.s[4] * 10000; }
int64_t big3_code(Big3 v) { return v.a * 10000 + v.b * 100 + v.c; }
int64_t vec4_code(Vec4 v) { return (int64_t)(v.x * 1000 + v.y * 100 + v.z * 10 + v.w); }
int64_t big5_code(Big5 v) { return (int64_t)(v.x[0] + v.x[1] * 10 + v.x[2] * 100 + v.x[3] * 1000 + v.x[4] * 10000); }

/* Several structs among scalars: each digit of the result is one argument's */
int64_t among(Big3 g, IntDbl i, Vec3 v, DblDbl dd, int32_t k) {
    int64_t n = g.a;
    n = n * 10 + g.b;
    n = n * 10 + g.c;
    n = n * 10 + i.a;
    n = n * 10 + (int64_t)i.d;
    n = n * 10 + (int64_t)v.x;
    n = n * 10 + (int64_t)v.y;
    n = n * 10 + (int64_t)v.z;
    n = n * 10 + (int64_t)dd.a;
    n = n * 10 + (int64_t)dd.b;
    return n * 10 + k;
}

/* Running out of argument registers (six integer, eight SSE). A 16-byte
 * struct needing two integer registers when one is left goes to the stack
 * whole; an 8-byte one goes there as a plain integer when none is left. */
int64_t six_then_ext(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, Ext2 s) {
    int64_t n = a;
    n = n * 10 + b; n = n * 10 + c; n = n * 10 + d; n = n * 10 + e; n = n * 10 + f;
    return n * 1000 + s.a * 10 + s.b;
}
int64_t five_then_big(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, Big g) {
    int64_t n = a;
    n = n * 10 + b; n = n * 10 + c; n = n * 10 + d; n = n * 10 + e;
    return n * 1000 + g.lo * 10 + g.hi;
}
int64_t eight_then_vec2(double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8, Vec2 v) {
    int64_t n = (int64_t)d1;
    n = n * 10 + (int64_t)d2; n = n * 10 + (int64_t)d3; n = n * 10 + (int64_t)d4;
    n = n * 10 + (int64_t)d5; n = n * 10 + (int64_t)d6; n = n * 10 + (int64_t)d7; n = n * 10 + (int64_t)d8;
    return n * 100 + (int64_t)v.x * 10 + (int64_t)v.y;
}
int64_t seven_ints_dbldbl(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, DblDbl p, int32_t f, int32_t g) {
    int64_t n = a;
    n = n * 10 + b; n = n * 10 + c; n = n * 10 + d; n = n * 10 + e;
    n = n * 10 + (int64_t)p.a; n = n * 10 + (int64_t)p.b;
    return (n * 10 + f) * 10 + g;
}

/* Returns */
IntDbl intdbl_make(int32_t n) { IntDbl v = { n, n * 0.25 }; return v; }
DblInt dblint_make(int32_t n) { DblInt v = { n * 0.25, n + 1 }; return v; }
DblDbl dbldbl_make(int32_t n) { DblDbl v = { n * 0.25, n * 0.5 }; return v; }
FltInt fltint_make(int32_t n) { FltInt v = { n * 0.25f, n + 2 }; return v; }
Nest nest_make(int32_t n) { Nest v = { { (float)n, (float)(n + 1) }, n + 2, (float)(n + 3) }; return v; }
Short5 short5_make(int32_t n) { Short5 v = { { (int16_t)n, (int16_t)(n + 1), (int16_t)(n + 2), (int16_t)(n + 3), (int16_t)(n + 4) } }; return v; }
Big3 big3_make(int32_t n) { Big3 v = { n, n + 1, n + 2 }; return v; }
Big5 big5_make(int32_t n) { Big5 v = { { n, n + 1, n + 2, n + 3, n + 4 } }; return v; }

/* A result slot takes the first integer register, so the sixth integer
 * argument here (a struct of 8 bytes) finds none left */
Big3 sret_then_ext(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, Ext2 s) {
    Big3 v = { a + b + c + d + e, s.a, s.b };
    return v;
}

/* Cone's C-named functions, which C calls */
int64_t cone_intdbl(IntDbl v);
DblInt cone_dblint(DblInt v, int32_t k);
Nest cone_nest(Nest v);
Big3 cone_big3(Big3 v, int32_t k);
Vec4 cone_vec4(Vec4 v, float k);
int64_t cone_among(Big3 g, IntDbl i, Vec3 v, DblDbl dd, int32_t k);
int64_t cone_six_then_ext(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, Ext2 s);
int64_t cone_five_then_big(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, Big g);
Big3 cone_sret_then_ext(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, Ext2 s);

int64_t c_via_cone_intdbl(void) { IntDbl v = { 3, 1.5 }; return cone_intdbl(v); }
int64_t c_via_cone_dblint(void) {
    DblInt v = { 2.5, 7 };
    DblInt r = cone_dblint(v, 10);
    return (int64_t)(r.d * 4) * 1000 + r.n;
}
int64_t c_via_cone_nest(void) {
    Nest v = { { 1, 2 }, 3, 4 };
    Nest r = cone_nest(v);
    return (int64_t)(r.v.x + r.v.y * 10) * 100000 + r.k * 100 + (int64_t)r.w;
}
int64_t c_via_cone_big3(void) {
    Big3 v = { 1, 2, 3 };
    Big3 r = cone_big3(v, 10);
    return r.a * 10000 + r.b * 100 + r.c;
}
int64_t c_via_cone_vec4(void) {
    Vec4 v = { 1, 2, 3, 4 };
    Vec4 r = cone_vec4(v, 2);
    return (int64_t)(r.x * 1000 + r.y * 100 + r.z * 10 + r.w);
}
int64_t c_via_cone_among(void) {
    Big3 g = { 1, 2, 3 };
    IntDbl i = { 4, 5 };
    Vec3 v = { 6, 7, 8 };
    DblDbl dd = { 9, 1 };
    return cone_among(g, i, v, dd, 2);
}
int64_t c_via_cone_six(void) { Ext2 s = { 7, 8 }; return cone_six_then_ext(1, 2, 3, 4, 5, 6, s); }
int64_t c_via_cone_five(void) { Big g = { 6, 7 }; return cone_five_then_big(1, 2, 3, 4, 5, g); }
int64_t c_via_cone_sret_then(void) {
    Ext2 s = { 6, 7 };
    Big3 r = cone_sret_then_ext(1, 2, 3, 4, 5, s);
    return r.a * 10000 + r.b * 100 + r.c;
}

/* A callback Cone hands C, called with a struct by value and returning one */
typedef int64_t (*IntDblFn)(IntDbl);
int64_t c_apply_intdbl(int32_t a, IntDblFn f) { IntDbl v = { a, 2.5 }; return f(v); }
typedef Big3 (*Big3Fn)(Big3, int32_t);
int64_t c_apply_big3(Big3Fn f) {
    Big3 v = { 4, 5, 6 };
    Big3 r = f(v, 3);
    return r.a * 10000 + r.b * 100 + r.c;
}
