// memstats: the tagging link shim for Cone programs built with -D MEMSTATS.
//
// Linked ahead of the C runtime, this library defines malloc, calloc, realloc,
// free, _msize, _recalloc, _expand and the _aligned_* functions. Each forwards to
// ucrtbase's own (found with GetProcAddress, as the experiment's memshim.c does),
// with a 32-byte TAG in front of every block it hands out:
//
//   word 0  check   the block's address xor MAGIC: the magic word and the check of
//                   the tag's own address in one word
//   word 1  size (low 40 bits) | kind (16) | growths (8, saturating)
//   word 2  the first allocation's time, in QueryPerformanceCounter ticks (the
//           clock the trace package's ticks() reads), carried across reallocs
//   word 3  allocating actor (32 bits: its header address >> 4) | worker (16:
//           a small number given to each thread on its first use) |
//           padding (16: in 16-byte units, from the start of the C runtime's
//           block to the tag, nonzero only for aligned blocks)
//
// A block the C runtime made inside its own DLL (_strdup, _getcwd, fopen's FILE...)
// carries no tag, and is recognised at free, realloc, _msize by its check word being
// wrong; it is forwarded untouched. The read of the 32 bytes before such a block is
// safe in practice on Windows (the heap's own header, and the block before it, lie
// there), but a block can start a page whose predecessor is not committed, so the
// read is guarded with SEH and a fault means foreign.
//
// Counters are per thread (a slot of its own, found through a C thread-local), summed
// by the sampler, so the shim adds no contention. The shim never allocates from the C
// heap, never takes a lock and never calls a sink: a thread's slot is VirtualAlloc'd.
//
// "Who is asking" is the C thread-local the region code sets through
// memstats_asking(kind, elemSize) just before it calls down, and clears with
// memstats_asking(0, 0) after; the outermost non-zero setter wins, so a collection's
// kind is kept when its region calls RawBlocks. A block allocated with no kind set
// is STRAY: the caller's return address is recorded (the report sorts them by site
// through the program's PDB).
//
// The sampler thread is started by the first non-zero memstats_asking (not from a
// malloc: the first malloc runs during the C runtime's start-up). It writes one JSON
// line every 100 ms to the file MEMSTATS_OUT names (a folder: memstats-<pid>.jsonl in it),
// and a totals line at exit. With MEMSTATS_OUT unset the totals go to stderr and there are
// no samples.
//
// This source file is part of the Cone Programming Language
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <intrin.h>
#include "memstats.h"

#define MAGIC 0x6d656d7374617473ull
#define TAGSZ 32
#define MAXK 64          // kinds counted (a larger kind is counted as MAXK-1; the tag keeps it)
#define NSC 40           // size classes: bit length of the size
#define NLIFE 64
#define MAXT 2048
#define NSTRAY 512

typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16;
typedef struct { u64 check, w1, time, w3; } Tag;

typedef struct { u64 na, ba, nf, bf, ni, bi, no, bo; } Key;   // alloc, free, realloc in, realloc out
typedef struct { u64 ra[4], n, b; } StrayRec;   // the return address into malloc and three callers above
typedef struct {
  Key key[MAXK][NSC];
  u64 xan[MAXK], xab[MAXK], xwn[MAXK], xwb[MAXK];  // freed by another actor / another worker
  u64 life[MAXK][NLIFE];                          // lifetime at free, by bit length of ticks
  u64 elem[MAXK][NSC];                            // allocations by bit length of the element size asked
  u64 grown[MAXK], moved[MAXK], copied[MAXK], nrealloc[MAXK];
  u64 foreignFree, foreignRealloc, bigSkipped, bigSkippedB;
  u64 strayLost;
  StrayRec stray[NSTRAY];
} TS;

typedef void* (__cdecl* fn_malloc)(size_t);
typedef void* (__cdecl* fn_calloc)(size_t, size_t);
typedef void* (__cdecl* fn_realloc)(void*, size_t);
typedef void (__cdecl* fn_free)(void*);
typedef size_t (__cdecl* fn_msize)(void*);
typedef void* (__cdecl* fn_expand)(void*, size_t);
typedef void* (__cdecl* fn_amalloc)(size_t, size_t);
typedef void (__cdecl* fn_afree)(void*);
typedef void* (__cdecl* fn_arealloc)(void*, size_t, size_t);
typedef size_t (__cdecl* fn_amsize)(void*, size_t, size_t);
static fn_malloc r_malloc; static fn_calloc r_calloc; static fn_realloc r_realloc; static fn_free r_free;
static fn_msize r_msize; static fn_expand r_expand; static fn_amalloc r_amalloc; static fn_afree r_afree;
static fn_arealloc r_arealloc; static fn_amsize r_amsize;

static volatile LONG init_state;
static LARGE_INTEGER qpf;
static TS* volatile slots[MAXT];
static volatile LONG nslots;
static __declspec(thread) TS* tl_ts;
static __declspec(thread) u32 tl_kind, tl_worker;
static __declspec(thread) size_t tl_elem;
static __declspec(thread) u32 tl_actor;

static void init(void) {
  if (InterlockedCompareExchange(&init_state, 1, 0) == 0) {
    HMODULE m = GetModuleHandleA("ucrtbase.dll");
    if (!m) m = LoadLibraryA("ucrtbase.dll");
    r_malloc = (fn_malloc)GetProcAddress(m, "malloc");
    r_calloc = (fn_calloc)GetProcAddress(m, "calloc");
    r_realloc = (fn_realloc)GetProcAddress(m, "realloc");
    r_free = (fn_free)GetProcAddress(m, "free");
    r_msize = (fn_msize)GetProcAddress(m, "_msize");
    r_expand = (fn_expand)GetProcAddress(m, "_expand");
    r_amalloc = (fn_amalloc)GetProcAddress(m, "_aligned_malloc");
    r_afree = (fn_afree)GetProcAddress(m, "_aligned_free");
    r_arealloc = (fn_arealloc)GetProcAddress(m, "_aligned_realloc");
    r_amsize = (fn_amsize)GetProcAddress(m, "_aligned_msize");
    QueryPerformanceFrequency(&qpf);
    InterlockedExchange(&init_state, 2);
  } else {
    while (init_state != 2) YieldProcessor();
  }
}
#define INIT() do { if (init_state != 2) init(); } while (0)

static u64 now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (u64)t.QuadPart; }
static int bitlen(u64 x) { unsigned long i; if (!x) return 0; _BitScanReverse64(&i, x); return (int)i + 1; }
static int szclass(u64 n) { int c = bitlen(n); return c < NSC ? c : NSC - 1; }
static u32 kidx(u32 k) { return k < MAXK ? k : MAXK - 1; }

static TS* ts_slow(void) {
  LONG i = InterlockedIncrement(&nslots) - 1;
  // beyond MAXT threads, the last slot is shared and its plain increments may lose counts
  if (i >= MAXT) i = MAXT - 1;
  TS* t = slots[i];
  if (!t) {
    TS* n = (TS*)VirtualAlloc(NULL, sizeof(TS), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!n) return slots[0] ? slots[0] : NULL;
    t = (TS*)InterlockedCompareExchangePointer((PVOID volatile*)&slots[i], n, NULL);
    if (t) VirtualFree(n, 0, MEM_RELEASE); else t = n;
  }
  tl_worker = (u32)(i + 1);
  tl_ts = t;
  return t;
}
static TS* ts(void) { TS* t = tl_ts; return t ? t : ts_slow(); }

// ---- the tag
static Tag* find_tag(void* p) {
  if (!p || ((uintptr_t)p & 15)) return NULL;
  Tag* t = (Tag*)((char*)p - TAGSZ);
  __try {
    if (t->check == ((u64)(uintptr_t)p ^ MAGIC)) return t;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return NULL;
}
#define T_SIZE(t) ((t)->w1 & 0xFFFFFFFFFFull)
#define T_KIND(t) ((u32)(((t)->w1 >> 40) & 0xFFFF))
#define T_GROW(t) ((u32)((t)->w1 >> 56))
#define T_PADB(t) ((size_t)(((t)->w3 >> 48) & 0xFFFF) * 16)
#define T_ACTOR(t) ((u32)((t)->w3 & 0xFFFFFFFFull))
#define T_WORKER(t) ((u32)(((t)->w3 >> 32) & 0xFFFF))
#define MAXSIZE (1ull << 39)

// A stray block's site: the return address of the call into malloc, and the three callers
// above it. The first alone is often a runtime function that every allocation of a layer goes
// through (rawblocks.cone's), which says nothing about who asked; the report picks the first
// address outside it. RtlCaptureStackBackTrace walks the unwind data and takes no lock and no heap.
static void note_stray(TS* s, void* ra, size_t n) {
  void* up[5] = {0, 0, 0, 0, 0};
  RtlCaptureStackBackTrace(2, 5, up, NULL);
  u64 f[4]; f[0] = (u64)(uintptr_t)ra; f[1] = (u64)(uintptr_t)up[2]; f[2] = (u64)(uintptr_t)up[3]; f[3] = (u64)(uintptr_t)up[4];
  u32 h = (u32)(((f[0] >> 2) ^ (f[1] * 31) ^ (f[2] * 17) ^ (f[3] * 7)) * 2654435761u) % NSTRAY;
  for (int i = 0; i < 16; i++) {
    StrayRec* r = &s->stray[(h + i) % NSTRAY];
    if (r->ra[0] == 0 || (r->ra[0] == f[0] && r->ra[1] == f[1] && r->ra[2] == f[2] && r->ra[3] == f[3])) {
      memcpy(r->ra, f, sizeof f); r->n++; r->b += n; return;
    }
  }
  s->strayLost++;
}

// Tags a block the C runtime gave at raw, whose user address is user, asked n bytes.
static void* tag_block(char* raw, char* user, size_t n, void* ra) {
  TS* s = ts();
  u32 kind = tl_kind;
  Tag* t = (Tag*)(user - TAGSZ);
  size_t pad = (size_t)(user - TAGSZ - raw);
  u64 tm = now();
  t->check = (u64)(uintptr_t)user ^ MAGIC;
  t->w1 = (u64)n | ((u64)(kind & 0xFFFF) << 40);
  t->time = tm;
  t->w3 = (u64)tl_actor | ((u64)(tl_worker & 0xFFFF) << 32) | ((u64)(pad / 16) << 48);
  u32 k = kidx(kind);
  s->key[k][szclass(n)].na++; s->key[k][szclass(n)].ba += n;
  if (tl_elem) s->elem[k][szclass(tl_elem)]++;
  if (kind == 0) note_stray(s, ra, n);
  return user;
}

static void* alloc_plain(size_t n, int zero, void* ra) {
  INIT();
  // kind 34 (the instrument's own memory, memtrace's tables): straight from the C runtime, untagged and uncounted,
  // so no report has it among the strays (a free or realloc of it finds no tag and is forwarded)
  if (n >= MAXSIZE || tl_kind == 34) return zero ? r_calloc(1, n) : r_malloc(n);
  ts();
  char* raw = (char*)(zero ? r_calloc(1, n + TAGSZ) : r_malloc(n + TAGSZ));
  if (!raw) return NULL;
  return tag_block(raw, raw + TAGSZ, n, ra);
}

// Aligned: the tag sits just before the aligned address, with padding between the
// C runtime's block and the tag. A padding that does not fit its 16-bit field of
// 16-byte units (alignments past about 1 MiB) is not tagged.
static void* alloc_aligned(size_t n, size_t align, void* ra) {
  INIT();
  if (align == 0 || (align & (align - 1))) return NULL;
  if (align <= 16) return alloc_plain(n, 0, ra);
  if (n >= MAXSIZE || align > ((size_t)1 << 19)) { TS* b = ts(); b->bigSkipped++; b->bigSkippedB += n; return r_amalloc(n, align); }
  ts();
  char* raw = (char*)r_malloc(n + align + TAGSZ);
  if (!raw) return NULL;
  char* user = (char*)(((uintptr_t)raw + TAGSZ + align - 1) & ~((uintptr_t)align - 1));
  return tag_block(raw, user, n, ra);
}

static char* raw_of(Tag* t) { return (char*)t - T_PADB(t); }

// Counts the free of a tagged block and zeroes its check word.
static void note_free(Tag* t) {
  TS* s = ts();
  u32 k = kidx(T_KIND(t));
  u64 n = T_SIZE(t);
  s->key[k][szclass(n)].nf++; s->key[k][szclass(n)].bf += n;
  if (T_ACTOR(t) != tl_actor) { s->xan[k]++; s->xab[k] += n; }
  if (T_WORKER(t) != (tl_worker & 0xFFFF)) { s->xwn[k]++; s->xwb[k] += n; }
  u64 life = now() - t->time;
  s->life[k][bitlen(life) < NLIFE ? bitlen(life) : NLIFE - 1]++;
  t->check = 0;
}

void* __cdecl malloc(size_t n) { return alloc_plain(n, 0, _ReturnAddress()); }
void* __cdecl calloc(size_t c, size_t s) {
  size_t n = c * s;
  if (s && n / s != c) return NULL;
  return alloc_plain(n, 1, _ReturnAddress());
}
void __cdecl free(void* p) {
  if (!p) return;
  INIT();
  Tag* t = find_tag(p);
  if (!t) { ts()->foreignFree++; r_free(p); return; }
  char* raw = raw_of(t);
  note_free(t);
  r_free(raw);
}

static void* realloc_impl(void* p, size_t n, void* ra) {
  INIT();
  if (!p) return alloc_plain(n, 0, ra);
  Tag* t = find_tag(p);
  if (!t) { ts()->foreignRealloc++; return r_realloc(p, n); }
  if (n == 0) { free(p); return NULL; }
  TS* s = ts();
  u64 old = T_SIZE(t);
  u32 kind = T_KIND(t), k = kidx(kind);
  s->nrealloc[k]++;
  if (n >= MAXSIZE) return NULL;
  if (T_PADB(t) == 0) {
    char* nr = (char*)r_realloc((char*)t, n + TAGSZ);
    if (!nr) return NULL;
    char* user = nr + TAGSZ;
    Tag* nt = (Tag*)nr;
    nt->check = (u64)(uintptr_t)user ^ MAGIC;
    u64 g = T_GROW(nt);
    if (n > old && g < 255) g++;
    nt->w1 = (u64)n | ((u64)kind << 40) | (g << 56);
    if (n > old) s->grown[k]++;
    if (nr != (char*)t) { s->moved[k]++; s->copied[k] += old < n ? old : n; }
    Key* ko = &s->key[k][szclass(old)]; ko->no++; ko->bo += old;
    Key* kn = &s->key[k][szclass(n)]; kn->ni++; kn->bi += n;
    return user;
  }
  // an aligned block: a new aligned block, a copy, and the old one freed; its first
  // time and growth count carry over, and it counts as a move (and as an alloc and a free)
  unsigned long tz; _BitScanForward64(&tz, (u64)(uintptr_t)p);
  size_t align = (size_t)1 << tz;
  u32 savedKind = tl_kind; tl_kind = kind;
  void* q = alloc_aligned(n, align, ra);
  tl_kind = savedKind;
  if (!q) return NULL;
  Tag* qt = find_tag(q);
  memcpy(q, p, old < n ? old : n);
  if (qt) {
    qt->time = t->time;
    u64 g = T_GROW(t); if (n > old && g < 255) g++;
    qt->w1 = (qt->w1 & 0x00FFFFFFFFFFFFFFull) | (g << 56);
  }
  if (n > old) s->grown[k]++;
  s->moved[k]++; s->copied[k] += old < n ? old : n;
  free(p);
  return q;
}
void* __cdecl realloc(void* p, size_t n) { return realloc_impl(p, n, _ReturnAddress()); }

size_t __cdecl _msize(void* p) {
  INIT();
  Tag* t = find_tag(p);
  return t ? (size_t)T_SIZE(t) : r_msize(p);
}
void* __cdecl _recalloc(void* p, size_t c, size_t s) {
  size_t n = c * s;
  if (s && n / s != c) return NULL;
  size_t old = p ? _msize(p) : 0;
  void* q = realloc_impl(p, n, _ReturnAddress());
  if (q && n > old) memset((char*)q + old, 0, n - old);
  return q;
}
void* __cdecl _expand(void* p, size_t n) {
  INIT();
  if (find_tag(p)) return NULL;   // an expand in place is allowed to fail
  return r_expand(p, n);
}
void* __cdecl _aligned_malloc(size_t n, size_t a) { return alloc_aligned(n, a, _ReturnAddress()); }
void __cdecl _aligned_free(void* p) {
  if (!p) return;
  INIT();
  Tag* t = find_tag(p);
  if (!t) { ts()->foreignFree++; r_afree(p); return; }
  char* raw = raw_of(t);
  note_free(t);
  r_free(raw);
}
void* __cdecl _aligned_realloc(void* p, size_t n, size_t a) {
  INIT();
  if (!p) return alloc_aligned(n, a, _ReturnAddress());
  Tag* t = find_tag(p);
  if (!t) return r_arealloc(p, n, a);
  if (n == 0) { _aligned_free(p); return NULL; }
  void* q = alloc_aligned(n, a, _ReturnAddress());
  if (!q) return NULL;
  u64 old = T_SIZE(t);
  memcpy(q, p, old < n ? old : n);
  _aligned_free(p);
  return q;
}
size_t __cdecl _aligned_msize(void* p, size_t a, size_t off) {
  INIT();
  Tag* t = find_tag(p);
  return t ? (size_t)T_SIZE(t) : r_amsize(p, a, off);
}

// ---- who is asking
static volatile LONG sampler_state;     // 0 not started, 1 starting, 2 running
static volatile LONG sampler_stop;
static HANDLE sampler_thread, out_file = INVALID_HANDLE_VALUE;
static u64 start_ticks;

static void write_totals(void);
static void sample(void);
static DWORD WINAPI sampler_main(LPVOID arg) {
  (void)arg;
  while (!sampler_stop) {
    Sleep(100);
    if (sampler_stop) break;
    sample();
  }
  return 0;
}
static void at_exit(void) {
  sampler_stop = 1;
  if (sampler_thread) WaitForSingleObject(sampler_thread, 1000);
  write_totals();
  if (out_file != INVALID_HANDLE_VALUE) { CloseHandle(out_file); out_file = INVALID_HANDLE_VALUE; }
}
static void start_sampler(void) {
  if (InterlockedCompareExchange(&sampler_state, 1, 0) != 0) return;
  INIT();
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("MEMSTATS_OUT", path, sizeof path);
  if (n > 0 && n < sizeof path - 40) {
    // a folder: one file per process, memstats-<pid>.jsonl, so a suite's many programs do not collide
    DWORD at = GetFileAttributesA(path);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
      _snprintf(path + n, sizeof path - n, "\\memstats-%lu.jsonl", GetCurrentProcessId());
  }
  if (n > 0 && n < sizeof path - 40)
    out_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  start_ticks = now();
  atexit(at_exit);
  if (out_file != INVALID_HANDLE_VALUE) sampler_thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
  InterlockedExchange(&sampler_state, 2);
}

void __cdecl memstats_asking(u32 kind, size_t elemSize) {
  if (kind != 0) {
    if (sampler_state == 0) start_sampler();
    if (tl_kind == 0) { tl_kind = kind; tl_elem = elemSize; }   // the outermost asker wins
  } else {
    tl_kind = 0; tl_elem = 0;
  }
}
void __cdecl memstats_actor(size_t header) { tl_actor = (u32)(header >> 4); }

// ---- reading it back
static int nthreads(void) { LONG n = nslots; return n > MAXT ? MAXT : n; }

void __cdecl memstats_live(u32 kind, u64* n, u64* bytes) {
  u64 ln = 0, lb = 0; u32 k = kidx(kind);
  for (int i = 0; i < nthreads(); i++) {
    TS* s = slots[i]; if (!s) continue;
    for (int c = 0; c < NSC; c++) {
      Key* x = &s->key[k][c];
      ln += x->na + x->ni - x->nf - x->no; lb += x->ba + x->bi - x->bf - x->bo;
    }
  }
  *n = ln; *bytes = lb;
}
u64 __cdecl memstats_counter(int which, u32 kind) {
  u64 v = 0; u32 k = kidx(kind);
  for (int i = 0; i < nthreads(); i++) {
    TS* s = slots[i]; if (!s) continue;
    switch (which) {
      case MS_FREED_OTHER_ACTOR: v += s->xan[k]; break;
      case MS_FREED_OTHER_WORKER: v += s->xwn[k]; break;
      case MS_GROWN: v += s->grown[k]; break;
      case MS_MOVED: v += s->moved[k]; break;
      case MS_REALLOCS: v += s->nrealloc[k]; break;
      case MS_FOREIGN_FREES: v += s->foreignFree; break;
      case MS_ALLOCS: for (int c = 0; c < NSC; c++) v += s->key[k][c].na; break;
      case MS_FREES: for (int c = 0; c < NSC; c++) v += s->key[k][c].nf; break;
    }
  }
  return v;
}
int __cdecl memstats_peek(void* p, MemstatsInfo* out) {
  INIT();
  Tag* t = find_tag(p);
  if (!t) return 0;
  out->size = T_SIZE(t); out->kind = T_KIND(t); out->growths = T_GROW(t); out->time = t->time;
  out->actor = T_ACTOR(t); out->worker = T_WORKER(t); out->padding = T_PADB(t);
  return 1;
}
u64 __cdecl memstats_this_worker(void) { ts(); return tl_worker & 0xFFFF; }

// ---- output (no CRT allocation: a static buffer and snprintf)
static char obuf[1 << 17];
static size_t olen;
static HANDLE odest;
static void oflush(void) {
  if (olen && odest != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(odest, obuf, (DWORD)olen, &w, NULL); }
  olen = 0;
}
static void oprintf(const char* fmt, ...) {
  if (olen > sizeof obuf - 512) oflush();
  va_list ap; va_start(ap, fmt);
  int n = _vsnprintf(obuf + olen, sizeof obuf - olen, fmt, ap);
  va_end(ap);
  if (n > 0) olen += (size_t)n;
}

static void sample(void) {
  PROCESS_MEMORY_COUNTERS_EX pmc; pmc.cb = sizeof pmc;
  u64 priv = 0;
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc)) priv = pmc.PrivateUsage;
  static Key sum[MAXK][NSC];   // only the sampler thread uses it between sample() calls
  memset(sum, 0, sizeof sum);
  for (int i = 0; i < nthreads(); i++) {
    TS* s = slots[i]; if (!s) continue;
    for (int k = 0; k < MAXK; k++) for (int c = 0; c < NSC; c++) {
      Key* x = &s->key[k][c]; Key* y = &sum[k][c];
      y->na += x->na; y->ba += x->ba; y->nf += x->nf; y->bf += x->bf;
      y->ni += x->ni; y->bi += x->bi; y->no += x->no; y->bo += x->bo;
    }
  }
  u64 ln = 0, lb = 0;
  odest = out_file; olen = 0;
  u64 t = now();
  oprintf("{\"t\":%llu,\"live\":[", t);
  int first = 1;
  for (int k = 0; k < MAXK; k++) for (int c = 0; c < NSC; c++) {
    Key* y = &sum[k][c];
    long long dn = (long long)(y->na + y->ni - y->nf - y->no), db = (long long)(y->ba + y->bi - y->bf - y->bo);
    if (dn == 0 && db == 0 && y->na == 0) continue;
    ln += dn; lb += db;
    oprintf("%s[%d,%d,%lld,%lld]", first ? "" : ",", k, c, dn, db);
    first = 0;
  }
  // tagged and stray are both in "live" (stray is kind 0); unaccounted is what the process holds beyond it
  oprintf("],\"live_n\":%lld,\"live_b\":%lld,\"private\":%llu,\"unaccounted\":%lld}\n",
          (long long)ln, (long long)lb, priv, (long long)priv - (long long)lb);
  oflush();
}

static void write_totals(void) {
  static Key sum[MAXK][NSC];
  static u64 xan[MAXK], xab[MAXK], xwn[MAXK], xwb[MAXK], life[MAXK][NLIFE], elem[MAXK][NSC];
  static u64 grown[MAXK], moved[MAXK], copied[MAXK], nre[MAXK];
  static StrayRec stray[NSTRAY * 4];
  u64 foreign = 0, foreignRe = 0, lost = 0, bigN = 0, bigB = 0;
  int nstray = 0;
  for (int i = 0; i < nthreads(); i++) {
    TS* s = slots[i]; if (!s) continue;
    for (int k = 0; k < MAXK; k++) {
      for (int c = 0; c < NSC; c++) {
        Key* x = &s->key[k][c]; Key* y = &sum[k][c];
        y->na += x->na; y->ba += x->ba; y->nf += x->nf; y->bf += x->bf;
        y->ni += x->ni; y->bi += x->bi; y->no += x->no; y->bo += x->bo;
        elem[k][c] += s->elem[k][c];
      }
      for (int c = 0; c < NLIFE; c++) life[k][c] += s->life[k][c];
      xan[k] += s->xan[k]; xab[k] += s->xab[k]; xwn[k] += s->xwn[k]; xwb[k] += s->xwb[k];
      grown[k] += s->grown[k]; moved[k] += s->moved[k]; copied[k] += s->copied[k]; nre[k] += s->nrealloc[k];
    }
    foreign += s->foreignFree; foreignRe += s->foreignRealloc; lost += s->strayLost;
    bigN += s->bigSkipped; bigB += s->bigSkippedB;
    for (int j = 0; j < NSTRAY; j++) {
      StrayRec* r = &s->stray[j]; if (!r->ra[0]) continue;
      int f = 0;
      for (int m = 0; m < nstray; m++) if (memcmp(stray[m].ra, r->ra, sizeof r->ra) == 0) { stray[m].n += r->n; stray[m].b += r->b; f = 1; break; }
      if (!f && nstray < NSTRAY * 4) stray[nstray++] = *r; else if (!f) lost++;
    }
  }
  char path[MAX_PATH];
  int tofile = out_file != INVALID_HANDLE_VALUE;
  odest = tofile ? out_file : GetStdHandle(STD_ERROR_HANDLE);
  olen = 0;
  if (!tofile && GetEnvironmentVariableA("MEMSTATS_OUT", path, sizeof path) == 0) {}
  oprintf("{\"totals\":true,\"freq\":%lld,\"start\":%llu,\"end\":%llu,\"base\":%llu,\"threads\":%d,\"foreign_frees\":%llu,\"foreign_reallocs\":%llu,\"stray_lost\":%llu,\"large_aligned\":[%llu,%llu],\"kinds\":[",
          (long long)qpf.QuadPart, start_ticks, now(), (u64)(uintptr_t)GetModuleHandleA(NULL), nthreads(), foreign, foreignRe, lost, bigN, bigB);
  int firstk = 1;
  for (int k = 0; k < MAXK; k++) {
    int any = 0;
    for (int c = 0; c < NSC; c++) if (sum[k][c].na) any = 1;
    if (!any) continue;
    oprintf("%s{\"k\":%d,\"freed_other_actor\":[%llu,%llu],\"freed_other_worker\":[%llu,%llu],\"reallocs\":%llu,\"grown\":%llu,\"moved\":%llu,\"copied\":%llu,\"cells\":[",
            firstk ? "" : ",", k, xan[k], xab[k], xwn[k], xwb[k], nre[k], grown[k], moved[k], copied[k]);
    firstk = 0;
    int fc = 1;
    for (int c = 0; c < NSC; c++) {
      Key* y = &sum[k][c];
      if (!y->na && !y->ni) continue;
      oprintf("%s[%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]", fc ? "" : ",", c, y->na, y->ba, y->nf, y->bf, y->ni, y->bi, y->no, y->bo);
      fc = 0;
    }
    oprintf("],\"life\":[");
    fc = 1;
    for (int c = 0; c < NLIFE; c++) if (life[k][c]) { oprintf("%s[%d,%llu]", fc ? "" : ",", c, life[k][c]); fc = 0; }
    oprintf("],\"elem\":[");
    fc = 1;
    for (int c = 0; c < NSC; c++) if (elem[k][c]) { oprintf("%s[%d,%llu]", fc ? "" : ",", c, elem[k][c]); fc = 0; }
    oprintf("]}");
  }
  oprintf("],\"strays\":[");
  for (int m = 0; m < nstray; m++) oprintf("%s[%llu,%llu,%llu,%llu,%llu,%llu]", m ? "," : "", stray[m].n, stray[m].b,
                                        stray[m].ra[0], stray[m].ra[1], stray[m].ra[2], stray[m].ra[3]);
  oprintf("]}\n");
  oflush();
}

// ---- the C runtime's own entry points under malloc and free. The heap functions of the
// import library come in one object, so a program that needs any of these from it would
// bring all of them in beside ours; defining them here (untagged forwards) keeps that
// object out. A block from one is foreign to the tag check, which is what it is.
static fn_malloc r_malloc_base; static fn_calloc r_calloc_base; static fn_realloc r_realloc_base; static fn_free r_free_base;
static void init_base(void) {
  if (r_free_base) return;
  HMODULE m = GetModuleHandleA("ucrtbase.dll");
  r_malloc_base = (fn_malloc)GetProcAddress(m, "_malloc_base");
  r_calloc_base = (fn_calloc)GetProcAddress(m, "_calloc_base");
  r_realloc_base = (fn_realloc)GetProcAddress(m, "_realloc_base");
  r_free_base = (fn_free)GetProcAddress(m, "_free_base");
}
void* __cdecl _malloc_base(size_t n) { INIT(); init_base(); return r_malloc_base(n); }
void* __cdecl _calloc_base(size_t c, size_t n) { INIT(); init_base(); return r_calloc_base(c, n); }
void* __cdecl _realloc_base(void* p, size_t n) { INIT(); init_base(); return r_realloc_base(p, n); }
void __cdecl _free_base(void* p) { INIT(); init_base(); r_free_base(p); }

// Code compiled against the DLL-import declarations (a C file of the runtime, a package's C
// object) calls these through __imp_ pointers, which the import library supplies from the
// same object as the functions themselves: that would bring the C runtime's own malloc in
// beside ours (LNK2005). These definitions answer the __imp_ names with our functions.
#define IMP(name) void* const __imp_##name = (void*)&name
IMP(malloc); IMP(calloc); IMP(realloc); IMP(free); IMP(_msize); IMP(_recalloc); IMP(_expand);
IMP(_aligned_malloc); IMP(_aligned_free); IMP(_aligned_realloc); IMP(_aligned_msize);
IMP(_malloc_base); IMP(_calloc_base); IMP(_realloc_base); IMP(_free_base);
