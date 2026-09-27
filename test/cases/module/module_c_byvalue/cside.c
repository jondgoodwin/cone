/* The C side of module_c_byvalue: C functions taking and returning structs by
 * value, and C calling Cone's C-named functions the same way. Compiled by the
 * platform's C compiler, so every call across meets C's own convention. */

#include <stdint.h>

typedef struct { uint8_t r, g, b, a; } Rgba;   /* 4 bytes */
typedef struct { uint8_t r, g, b; } Rgb;       /* 3 bytes */
typedef struct { int32_t a, b; } Ext2;         /* 8 bytes */
typedef struct { float x, y; } Vec2;           /* 8 bytes, floats */
typedef struct { double d; } D1;               /* 8 bytes, one double */
typedef struct { float x, y, z; } Vec3;        /* 12 bytes */
typedef struct { float x, y, z, w; } Vec4;     /* 16 bytes */
typedef struct { int64_t lo, hi; } Big;        /* 16 bytes */

/* Arguments */
int32_t rgba_code(Rgba c) { return c.r * 1000 + c.g * 100 + c.b * 10 + c.a; }
int32_t rgb_code(Rgb c) { return c.r * 100 + c.g * 10 + c.b; }
int32_t ext_code(Ext2 e) { return e.a * 1000 + e.b; }
int32_t vec2_code(Vec2 v) { return (int32_t)(v.x * 10 + v.y); }
int32_t d1_code(D1 v) { return (int32_t)(v.d * 4); }
int32_t vec3_code(Vec3 v) { return (int32_t)(v.x * 100 + v.y * 10 + v.z); }
int32_t vec4_code(Vec4 v) { return (int32_t)(v.x * 1000 + v.y * 100 + v.z * 10 + v.w); }
int64_t big_code(Big b) { return b.lo * 1000 + b.hi; }

/* Structs among scalars: each digit of the result is one argument's */
int64_t mixed(int32_t k, Ext2 e, double s, Vec3 v, int8_t last) {
    int64_t n = k;
    n = n * 10 + e.a;
    n = n * 10 + e.b;
    n = n * 10 + (int64_t)s;
    n = n * 10 + (int64_t)v.x;
    n = n * 10 + (int64_t)v.y;
    n = n * 10 + (int64_t)v.z;
    return n * 10 + last;
}

/* Returns */
Rgba rgba_make(int32_t n) { Rgba c = { (uint8_t)n, (uint8_t)(n + 1), (uint8_t)(n + 2), (uint8_t)(n + 3) }; return c; }
Rgb rgb_make(int32_t n) { Rgb c = { (uint8_t)n, (uint8_t)(n + 1), (uint8_t)(n + 2) }; return c; }
Ext2 ext_make(int32_t n) { Ext2 e = { n, n + 100 }; return e; }
Vec2 vec2_make(float f) { Vec2 v = { f, 2 * f }; return v; }
Vec3 vec3_make(float f) { Vec3 v = { f, 2 * f, 3 * f }; return v; }
Big big_make(int64_t n) { Big b = { n, -n }; return b; }

/* Cone's C-named functions, which C calls */
int32_t cone_ext(Ext2 e);
Vec3 cone_vec3(Vec3 v, float k);
Ext2 cone_ext_make(int32_t n);

int32_t c_via_cone_ext(void) { Ext2 e = { 3, 4 }; return cone_ext(e); }
int32_t c_via_cone_vec3(void) {
    Vec3 v = { 1, 2, 3 };
    Vec3 r = cone_vec3(v, 10);
    return (int32_t)(r.x * 100 + r.y * 10 + r.z);
}
int32_t c_via_cone_ext_make(void) { Ext2 m = cone_ext_make(7); return m.a * 1000 + m.b; }

/* A callback Cone hands C, called with a struct by value */
typedef int32_t (*ExtFn)(Ext2);
int32_t c_apply(int32_t a, int32_t b, ExtFn f) { Ext2 e = { a, b }; return f(e); }
