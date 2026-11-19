// cn_plat.h: the platform layer of cn_shim.c (row 550). The shim's text is one source for Windows (MSVC) and Linux (GCC or
// Clang); this header is the only place the two differ. Everything the shim shares between threads, the lock-free paths'
// words above all, is read and written through the forms below, never through `volatile`:
//
//   MSVC (x64, /volatile:ms, the default): each form is the volatile access or the _Interlocked intrinsic the shim used
//   before this header, so the compiler's code for the common paths is unchanged. (A volatile load is an acquire load and
//   a volatile store a release store under /volatile:ms; the interlocked intrinsics are full barriers.)
//
//   GCC and Clang: each form is the explicit __atomic builtin with the ordering written in its name. GCC gives `volatile` no
//   ordering against other accesses, so a volatile word would let the compiler move a load across the fetch_or it must follow
//   (the silent failure the PR's table of the volatile sites exists to rule out). On x64 an acquire load, a release store and
//   a relaxed access are the plain mov; every read-modify-write is the same locked instruction either way.
//
// The orderings are those of WI\cnmalloc-second-design-spec.md, section 11:
//   _acq   an acquire load: the allocator's load of a slot's page, of genclass (g1 and g2), of a word of bits
//   _sc    a sequentially consistent load: the freer's load of `state` after its fetch_or (the detacher's side of the
//          Dekker rule is the Cone code's, all seq_cst)
//   _rel   a release store: init flags published last (g_cap, g_fast, g_lf, g_heap, after what they guard)
//   _rlx   a relaxed access: counters, hints, a word only its owner changes
//   cn_cas*/cn_or64/cn_add*/cn_inc*/cn_xchg32  read-modify-writes, sequentially consistent (as _Interlocked* are on x64)
// x64 only, as row 533 says: another processor stays out.
#ifndef CN_PLAT_H
#define CN_PLAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #if !defined(_M_X64)
    #error "cnmalloc: x64 only"
  #endif
  #include <windows.h>
  #include <intrin.h>

  #define CN_WINDOWS 1
  #define CN_TLS __declspec(thread)
  #define CN_ALIGN64 __declspec(align(64))
  #define CN_FORCEINLINE __forceinline
  #define CN_NOINLINE __declspec(noinline)
  #define CN_CALL __cdecl
  #define CN_EXPORT __declspec(dllexport)
  #define CN_UNLIKELY(x) (x)
  // what an allocation returns: Windows' C runtime sets errno itself where it matters, and this shim never did
  #define CN_RET_MEM(e) return (e)
  #define CN_RET_MEMN(e, n) return (e)

  // loads and stores of the shared words: the volatile access of /volatile:ms
  // (macros, not functions: the compiler sees the same expression the shim spelled before this header, so its code for the
  // common paths does not move; measured, an inline function here cost the live row 3%)
  #define cn_ld8_rlx(p) (*(const volatile uint8_t*)(p))
  #define cn_ld32_acq(p) (*(const volatile uint32_t*)(p))
  #define cn_ld32_rlx(p) (*(const volatile uint32_t*)(p))
  #define cn_ld64_acq(p) (*(const volatile uint64_t*)(p))
  #define cn_ld64_rlx(p) (*(const volatile uint64_t*)(p))
  #define cn_ld64_sc(p) (*(const volatile uint64_t*)(p))
  #define cn_st8_rlx(p, v) (*(volatile uint8_t*)(p) = (v))
  #define cn_st32_rlx(p, v) (*(volatile uint32_t*)(p) = (v))
  #define cn_st64_rel(p, v) (*(volatile uint64_t*)(p) = (v))
  #define cn_st64_rlx(p, v) (*(volatile uint64_t*)(p) = (v))
  #define cn_add64_rlx(p, v) (*(volatile uint64_t*)(p) += (v))
  // a flag the shim sets once, before it publishes the heap, and reads before it knows the heap is ready (g_cap, g_fast):
  // today a plain read and write, and so it stays
  #define CN_FLAG_LD(x) (x)
  #define CN_FLAG_ST(x, v) ((x) = (v))

  #define cn_add64(p, v) ((uint64_t)_InterlockedExchangeAdd64((volatile LONG64*)(p), (LONG64)(v)))
  #define cn_inc64(p) ((uint64_t)_InterlockedIncrement64((volatile LONG64*)(p)))
  #define cn_inc32(p) ((uint32_t)_InterlockedIncrement((volatile LONG*)(p)))
  #define cn_or64(p, v) ((uint64_t)_InterlockedOr64((volatile LONG64*)(p), (LONG64)(v)))
  // the old value; the swap is made when *p == expected
  #define cn_cas64(p, expected, desired) ((uint64_t)_InterlockedCompareExchange64((volatile LONG64*)(p), (LONG64)(desired), (LONG64)(expected)))
  static __forceinline uint32_t cn_cas32(uint32_t* p, uint32_t expected, uint32_t desired) {
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)p, (LONG)desired, (LONG)expected);
  }
  static __forceinline uint32_t cn_xchg32(uint32_t* p, uint32_t v) { return (uint32_t)InterlockedExchange((volatile LONG*)p, (LONG)v); }
  static __forceinline void cn_fence(void) { MemoryBarrier(); }
  static __forceinline void cn_pause(void) { YieldProcessor(); }
  static __forceinline unsigned cn_bsf64(uint64_t x) { unsigned long b; _BitScanForward64(&b, x); return (unsigned)b; }
  static __forceinline unsigned cn_bsr64(uint64_t x) { unsigned long b; _BitScanReverse64(&b, x); return (unsigned)b; }
  static __forceinline uint64_t cn_popcnt64(uint64_t x) { return __popcnt64(x); }
  static __forceinline void cn_write_err(const char* s, size_t n) {
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)n, &w, NULL);
  }
  static __forceinline void cn_exit(int code) { ExitProcess((UINT)code); }

#elif defined(__linux__)
  #if !defined(__x86_64__)
    #error "cnmalloc: x64 only"
  #endif
  #include <errno.h>
  #include <unistd.h>

  #define CN_LINUX 1
  // the initial-exec model: no __tls_get_addr, so no lazily allocated TLS block, so no malloc from inside malloc. The
  // shim is an object on the executable's link line, never a shared library (build-cn.sh; README, "Linking it")
  #define CN_TLS __thread __attribute__((tls_model("initial-exec")))
  #define CN_ALIGN64 __attribute__((aligned(64)))
  #define CN_FORCEINLINE inline __attribute__((always_inline))
  #define CN_NOINLINE __attribute__((noinline))
  #define CN_CALL
  #define CN_EXPORT __attribute__((visibility("default")))
  #define CN_UNLIKELY(x) __builtin_expect(!!(x), 0)
  // what an allocation returns: the C library's allocators set errno to ENOMEM when they answer null (CN_RET_MEMN: but
  // not for a zero-size realloc, whose null is a success)
  #define CN_RET_MEM(e) do { void* cn_r = (e); if (CN_UNLIKELY(!cn_r)) errno = ENOMEM; return cn_r; } while (0)
  #define CN_RET_MEMN(e, n) do { void* cn_r = (e); if (CN_UNLIKELY(!cn_r) && (n)) errno = ENOMEM; return cn_r; } while (0)

  #define CN_ACCESS(N, T) \
    static CN_FORCEINLINE T cn_ld##N##_acq(const T* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); } \
    static CN_FORCEINLINE T cn_ld##N##_rlx(const T* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); } \
    static CN_FORCEINLINE T cn_ld##N##_sc(const T* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); } \
    static CN_FORCEINLINE void cn_st##N##_rel(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); } \
    static CN_FORCEINLINE void cn_st##N##_rlx(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); } \
    static CN_FORCEINLINE void cn_add##N##_rlx(T* p, T v) { __atomic_store_n(p, (T)(__atomic_load_n(p, __ATOMIC_RELAXED) + v), __ATOMIC_RELAXED); }
  CN_ACCESS(8, uint8_t)
  CN_ACCESS(32, uint32_t)
  CN_ACCESS(64, uint64_t)
  #undef CN_ACCESS
  #define CN_FLAG_LD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
  #define CN_FLAG_ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

  static CN_FORCEINLINE uint64_t cn_add64(uint64_t* p, uint64_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
  static CN_FORCEINLINE uint64_t cn_inc64(uint64_t* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
  static CN_FORCEINLINE uint32_t cn_inc32(uint32_t* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
  static CN_FORCEINLINE uint64_t cn_or64(uint64_t* p, uint64_t v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
  static CN_FORCEINLINE uint64_t cn_cas64(uint64_t* p, uint64_t expected, uint64_t desired) {
    __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;                         // the old value: unchanged when the swap was made, what *p held when it was not
  }
  static CN_FORCEINLINE uint32_t cn_cas32(uint32_t* p, uint32_t expected, uint32_t desired) {
    __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
  }
  static CN_FORCEINLINE uint32_t cn_xchg32(uint32_t* p, uint32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
  static CN_FORCEINLINE void cn_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
  static CN_FORCEINLINE void cn_pause(void) { __builtin_ia32_pause(); }
  static CN_FORCEINLINE unsigned cn_bsf64(uint64_t x) { return (unsigned)__builtin_ctzll(x); }
  static CN_FORCEINLINE unsigned cn_bsr64(uint64_t x) { return 63u - (unsigned)__builtin_clzll(x); }
  static CN_FORCEINLINE uint64_t cn_popcnt64(uint64_t x) { return (uint64_t)__builtin_popcountll(x); }
  static CN_FORCEINLINE void cn_write_err(const char* s, size_t n) { ssize_t r = write(2, s, n); (void)r; }
  // _exit, not exit: exit runs the C library's atexit handlers and destructors, which free
  static CN_FORCEINLINE void cn_exit(int code) { _exit(code); }
#else
  #error "cnmalloc: no platform layer for this system (Windows and Linux only)"
#endif

#endif
