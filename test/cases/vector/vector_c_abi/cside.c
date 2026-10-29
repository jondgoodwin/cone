/* The C side of vector_c_abi: SIMD values passed and returned by value as the
   platform's C compiler passes its own __m128, __m128i and __m128d, in both
   directions. On Win64 C passes a 128-bit vector through a pointer to a copy and
   returns it in XMM0; on System V it passes and returns it in XMM registers. The
   Cone side declares the same functions with Vector[f32, 4], Vector[i32, 4] and
   Vector[f64, 2], so that what matches is the ABI and not a reading of it. */
#include <emmintrin.h>

#if defined(_MSC_VER)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

/* Cone calling C */
EXPORT __m128 cv_add(__m128 a, __m128 b) { return _mm_add_ps(a, b); }

EXPORT float cv_sum(__m128 v) {
    float t[4];
    _mm_storeu_ps(t, v);
    return t[0] + t[1] + t[2] + t[3];
}

EXPORT __m128i cv_iadd(__m128i a, __m128i b) { return _mm_add_epi32(a, b); }

EXPORT __m128d cv_dscale(__m128d v, double k) { return _mm_mul_pd(v, _mm_set1_pd(k)); }

/* several vectors and scalars among the registers, in an order that mixes them */
EXPORT float cv_mixed(int n, __m128 a, double d, __m128 b, float f) {
    float t[4], u[4];
    _mm_storeu_ps(t, a);
    _mm_storeu_ps(u, b);
    return (float)n + t[0] + u[3] + (float)d + f;
}

/* C calling Cone's own C-named functions */
extern __m128 cone_scale(__m128 v, float k);
extern __m128i cone_ibump(__m128i v);

EXPORT float c_via_cone_scale(void) {
    float t[4];
    __m128 r = cone_scale(_mm_set_ps(4.0f, 3.0f, 2.0f, 1.0f), 2.0f);
    _mm_storeu_ps(t, r);
    return t[0] + t[1] + t[2] + t[3];
}

EXPORT int c_via_cone_ibump(void) {
    int t[4];
    __m128i r = cone_ibump(_mm_set_epi32(40, 30, 20, 10));
    _mm_storeu_si128((__m128i *)t, r);
    return t[0] * 1000 + t[1] * 100 + t[2] * 10 + t[3] - 3000;
}
