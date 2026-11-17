// cn_shim.c: the C part of cnmalloc (row 425). The heap itself is the Cone package (src/cnmalloc.cone, object
// cnmalloc.obj); this file is what Cone could not say, each part a language gap named in the PR:
//   1. the one mutable global (the process's heap pointer), error 1626 in a C-named module;
//   2. the compare-and-swap lazy start (no atomics in a prelude-less module);
//   3. the __imp_ pointers (data symbols whose value is a function's address);
//   4. the exit-time statistics hook (atexit, only under CNMALLOC=stats).
// Everything else (the names malloc, calloc, ... included) could be exported from Cone; they are here too so the
// library has one place that decides what a standard-API call does. Nothing here calls malloc or anything that does.
//
// Build: /D_ACRTIMP= (else a C caller's malloc is dllimport and the swap silently does not happen), /MD.
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// the Cone package's exports (all take the heap's address first)
extern uintptr_t cnm_boot(void);
extern _Bool cnm_owns(uintptr_t h, uintptr_t p);
extern _Bool cnm_wants_stats(uintptr_t h);
extern void* cnm_malloc(uintptr_t h, size_t n);
extern void* cnm_calloc(uintptr_t h, size_t c, size_t n);
extern void* cnm_realloc(uintptr_t h, void* p, size_t n);
extern void cnm_free(uintptr_t h, void* p);
extern size_t cnm_usable(uintptr_t h, void* p);
extern void* cnm_expand(uintptr_t h, void* p, size_t n);
extern void* cnm_recalloc(uintptr_t h, void* p, size_t c, size_t n);
extern void* cnm_aligned(uintptr_t h, size_t align, size_t n);
extern void* cnm_aligned_realloc(uintptr_t h, void* p, size_t n, size_t align);
extern size_t cnm_aligned_msize(uintptr_t h, void* p);
extern void cnm_print_stats(uintptr_t h);
extern void cnm_print_lock_waits_of(uintptr_t h);
extern uint64_t cnm_collect(uintptr_t h, _Bool force);
extern uint64_t cnm_stats(uintptr_t h, uint64_t* out, size_t max);

static volatile uintptr_t g_heap;
static volatile LONG g_state;       // 0 not started, 1 starting, 2 ready
static int g_stats;

// CNMALLOC=stats: the counters are printed at exit. atexit is the exe's own CRT's and keeps its table in the C
// runtime's internal heap (not through the exported malloc this file replaces), so registering allocates nothing of ours.
static void __cdecl cn_at_exit(void) { if (g_stats && g_heap) cnm_print_stats(g_heap); }

static uintptr_t cn_start(void) {
  if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
    uintptr_t h = cnm_boot();
    if (!h) ExitProcess(3);   // nothing sane to do without a heap
    g_heap = h;
    g_stats = cnm_wants_stats(h) ? 1 : 0;   // CNMALLOC=stats
    if (g_stats) atexit(cn_at_exit);
    InterlockedExchange(&g_state, 2);
  } else {
    while (g_state != 2) YieldProcessor();
  }
  return g_heap;
}
#define H() (g_state == 2 ? g_heap : cn_start())

// ---- the C runtime's heap names, over the heap; foreign blocks are forwarded inside cnm_* ---------------------------
void* __cdecl malloc(size_t n) { return cnm_malloc(H(), n); }
void* __cdecl calloc(size_t c, size_t n) { return cnm_calloc(H(), c, n); }
void* __cdecl realloc(void* p, size_t n) { return cnm_realloc(H(), p, n); }
void __cdecl free(void* p) { if (p) cnm_free(H(), p); }
size_t __cdecl _msize(void* p) { return cnm_usable(H(), p); }
void* __cdecl _recalloc(void* p, size_t c, size_t n) { return cnm_recalloc(H(), p, c, n); }
void* __cdecl _expand(void* p, size_t n) { return cnm_expand(H(), p, n); }
void* __cdecl _aligned_malloc(size_t n, size_t a) { return cnm_aligned(H(), a, n); }
void __cdecl _aligned_free(void* p) { if (p) cnm_free(H(), p); }
void* __cdecl _aligned_realloc(void* p, size_t n, size_t a) { return cnm_aligned_realloc(H(), p, n, a); }
size_t __cdecl _aligned_msize(void* p, size_t a, size_t off) { (void)a; (void)off; return cnm_aligned_msize(H(), p); }

// Callers compiled with dllimport reference __imp_<name>, which pulls ucrt.lib's import member and collides with
// the definitions above (LNK2005). Defining the pointers keeps those members out and swaps those callers too.
void* (__cdecl* __imp_malloc)(size_t) = malloc;
void* (__cdecl* __imp_calloc)(size_t, size_t) = calloc;
void* (__cdecl* __imp_realloc)(void*, size_t) = realloc;
void  (__cdecl* __imp_free)(void*) = free;
size_t(__cdecl* __imp__msize)(void*) = _msize;
void* (__cdecl* __imp__recalloc)(void*, size_t, size_t) = _recalloc;
void* (__cdecl* __imp__expand)(void*, size_t) = _expand;
void* (__cdecl* __imp__aligned_malloc)(size_t, size_t) = _aligned_malloc;
void  (__cdecl* __imp__aligned_free)(void*) = _aligned_free;
void* (__cdecl* __imp__aligned_realloc)(void*, size_t, size_t) = _aligned_realloc;
size_t(__cdecl* __imp__aligned_msize)(void*, size_t, size_t) = _aligned_msize;

// what the benchmark runner calls at the end of a run (row 428): one line per contended class, then the total
void cn_print_lock_waits(void) { if (g_state == 2) cnm_print_lock_waits_of(g_heap); }

// for the process's own tools: give back what is due (force: everything), the counters
uint64_t cn_collect(int force) { return cnm_collect(H(), force); }
uint64_t cn_stats(uint64_t* out, size_t max) { return cnm_stats(H(), out, max); }
