// cn_shim.c: the C part of cnmalloc (row 425). The heap itself is the Cone package (src/cnmalloc.cone, object
// cnmalloc.obj); this file is what Cone could not say, each part a language gap named in the PR:
//   1. the one mutable global (the process's heap pointer), error 1626 in a C-named module;
//   2. the compare-and-swap lazy start (no atomics in a prelude-less module);
//   3. the __imp_ pointers (data symbols whose value is a function's address);
//   4. the exit-time statistics hook (atexit, only under CNMALLOC=stats);
//   5. (row 426) the workers' cache: a C thread-local, the one thread-local in cnmalloc (see "THE WORKERS' CACHE").
// Everything else (the names malloc, calloc, ... included) could be exported from Cone; they are here too so the
// library has one place that decides what a standard-API call does. Nothing here calls malloc or anything that does.
//
// Build: /D_ACRTIMP= (else a C caller's malloc is dllimport and the swap silently does not happen), /MD.
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
extern size_t cnm_aligned_msize(uintptr_t h, void* p, size_t align, size_t off);
extern void cnm_aligned_free(uintptr_t h, void* p);
extern void cnm_print_stats(uintptr_t h);
extern void cnm_print_lock_waits_of(uintptr_t h);
extern uint64_t cnm_collect(uintptr_t h, _Bool force);
extern uint64_t cnm_stats(uintptr_t h, uint64_t* out, size_t max);
// row 426: what the workers' cache needs of the heap (all in src/cnmalloc.cone)
extern size_t cnm_cache_cap(uintptr_t h);
extern size_t cnm_cache_max(uintptr_t h);
extern size_t cnm_class_of(uintptr_t h, size_t n);
extern size_t cnm_class_size(uintptr_t h, size_t cl);
extern size_t cnm_cell_class(uintptr_t h, void* p);
extern void* cnm_batch_alloc(uintptr_t h, size_t cl, size_t n, size_t* got);
extern void cnm_batch_free(uintptr_t h, size_t cl, void* head, size_t n);
extern void cnm_set_cache_stats(uintptr_t h, uint64_t bytes, uint64_t hits, uint64_t misses, uint64_t returns, uint64_t fills);

static volatile uintptr_t g_heap;
static volatile LONG g_state;       // 0 not started, 1 starting, 2 ready
static int g_stats;

// ---- THE WORKERS' CACHE (row 426) -------------------------------------------------------------------------------
// Only a thread the runtime has declared a worker (cnmalloc_workerBegin) has one; every other thread goes straight
// to the shared heap. Per size class up to CNMALLOC's cache_max (default 1024 bytes) a worker keeps a short LIFO
// list of freed cells, linked through their first words, and its count. CNMALLOC=cache=<n> is the cap (0, the
// default, turns the cache off: g_cap is 0 and no thread-local is ever read or written).
//   free(p), small, on a worker: push p, no lock. Over the cap, return the older half of the list to the heap under
//     one take of the class lock (cnm_batch_free).
//   allocate(c), on a worker: pop. Empty: take the class lock once and fill to half the cap (cnm_batch_alloc).
// WHY THE PAGE COUNTS STAY EXACT: a cell in a cache is still "used" as far as the heap is concerned. It left the
// heap through takeCell (its page's used count + 1, under the class lock) and it comes back only through putCell
// (used - 1, under the class lock, and the page retires there if that was its last live cell). Nothing in this file
// touches a page's used count, free list or bit, so a page holding a cached cell has used >= 1 and cannot retire;
// retiring is exactly row 425's, under the class lock. The cost is that a page can stay un-retired while its cells
// sit in a cache: at most cap cells per class per worker, which is the bounded slack.
// A cached cell is dirty (it has been written, and its first word is the link), so a calloc from the cache clears
// the whole cell. A thread that ends without cnmalloc_workerEnd keeps its cells counted as used for good.
#define CN_MAXCLS 72
#define CN_SLOTS 512
#define CN_MAXREQ 32768
typedef struct { volatile int64_t bytes, hits, misses, returns, fills; int64_t pad[3]; } CnSlot;   // one cache line
typedef struct { void* head[CN_MAXCLS]; uint16_t count[CN_MAXCLS]; CnSlot* slot; } CnTls;
static __declspec(thread) CnTls t_c;
static __declspec(align(64)) CnSlot g_slots[CN_SLOTS];
static volatile LONG g_slotUsed[CN_SLOTS];
static volatile LONG64 g_endHits, g_endMisses, g_endReturns, g_endFills;   // workers that have ended
static size_t g_cap, g_max, g_ncached, g_fill;
static size_t g_csize[CN_MAXCLS];
static uint8_t g_cls[CN_MAXREQ / 16 + 2];       // (n + 15) >> 4 -> class, for n up to cache_max

// push the totals to the heap's counters (before they are read or printed)
static void cn_publish(void) {
  if (!g_cap) return;
  int64_t b = 0, h = g_endHits, m = g_endMisses, r = g_endReturns, f = g_endFills;
  for (int i = 0; i < CN_SLOTS; i++) {
    CnSlot* s = &g_slots[i];
    b += s->bytes; h += s->hits; m += s->misses; r += s->returns; f += s->fills;
  }
  cnm_set_cache_stats(g_heap, (uint64_t)b, (uint64_t)h, (uint64_t)m, (uint64_t)r, (uint64_t)f);
}

// CNMALLOC=stats: the counters are printed at exit. atexit is the exe's own CRT's and keeps its table in the C
// runtime's internal heap (not through the exported malloc this file replaces), so registering allocates nothing of ours.
static void __cdecl cn_at_exit(void) { if (g_stats && g_heap) { cn_publish(); cnm_print_stats(g_heap); } }

static uintptr_t cn_start(void) {
  if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
    uintptr_t h = cnm_boot();
    if (!h) ExitProcess(3);   // nothing sane to do without a heap
    g_heap = h;
    g_stats = cnm_wants_stats(h) ? 1 : 0;   // CNMALLOC=stats
    if (g_stats) atexit(cn_at_exit);
    size_t cap = cnm_cache_cap(h), mx = cnm_cache_max(h);
    if (cap && mx) {
      for (size_t i = 0; i <= (mx + 15) >> 4; i++) g_cls[i] = (uint8_t)cnm_class_of(h, i * 16);
      for (size_t c = 0; c < CN_MAXCLS; c++) g_csize[c] = cnm_class_size(h, c);
      g_ncached = g_cls[(mx + 15) >> 4] + 1;
      g_fill = cap / 2 ? cap / 2 : 1;
      g_max = mx;
      MemoryBarrier();
      g_cap = cap;                      // last: the cache is on only when everything above is ready
    }
    InterlockedExchange(&g_state, 2);
  } else {
    while (g_state != 2) YieldProcessor();
  }
  return g_heap;
}
#define H() (g_state == 2 ? g_heap : cn_start())

// ---- the C runtime's heap names, over the heap; foreign blocks are forwarded inside cnm_* ---------------------------
// The cache's allocate: the caller has checked g_cap, a worker's slot and n <= g_max.
static void* cn_cache_alloc(size_t n, int zero) {
  CnTls* t = &t_c;
  CnSlot* s = t->slot;
  size_t cl = g_cls[(n + 15) >> 4];
  void* p = t->head[cl];
  if (p) {
    t->head[cl] = *(void**)p;
    t->count[cl]--;
    s->hits++;
    s->bytes -= (int64_t)g_csize[cl];
  } else {
    size_t got = 0;
    s->misses++;
    void* chain = cnm_batch_alloc(g_heap, cl, g_fill, &got);   // one take of the class lock
    if (!got) return NULL;
    p = chain;
    t->head[cl] = *(void**)chain;       // the rest of the chain is the worker's list
    t->count[cl] = (uint16_t)(got - 1);
    s->bytes += (int64_t)((got - 1) * g_csize[cl]);
    s->fills++;
  }
  if (zero) memset(p, 0, g_csize[cl]);  // a cached cell is dirty
  return p;
}

// The cache's free: p is a small block of a cached class. See "WHY THE PAGE COUNTS STAY EXACT" above: p's page's
// used count is not touched here, so the page cannot retire while p sits in this list. Reading p's class without
// a lock is cnm_cell_class's, on freeSmall's argument (p is live in the caller's hands, so its page's class is
// fixed until p is freed to the heap).
static void cn_cache_free(size_t cl, void* p) {
  CnTls* t = &t_c;
  CnSlot* s = t->slot;
  *(void**)p = t->head[cl];
  t->head[cl] = p;
  t->count[cl]++;
  s->bytes += (int64_t)g_csize[cl];
  if (t->count[cl] > g_cap) {
    // over the cap: keep the newer half, return the older half under one take of the class lock
    size_t count = t->count[cl], ret = count / 2, keep = count - ret;
    void* last = t->head[cl];
    for (size_t i = 1; i < keep; i++) last = *(void**)last;
    void* chain = *(void**)last;
    *(void**)last = NULL;
    t->count[cl] = (uint16_t)keep;
    s->bytes -= (int64_t)(ret * g_csize[cl]);
    s->returns++;
    cnm_batch_free(g_heap, cl, chain, ret);
  }
}

// every cached cell goes back to the heap
static void cn_cache_flush(void) {
  CnTls* t = &t_c;
  if (!t->slot) return;
  for (size_t cl = 0; cl < g_ncached; cl++) {
    if (t->count[cl]) {
      size_t n = t->count[cl];
      void* head = t->head[cl];
      t->head[cl] = NULL;
      t->count[cl] = 0;
      cnm_batch_free(g_heap, cl, head, n);
    }
  }
  t->slot->bytes = 0;
}

void* __cdecl malloc(size_t n) {
  if (g_cap && n <= g_max && t_c.slot) return cn_cache_alloc(n, 0);
  return cnm_malloc(H(), n);
}
void* __cdecl calloc(size_t c, size_t n) {
  if (g_cap && t_c.slot && (c == 0 || n <= SIZE_MAX / c) && c * n <= g_max) return cn_cache_alloc(c * n, 1);
  return cnm_calloc(H(), c, n);
}
void* __cdecl realloc(void* p, size_t n) { return cnm_realloc(H(), p, n); }
void __cdecl free(void* p) {
  if (!p) return;
  if (g_cap && t_c.slot) {
    size_t cl = cnm_cell_class(g_heap, p);    // none (all ones) for a large or foreign block
    if (cl < g_ncached) { cn_cache_free(cl, p); return; }
  }
  cnm_free(H(), p);
}
size_t __cdecl _msize(void* p) { return cnm_usable(H(), p); }
void* __cdecl _recalloc(void* p, size_t c, size_t n) { return cnm_recalloc(H(), p, c, n); }
void* __cdecl _expand(void* p, size_t n) { return cnm_expand(H(), p, n); }
void* __cdecl _aligned_malloc(size_t n, size_t a) { return cnm_aligned(H(), a, n); }
void __cdecl _aligned_free(void* p) { if (p) cnm_aligned_free(H(), p); }
void* __cdecl _aligned_realloc(void* p, size_t n, size_t a) { return cnm_aligned_realloc(H(), p, n, a); }
size_t __cdecl _aligned_msize(void* p, size_t a, size_t off) { return cnm_aligned_msize(H(), p, a, off); }

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

// row 451: the size of the cell malloc(n) hands out, for the heap region's trial setting (CONE_HEAP_CLASS_SIZE=1;
// conestd's heapregion.c finds this by GetProcAddress). 0 and requests over the small classes answer n.
__declspec(dllexport) size_t cnmalloc_sizeFor(size_t n) {
  if (n == 0 || n > 32768) return n;
  uintptr_t h = H();
  size_t cl = cnm_class_of(h, n);
  if (cl == (size_t)-1) return n;
  size_t s = cnm_class_size(h, cl);
  return s < n ? n : s;
}

// for the process's own tools: give back what is due (force: everything), the counters
uint64_t cn_collect(int force) { return cnm_collect(H(), force); }
uint64_t cn_stats(uint64_t* out, size_t max) { uintptr_t h = H(); cn_publish(); return cnm_stats(h, out, max); }

// ---- the runtime's three calls (row 426) -------------------------------------------------------------------------
// The actors runtime finds these by GetProcAddress on the program's own module, once per worker: they are exported
// from the executable only when this library is linked, so a program without cnmalloc finds nothing and calls nothing.
// begin: this thread is a worker and gets a cache (when the cache is on). park: it is about to sleep; its cache goes
// back to the heap, then everything due under the give-back delay is given back (row 425's timer handles what falls
// due later). end: the worker is finishing; its cache goes back and its slot is freed.
__declspec(dllexport) void cnmalloc_workerBegin(void) {
  H();
  if (!g_cap || t_c.slot) return;
  for (int i = 0; i < CN_SLOTS; i++) {
    if (g_slotUsed[i] == 0 && InterlockedCompareExchange(&g_slotUsed[i], 1, 0) == 0) { t_c.slot = &g_slots[i]; return; }
  }                                    // out of slots: this worker simply has no cache
}

__declspec(dllexport) void cnmalloc_workerPark(void) {
  uintptr_t h = H();
  if (g_cap) cn_cache_flush();
  cnm_collect(h, 0);
}

__declspec(dllexport) void cnmalloc_workerEnd(void) {
  if (!g_cap || !t_c.slot) return;
  cn_cache_flush();
  CnSlot* s = t_c.slot;
  InterlockedAdd64(&g_endHits, s->hits);
  InterlockedAdd64(&g_endMisses, s->misses);
  InterlockedAdd64(&g_endReturns, s->returns);
  InterlockedAdd64(&g_endFills, s->fills);
  s->hits = s->misses = s->returns = s->fills = s->bytes = 0;
  t_c.slot = NULL;
  InterlockedExchange(&g_slotUsed[s - g_slots], 0);
}
