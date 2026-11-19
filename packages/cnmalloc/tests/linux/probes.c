// probes.c: the Linux probes of row 550 step 2, plain C programs linked with the shim and nothing of Cone's.
//   cc -O1 -pthread -fPIC tests/linux/probes.c <dir>/cn_shim.o <dir>/libcnpkg.a -ldl -o probes
//   ./probes first      the first allocation in a fresh process does not recurse (a hang is the failure: run it under `timeout`).
//                       Allocates, frees and reallocs a few blocks, waits for the give-back timer's thread to be made and to fire (CNMALLOC
//                       giveback=now or delay:<ms> arms it at the first free), and prints how many bytes of the bootstrap buffer were
//                       ever used (0: the start-up allocated nothing, so the buffer was belt and braces).
//   ./probes cthread    threads made by C code (pthread_create) allocate and free, every size class and large blocks, each block stamped and
//                       checked, half of the blocks freed by another thread; prints the counts and "ok" or the first failure.
//   ./probes foreign    a block from __libc_malloc is freed, resized and measured through the shim's names (the foreign path).
//   ./probes names      malloc_usable_size, memalign, aligned_alloc, posix_memalign, valloc, pvalloc, reallocarray: alignment and size.
#define _GNU_SOURCE
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern size_t cnmalloc_bootUsed(void);
extern void* __libc_malloc(size_t);
extern void* __libc_realloc(void*, size_t);
extern void __libc_free(void*);
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }

static int first(void) {
  void* a = malloc(100); void* b = calloc(10, 10); void* c = malloc(70000);
  if (!a || !b || !c) { puts("first: allocation failed"); return 1; }
  memset(a, 1, 100); memset(c, 2, 70000);
  b = realloc(b, 5000);
  free(a); free(b); free(c);             // the first free leaves memory waiting: the timer is made here, from inside a free
  msleep(1500);                           // and fires (giveback=now: 1 ms, delay: its delay)
  void* d = malloc(10); free(d);
  printf("first: ok; bootstrap buffer used %zu bytes\n", cnmalloc_bootUsed());
  return 0;
}

#define NT 8
typedef struct { int id; long rounds; volatile int fail; void* inbox[256]; pthread_mutex_t m; int n; } W;
static W w[NT];
static size_t sizeAt(unsigned x) { static const size_t s[] = {8, 16, 24, 100, 256, 1000, 4096, 20000, 32768, 40000, 100000, 300000}; return s[x % 12]; }
static void stamp(unsigned char* p, size_t n, int id) { p[0] = (unsigned char)id; p[n - 1] = (unsigned char)(id + 1); }
static int check(unsigned char* p, size_t n, int id) { return p[0] == (unsigned char)id && p[n - 1] == (unsigned char)(id + 1) && malloc_usable_size(p) >= n; }
static void* worker(void* a) {
  W* me = a; unsigned r = 12345u + me->id * 977u;
  for (long i = 0; i < me->rounds && !me->fail; i++) {
    r = r * 1664525u + 1013904223u;
    size_t n = sizeAt(r >> 8);
    unsigned char* p = malloc(n);
    if (!p) { me->fail = 1; break; }
    stamp(p, n, me->id);
    if (r & 1) {                          // hand the block to another thread's inbox (it frees it): a remote free
      W* o = &w[(me->id + 1) % NT];
      pthread_mutex_lock(&o->m);
      if (o->n < 256) { o->inbox[o->n++] = p; p = 0; }
      pthread_mutex_unlock(&o->m);
      // the receiver does not know n or the id: it only frees; the check below is for our own blocks
    }
    if (p) { if (!check(p, n, me->id)) { me->fail = 2; break; } free(p); }
    pthread_mutex_lock(&me->m);
    while (me->n > 0) { void* q = me->inbox[--me->n]; free(q); }
    pthread_mutex_unlock(&me->m);
  }
  return 0;
}
static int cthread(void) {
  pthread_t t[NT];
  for (int i = 0; i < NT; i++) { w[i].id = i; w[i].rounds = 200000; pthread_mutex_init(&w[i].m, 0); }
  for (int i = 0; i < NT; i++) pthread_create(&t[i], 0, worker, &w[i]);
  for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
  int bad = 0;
  for (int i = 0; i < NT; i++) if (w[i].fail) { printf("cthread: thread %d failed (%d)\n", i, w[i].fail); bad = 1; }
  if (!bad) printf("cthread: ok; %d threads x %ld rounds; bootstrap buffer used %zu bytes\n", NT, w[0].rounds, cnmalloc_bootUsed());
  return bad;
}

static int foreign(void) {
  unsigned char* p = __libc_malloc(100); memset(p, 7, 100);
  if (malloc_usable_size(p) < 100) { puts("foreign: malloc_usable_size of a foreign block is short"); return 1; }
  p = realloc(p, 5000);                   // a foreign block stays foreign
  if (!p || p[99] != 7) { puts("foreign: realloc lost the bytes"); return 1; }
  free(p);
  unsigned char* q = __libc_malloc(300);  // freed by the shim's free: must reach __libc_free, not crash
  free(q);
  void* r = malloc(50); r = realloc(r, 200000); free(r);
  puts("foreign: ok");
  return 0;
}

static int names(void) {
  int bad = 0;
  for (size_t a = 16; a <= 65536; a *= 2) {
    void* p = memalign(a, 100); if (!p || (uintptr_t)p % a) { printf("memalign(%zu) -> %p\n", a, p); bad = 1; } free(p);
    p = aligned_alloc(a, a * 2); if (!p || (uintptr_t)p % a) { printf("aligned_alloc(%zu) -> %p\n", a, p); bad = 1; } free(p);
    void* q = 0; int e = posix_memalign(&q, a < 8 ? 8 : a, 1000); if (e || !q || (uintptr_t)q % a) { printf("posix_memalign(%zu) -> %d %p\n", a, e, q); bad = 1; } free(q);
  }
  void* v = valloc(10); if (!v || (uintptr_t)v % 4096) { puts("valloc"); bad = 1; } free(v);
  v = pvalloc(10); if (!v || (uintptr_t)v % 4096 || malloc_usable_size(v) < 4096) { puts("pvalloc"); bad = 1; } free(v);
  v = reallocarray(0, 10, 10); if (!v || malloc_usable_size(v) < 100) { puts("reallocarray"); bad = 1; } free(v);
  if (reallocarray(0, (size_t)1 << 62, 8) || errno != ENOMEM) { puts("reallocarray overflow"); bad = 1; }
  if (posix_memalign(&v, 3, 10) != EINVAL) { puts("posix_memalign(3)"); bad = 1; }
  if (malloc_usable_size(0) != 0) { puts("usable(0)"); bad = 1; }
  void* z = calloc(1000, 1000); int zero = 1; for (size_t i = 0; i < 1000000; i += 4096) if (((char*)z)[i]) zero = 0; free(z);
  if (!zero) { puts("calloc not zero"); bad = 1; }
  if (!bad) puts("names: ok");
  return bad;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  if (!strcmp(argv[1], "first")) return first();
  if (!strcmp(argv[1], "cthread")) return cthread();
  if (!strcmp(argv[1], "foreign")) return foreign();
  if (!strcmp(argv[1], "names")) return names();
  return 2;
}
