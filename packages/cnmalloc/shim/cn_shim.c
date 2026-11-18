// cn_shim.c: the C part of cnmalloc (row 425). The heap itself is the Cone package (src/cnmalloc.cone, object
// cnmalloc.obj); this file is what Cone could not say, each part a language gap named in the PR:
//   1. the one mutable global (the process's heap pointer), error 1626 in a C-named module;
//   2. the compare-and-swap lazy start (no atomics in a prelude-less module);
//   3. the __imp_ pointers (data symbols whose value is a function's address);
//   4. the exit-time statistics hook (atexit, only under CNMALLOC=stats);
//   5. (row 426) the workers' cache: a C thread-local, the one thread-local in cnmalloc (see "THE WORKERS' CACHE").
//   6. (row 454) the second design's common paths: lf_alloc, lf_free, lf_release and what they need (see "THE SECOND
//      DESIGN"): an atomic operation and a thread-local on every call, which a C-named Cone module cannot say.
// Everything else (the names malloc, calloc, ... included) could be exported from Cone; they are here too so the
// library has one place that decides what a standard-API call does. Nothing here calls malloc or anything that does.
//
// Build: /D_ACRTIMP= (else a C caller's malloc is dllimport and the swap silently does not happen), /MD.
// Variants (shim/build-cn.ps1): -DCN_HOOKS adds the test hooks of the second design (a function called between a
// thread's page load and its compare-and-swap, and inside a detach and a retire); -DCN_DEBUG makes a double free abort
// (or call the handler a test installs).
#include <windows.h>
#include <intrin.h>
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
// row 454: the second design's slow paths (all in src/cnmalloc.cone, under the class lock)
extern size_t cnm_design(uintptr_t h);
extern uintptr_t cnm_lf_info(uintptr_t h);
extern size_t cnm_lf_attach(uintptr_t h, size_t cl, size_t tnum, uintptr_t seen);
extern void cnm_lf_after_free(uintptr_t h, uintptr_t d2, uint64_t g);
extern void cnm_lf_grow(uintptr_t h, size_t cl);
extern void cnm_lf_set(uintptr_t h, void* alloc, void* free, size_t debug);
extern void cnm_lf_set_hooks(uintptr_t h, void* detach, void* retire, void* claim);
extern void cnm_lf_set_counts(uintptr_t h, uint64_t callocs, uint64_t callocBytes, uint64_t nonWorker);
extern uint64_t cnm_park_ms(uintptr_t h);

static volatile uintptr_t g_heap;
static volatile LONG g_state;       // 0 not started, 1 starting, 2 ready
static int g_stats;

// ---- THE SECOND DESIGN (row 454; WI\cnmalloc-second-design-spec.md) ------------------------------------------------
// Off unless CNMALLOC=design=lockfree (g_lf is 0, and nothing below this banner is reached). The page descriptors and the
// per-class records are the Cone package's (src/cnmalloc.cone, "THE SECOND DESIGN" has the layout and the rules for
// who writes what); this file reads and compare-and-swaps them on the common paths of section 3 (allocate) and
// section 4 (free). Static asserts pin the layout both sides agree on.
#define CN_MAXCLS 72
#define LF_ATTACHED 1ull

typedef struct {                         // a page's second descriptor: 576 bytes
  volatile uint64_t genclass;            // +0   class in the low 8 bits, generation above
  volatile uint64_t state;               // +8   ATTACHED 1, LISTED 2, UNLISTED 3, RETIRED 0
  volatile uint64_t ncells;              // +16
  uint64_t base;                         // +24  the page's address, never changes
  uint64_t pad[4];                       // +32
  volatile uint64_t bits[64];            // +64  set means free
} LfD2;

typedef struct {                         // a class's record: 512 bytes
  volatile uintptr_t slot[8];            // +0    an ATTACHED page's descriptor, or 0
  volatile uint32_t active;              // +64   active slots
  uint32_t cap;                          // +68
  uint32_t collLimit;                    // +72
  uint32_t pad0;
  volatile uint8_t used[8];              // +80   set by an allocation that finds it clear; cleared by collect
  uint8_t pad1[40];
  uint64_t idleSince[8];                 // +128  (the Cone side's)
  volatile LONG collisions;              // +192  collisions since the last change of the slot count
  uint8_t pad2[60];
  volatile int64_t st[10];               // +256  allocs, frees, collisions, slots added, dropped, pages attached, listed, retired, retires abandoned, Rule G undos
  uint8_t pad3[176];
} LfCls;

typedef struct { uint32_t tnum; uint8_t start[CN_MAXCLS]; uintptr_t startPage[CN_MAXCLS]; } LfTls;

typedef char lf_assert_d2[(sizeof(LfD2) == 576 && offsetof(LfD2, bits) == 64) ? 1 : -1];
typedef char lf_assert_cls[(sizeof(LfCls) == 512 && offsetof(LfCls, used) == 80 && offsetof(LfCls, collisions) == 192 && offsetof(LfCls, st) == 256) ? 1 : -1];

static int g_lf;                         // the second design is on
static uintptr_t g_pbase, g_psize, g_d2base;
static LfCls* g_lfc;
static LONG g_tnum;
static size_t g_lfsize[CN_MAXCLS];       // cell size
static uint32_t g_lfnw[CN_MAXCLS];       // words of bits
static uint32_t g_lfnfull[CN_MAXCLS];    // words wholly valid
static uint64_t g_lflastmask[CN_MAXCLS]; // the valid bits of the word after them (0: none)
static uint64_t g_lfrecip[CN_MAXCLS];    // ceil(2^32 / size): offset * recip >> 32 is offset / size for offsets under 65536
static uint8_t g_lfmap[65];              // (n + 15) >> 4 -> class, for n up to 1024
static __declspec(thread) LfTls t_lf;
static volatile LONG64 g_callocs, g_callocBytes, g_nonWorker;
#ifdef CN_HOOKS
static void (*g_hookPre)(size_t cl, uint64_t old);
#define LF_HOOK(c, old) do { void (*hk)(size_t, uint64_t) = g_hookPre; if (hk) hk(c, old); } while (0)
#else
#define LF_HOOK(c, old) do { } while (0)
#endif
#ifdef CN_DEBUG
static void (*g_dfHandler)(void* cell);
#endif

static inline size_t lf_classof(size_t n) {          // n up to 32768
  if (n <= 1024) return g_lfmap[(n + 15) >> 4];
  unsigned long msb;
  _BitScanReverse64(&msb, n - 1);
  return 8 + (msb - 7) * 8 + (((n - 1) >> (msb - 3)) & 7);
}
static inline int lf_owned(uintptr_t p) { return p - g_pbase < g_psize; }
static inline uint64_t lf_mask(size_t c, uint32_t j) {
  if (j < g_lfnfull[c]) return ~0ull;
  return j == g_lfnfull[c] ? g_lflastmask[c] : 0;
}
static inline void lf_count(volatile int64_t* k) { _InterlockedIncrement64((volatile LONG64*)k); }

#ifdef CN_DEBUG
static void lf_double_free(LfD2* P, uint32_t j, uint64_t bit, uint64_t g) {
  unsigned long b;
  _BitScanForward64(&b, bit);
  void* cell = (void*)(P->base + ((size_t)j * 64 + b) * g_lfsize[g & 0xFF]);
  if (g_dfHandler) { g_dfHandler(cell); return; }
  static const char msg[] = "cnmalloc: double free\n";
  DWORD w;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof msg - 1, &w, NULL);
  ExitProcess(134);
}
#endif

// Section 4's release: one fetch_or, never retried. g is the page's genclass as read BEFORE the cell stopped being
// the caller's (free reads it first; an allocation's undo passes g2, read while the thread held the bit).
static void lf_release(LfD2* P, uint64_t g, uint32_t j, uint64_t bit) {
  uint64_t old = (uint64_t)_InterlockedOr64((volatile LONG64*)&P->bits[j], (LONG64)bit);
  if (old & bit) {                                    // double free: abort in a debug build, ignore otherwise
#ifdef CN_DEBUG
    lf_double_free(P, j, bit, g);
#endif
    return;
  }
  if ((old == 0 || (old | bit) == lf_mask(g & 0xFF, j)) && P->state != LF_ATTACHED)
    cnm_lf_after_free(g_heap, (uintptr_t)P, g);        // a full page has room again, or a page may be wholly free
}

static void lf_free(void* p) {
  uintptr_t off = (uintptr_t)p - g_pbase;
  LfD2* P = (LfD2*)(g_d2base + (off >> 16) * sizeof(LfD2));
  uint64_t g = P->genclass;                           // read while the cell is still the caller's
  size_t c = g & 0xFF;
  uint32_t idx = (uint32_t)(((uint64_t)(off & 0xFFFF) * g_lfrecip[c]) >> 32);
  if (g_stats) lf_count(&g_lfc[c].st[1]);
  lf_release(P, g, idx >> 6, 1ull << (idx & 63));
}

// Section 3's allocate. One compare-and-swap on the common path; the page's genclass is read before it and after it
// (Rule G). Where a thread starts: word 0, or the word it last moved to after a collision on this page. A pass that
// found no free bit but lost a compare-and-swap is repeated (up to four passes) before the slot's page is given up.
static void* lf_alloc(size_t c) {
  LfCls* L = &g_lfc[c];
  LfTls* T = &t_lf;
restart:;
  uint32_t tn = T->tnum;
  if (!tn) tn = T->tnum = (uint32_t)InterlockedIncrement(&g_tnum);
  uint32_t si = tn % L->active;
  LfD2* P = (LfD2*)L->slot[si];
  int ask = 0;
  if (P) {
    uint64_t g1 = P->genclass;
    if ((g1 & 0xFF) == c) {
      if (!L->used[si]) L->used[si] = 1;
      uint32_t nw = g_lfnw[c];
      uint32_t w = T->startPage[c] == (uintptr_t)P ? T->start[c] : 0;
      int collided = 0;
      for (int pass = 0; pass < 4; pass++) {
        int lost = 0;
        for (uint32_t n = 0; n < nw; n++) {
          uint32_t j = w + n;
          if (j >= nw) j -= nw;
          uint64_t old = P->bits[j];
          if (!old) continue;
          unsigned long b;
          _BitScanForward64(&b, old);
          uint64_t bit = 1ull << b;
          LF_HOOK(c, old);
          if ((uint64_t)_InterlockedCompareExchange64((volatile LONG64*)&P->bits[j], (LONG64)(old & ~bit), (LONG64)old) != old) {
            lost = collided = 1;                        // note one collision on class c; move to the next word
            if (g_stats) lf_count(&L->st[2]);
            if ((uint32_t)_InterlockedIncrement(&L->collisions) == L->collLimit) {
              if (L->active < L->cap) ask = 1; else L->collisions = 0;
            }
            continue;
          }
          uint64_t g2 = P->genclass;
          if (g2 != g1) {                               // Rule G: the page changed life; give the bit back, start again
            if (g_stats) lf_count(&L->st[9]);
            lf_release(P, g2, j, bit);
            if (ask) cnm_lf_grow(g_heap, c);
            goto restart;
          }
          if (collided) { T->start[c] = (uint8_t)j; T->startPage[c] = (uintptr_t)P; }
          if (ask) cnm_lf_grow(g_heap, c);
          if (g_stats) lf_count(&L->st[0]);
          return (void*)(P->base + ((size_t)j * 64 + b) * g_lfsize[c]);
        }
        if (!lost) break;
      }
    }
  }
  if (ask) cnm_lf_grow(g_heap, c);
  if (!cnm_lf_attach(g_heap, c, tn, (uintptr_t)P)) return NULL;     // the slow path, under the class lock
  goto restart;
}

static size_t lf_cellclass(uintptr_t p) {              // none (all ones) for a block that is not a small cell
  if (!lf_owned(p)) return (size_t)-1;
  return (size_t)(((LfD2*)(g_d2base + ((p - g_pbase) >> 16) * sizeof(LfD2)))->genclass & 0xFF);
}

static void lf_setup(uintptr_t h) {
  uintptr_t info = cnm_lf_info(h);
  g_pbase = ((uintptr_t*)info)[0];
  g_d2base = ((uintptr_t*)info)[1];
  g_psize = ((uintptr_t*)info)[2];
  g_lfc = (LfCls*)((uintptr_t*)info)[3];
  for (size_t c = 0; c < CN_MAXCLS; c++) {
    size_t sz = cnm_class_size(h, c);
    size_t ncells = 65536 / sz;
    g_lfsize[c] = sz;
    g_lfnw[c] = (uint32_t)((ncells + 63) / 64);
    g_lfnfull[c] = (uint32_t)(ncells >> 6);
    g_lflastmask[c] = (ncells & 63) ? (1ull << (ncells & 63)) - 1 : 0;
    g_lfrecip[c] = ((1ull << 32) + sz - 1) / sz;
  }
  for (size_t i = 0; i <= 64; i++) g_lfmap[i] = (uint8_t)cnm_class_of(h, i * 16);
}

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
//
// Under the second design (row 454) the same cache, mended (spec section 9): a cached cell is one whose bit is clear
// in its page (taken by lf_alloc, not yet given back by lf_free), so a page holding one cannot retire, exactly as
// above. (1) It is emptied only after the worker has stayed idle: the actors scheduler parks the worker with a timeout
// (cnmalloc_workerParkMs) and calls cnmalloc_workerPark only when it passes with no work. (2) A fill is sized to
// demand: 2 cells on the first miss after the cache was empty, doubling on each further miss, up to half the cap.
// (3) Small realloc, _recalloc and the aligned calls go through it (below).
#define CN_SLOTS 512
#define CN_MAXREQ 32768
typedef struct { volatile int64_t bytes, hits, misses, returns, fills; int64_t pad[3]; } CnSlot;   // one cache line
typedef struct { void* head[CN_MAXCLS]; uint16_t count[CN_MAXCLS]; uint16_t fill[CN_MAXCLS]; CnSlot* slot; } CnTls;
static __declspec(thread) CnTls t_c;
static __declspec(align(64)) CnSlot g_slots[CN_SLOTS];
static volatile LONG g_slotUsed[CN_SLOTS];
static volatile LONG64 g_endHits, g_endMisses, g_endReturns, g_endFills;   // workers that have ended
static size_t g_cap, g_max, g_ncached, g_fill;
static size_t g_csize[CN_MAXCLS];
static uint8_t g_cls[CN_MAXREQ / 16 + 2];       // (n + 15) >> 4 -> class, for n up to cache_max

// push the totals to the heap's counters (before they are read or printed)
static void cn_publish(void) {
  if (g_lf) cnm_lf_set_counts(g_heap, (uint64_t)g_callocs, (uint64_t)g_callocBytes, (uint64_t)g_nonWorker);
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
    if (cnm_design(h) == 2) {
      lf_setup(h);
#ifdef CN_DEBUG
      cnm_lf_set(h, (void*)lf_alloc, (void*)lf_free, 1);
#else
      cnm_lf_set(h, (void*)lf_alloc, (void*)lf_free, 0);
#endif
      MemoryBarrier();
      g_lf = 1;
    }
    InterlockedExchange(&g_state, 2);
  } else {
    while (g_state != 2) YieldProcessor();
  }
  return g_heap;
}
#define H() (g_state == 2 ? g_heap : cn_start())

// ---- the C runtime's heap names, over the heap; foreign blocks are forwarded inside cnm_* ---------------------------
// The cache's allocate: the caller has checked g_cap, a worker's slot and that the class is cached.
static void* cn_cache_alloc_cl(size_t cl, int zero) {
  CnTls* t = &t_c;
  CnSlot* s = t->slot;
  void* p = t->head[cl];
  if (p) {
    t->head[cl] = *(void**)p;
    t->count[cl]--;
    s->hits++;
    s->bytes -= (int64_t)g_csize[cl];
  } else if (g_lf) {
    // the second design: no lock to take, so the fill is sized to demand (2 cells, then doubling up to half the cap)
    size_t want = t->fill[cl] ? t->fill[cl] : 2;
    if (want > g_fill) want = g_fill;
    s->misses++;
    p = lf_alloc(cl);
    if (!p) return NULL;
    size_t got = 1;
    while (got < want) {
      void* q = lf_alloc(cl);
      if (!q) break;
      *(void**)q = t->head[cl];
      t->head[cl] = q;
      t->count[cl]++;
      got++;
    }
    s->bytes += (int64_t)((got - 1) * g_csize[cl]);
    s->fills++;
    t->fill[cl] = (uint16_t)(want * 2 > g_fill ? g_fill : want * 2);
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
  if (zero) {
    memset(p, 0, g_csize[cl]);          // a cached cell is dirty
    if (g_lf && g_stats) { _InterlockedIncrement64(&g_callocs); _InterlockedExchangeAdd64(&g_callocBytes, (LONG64)g_csize[cl]); }
  }
  return p;
}
static void* cn_cache_alloc(size_t n, int zero) { return cn_cache_alloc_cl(g_cls[(n + 15) >> 4], zero); }

// give a chain of cells (linked through their first words) back to the heap, one release each (design 2)
static void cn_lf_give_chain(void* p, size_t n) {
  while (p && n--) {
    void* next = *(void**)p;            // before the release: once the bit is set the cell may be anyone's
    lf_free(p);
    p = next;
  }
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
    if (g_lf) cn_lf_give_chain(chain, ret);
    else cnm_batch_free(g_heap, cl, chain, ret);
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
      if (g_lf) cn_lf_give_chain(head, n);
      else cnm_batch_free(g_heap, cl, head, n);
    }
    t->fill[cl] = 0;                    // design 2: the next miss fills 2 again
  }
  t->slot->bytes = 0;
}

// (design 2) a small cell of class cl for a caller that wants one: from the worker's cache when it has one for the class
static void* lf_take_class(size_t cl) {
  if (g_cap && t_c.slot && cl < g_ncached) return cn_cache_alloc_cl(cl, 0);
  if (g_stats && !t_c.slot) _InterlockedIncrement64(&g_nonWorker);
  return lf_alloc(cl);
}

void* __cdecl malloc(size_t n) {
  if (g_cap && n <= g_max && t_c.slot) return cn_cache_alloc(n, 0);
  uintptr_t h = H();
  if (g_lf && n <= 32768) return lf_take_class(lf_classof(n));
  return cnm_malloc(h, n);
}
void* __cdecl calloc(size_t c, size_t n) {
  if (g_cap && t_c.slot && (c == 0 || n <= SIZE_MAX / c) && c * n <= g_max) return cn_cache_alloc(c * n, 1);
  uintptr_t h = H();
  if (g_lf && (c == 0 || n <= SIZE_MAX / c) && c * n <= 32768) {
    size_t cl = lf_classof(c * n);
    void* p = lf_take_class(cl);
    if (p) {                            // every small cell is filled (spec section 7)
      memset(p, 0, g_lfsize[cl]);
      if (g_stats) { _InterlockedIncrement64(&g_callocs); _InterlockedExchangeAdd64(&g_callocBytes, (LONG64)g_lfsize[cl]); }
    }
    return p;
  }
  return cnm_calloc(h, c, n);
}
void __cdecl free(void* p) {
  if (!p) return;
  if (g_cap && t_c.slot) {
    size_t cl = g_lf ? lf_cellclass((uintptr_t)p) : cnm_cell_class(g_heap, p);    // none (all ones) for a large or foreign block
    if (cl < g_ncached) { cn_cache_free(cl, p); return; }
  }
  uintptr_t h = H();
  if (g_lf && lf_owned((uintptr_t)p)) { lf_free(p); return; }
  cnm_free(h, p);
}
// (design 2) realloc of a small block: the same class stays put; otherwise a new cell through malloc (so through the
// cache), the bytes copied, the old cell through free
static void* lf_realloc_small(void* p, size_t n) {
  size_t c = lf_cellclass((uintptr_t)p), old = g_lfsize[c];
  if (n == 0) { free(p); return NULL; }
  if (n <= 32768 && lf_classof(n) == c) return p;
  void* q = malloc(n);
  if (!q) return NULL;
  memcpy(q, p, n < old ? n : old);
  free(p);
  return q;
}
void* __cdecl realloc(void* p, size_t n) {
  uintptr_t h = H();
  if (g_lf && p && lf_owned((uintptr_t)p)) return lf_realloc_small(p, n);
  return cnm_realloc(h, p, n);
}
size_t __cdecl _msize(void* p) { return cnm_usable(H(), p); }
void* __cdecl _recalloc(void* p, size_t c, size_t n) {
  uintptr_t h = H();
  if (!g_lf) return cnm_recalloc(h, p, c, n);
  if (c != 0 && n > SIZE_MAX / c) return NULL;          // as cnm_recalloc: what the block grew by is zeroed, counted from _msize
  size_t total = c * n, old = p ? cnm_usable(h, p) : 0;
  void* q = realloc(p, total);
  if (q && total > old && total != 0) memset((char*)q + old, 0, total - old);
  return q;
}
void* __cdecl _expand(void* p, size_t n) { return cnm_expand(H(), p, n); }
// (design 2) a small aligned cell: the smallest class whose size is a multiple of the alignment, through the cache
void* __cdecl _aligned_malloc(size_t n, size_t a) {
  uintptr_t h = H();
  if (g_lf && a != 0 && (a & (a - 1)) == 0) {
    if (a <= 16) return malloc(n);
    if (n <= 32768 && a <= 32768) {
      size_t cl = lf_classof(n);
      while (cl < CN_MAXCLS && (g_lfsize[cl] & (a - 1))) cl++;
      if (cl < CN_MAXCLS) return lf_take_class(cl);
    }
  }
  return cnm_aligned(h, a, n);
}
void __cdecl _aligned_free(void* p) {
  if (!p) return;
  uintptr_t h = H();
  if (g_lf && lf_owned((uintptr_t)p)) { free(p); return; }
  cnm_aligned_free(h, p);
}
void* __cdecl _aligned_realloc(void* p, size_t n, size_t a) {
  uintptr_t h = H();
  if (g_lf && (!p || lf_owned((uintptr_t)p))) {
    if (!p) return _aligned_malloc(n, a);
    void* q = _aligned_malloc(n, a);
    if (!q) return NULL;
    size_t old = g_lfsize[lf_cellclass((uintptr_t)p)];
    memcpy(q, p, n < old ? n : old);
    free(p);
    return q;
  }
  return cnm_aligned_realloc(h, p, n, a);
}
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

// for the process's own tools: give back what is due (force: everything), the counters
uint64_t cn_collect(int force) { return cnm_collect(H(), force); }
uint64_t cn_stats(uint64_t* out, size_t max) { uintptr_t h = H(); cn_publish(); return cnm_stats(h, out, max); }

// ---- the runtime's calls (rows 426 and 454) --------------------------------------------------------------------------
// The actors runtime finds these by GetProcAddress on the program's own module, once per worker: they are exported
// from the executable only when this library is linked, so a program without cnmalloc finds nothing and calls nothing.
// begin: this thread is a worker and gets a cache (when the cache is on). park: it is about to sleep (design 1), or it
// has slept a whole give-back delay with no work (design 2): its cache goes back to the heap, then everything due under
// the give-back delay is given back (row 425's timer handles what falls due later). end: the worker is finishing; its
// cache goes back and its slot is freed. parkMs: 0 under design 1 (the scheduler parks and calls park as it always did);
// under design 2 the milliseconds a worker sleeps before it calls park, then it sleeps without a limit.
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

__declspec(dllexport) uint32_t cnmalloc_workerParkMs(void) {
  uintptr_t h = H();
  return g_lf ? (uint32_t)cnm_park_ms(h) : 0;
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

// ---- for the tests (row 454) -----------------------------------------------------------------------------------------
// The second design's counters summed over the classes: 0 allocs, 1 frees, 2 collisions, 3 slots added, 4 slots dropped,
// 5 pages attached, 6 listed, 7 retired, 8 retires abandoned, 9 Rule G undos (0, 1, 2 and 9 only in a statistics run).
// -1 when the second design is off. And which design this heap is.
__declspec(dllexport) int64_t cnmalloc_lfTotal(int which) {
  H();
  if (!g_lf || which < 0 || which > 9) return -1;
  int64_t t = 0;
  for (int c = 0; c < CN_MAXCLS; c++) t += g_lfc[c].st[which];
  return t;
}
__declspec(dllexport) int cnmalloc_design(void) { H(); return g_lf ? 2 : 1; }
__declspec(dllexport) uintptr_t cnmalloc_heap(void) { return H(); }       // the process's heap, for tests that call cnm_* on it
__declspec(dllexport) uint64_t cnmalloc_collect(int force) { return cnm_collect(H(), force != 0); }
__declspec(dllexport) int cnmalloc_lfActive(int cl) { H(); return g_lf ? (int)g_lfc[cl].active : 0; }
#ifdef CN_HOOKS
// pre: called in lf_alloc between a thread's page load and its compare-and-swap, with the class and the word value the swap will expect; detach: inside a
// detach, between the state exchange and the scan; retire: inside a retire, after RETIRED and before the generation
// rises; claim: inside a retire, after the page was found wholly free and before its words are claimed
__declspec(dllexport) void cnmalloc_lfSetHooks(void (*pre)(size_t, uint64_t), void (*detach)(size_t), void (*retire)(size_t), void (*claim)(size_t)) {
  uintptr_t h = H();
  g_hookPre = pre;
  cnm_lf_set_hooks(h, (void*)detach, (void*)retire, (void*)claim);
}
#endif
#ifdef CN_DEBUG
// a test installs a handler that is called with the cell instead of the process ending
__declspec(dllexport) void cnmalloc_setDoubleFreeHandler(void (*f)(void*)) { g_dfHandler = f; }
#endif
