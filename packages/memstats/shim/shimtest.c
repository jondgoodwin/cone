// The shim's own test: tags, foreign blocks, realloc, aligned blocks, many threads.
// Built and run by build.ps1 -Test (the shim linked ahead of the C runtime).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include "memstats.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void tags(void) {
  MemstatsInfo i;
  memstats_asking(7, 24);
  char* p = (char*)malloc(100);
  memstats_asking(0, 0);
  CHECK(p && memstats_peek(p, &i));
  CHECK(i.size == 100 && i.kind == 7 && i.padding == 0 && i.growths == 0 && i.time != 0);
  CHECK(((uintptr_t)p & 15) == 0);
  CHECK(_msize(p) == 100);
  uint64_t n, b; memstats_live(7, &n, &b); CHECK(n == 1 && b == 100);
  uint64_t t0 = i.time;
  // outermost asker wins
  memstats_asking(9, 8); memstats_asking(22, 1);
  void* q = calloc(4, 10);
  memstats_asking(0, 0);
  CHECK(memstats_peek(q, &i) && i.kind == 9 && i.size == 40);
  for (int k = 0; k < 40; k++) CHECK(((char*)q)[k] == 0);
  free(q);
  // stray: no kind
  void* s = malloc(5); CHECK(memstats_peek(s, &i) && i.kind == 0); free(s);
  // realloc grows, carries the first time, counts growth, may move
  memset(p, 0x5a, 100);
  char* r = (char*)realloc(p, 100000);
  CHECK(r && memstats_peek(r, &i) && i.size == 100000 && i.growths == 1 && i.time == t0 && i.kind == 7);
  CHECK(r[0] == 0x5a && r[99] == 0x5a);
  r = (char*)realloc(r, 50);
  CHECK(memstats_peek(r, &i) && i.size == 50 && i.growths == 1);
  memstats_live(7, &n, &b); CHECK(n == 1 && b == 50);
  CHECK(memstats_counter(MS_REALLOCS, 7) == 2 && memstats_counter(MS_GROWN, 7) == 1);
  free(r);
  memstats_live(7, &n, &b); CHECK(n == 0 && b == 0);
  CHECK(realloc(NULL, 8) != NULL);
}

static void foreign(void) {
  MemstatsInfo i;
  char* d = _strdup("a foreign block made by the C runtime");
  CHECK(d && !memstats_peek(d, &i));
  uint64_t before = memstats_counter(MS_FOREIGN_FREES, 0);
  CHECK(_msize(d) >= 30);
  d = (char*)realloc(d, 200);
  CHECK(d && strcmp(d, "a foreign block made by the C runtime") == 0);
  free(d);
  CHECK(memstats_counter(MS_FOREIGN_FREES, 0) == before + 1);
  // a block that starts a page, whose predecessor is not committed: the guarded read must not crash
  char* page = (char*)VirtualAlloc(NULL, 2 * 4096, MEM_RESERVE, PAGE_NOACCESS);
  VirtualAlloc(page + 4096, 4096, MEM_COMMIT, PAGE_READWRITE);
  CHECK(!memstats_peek(page + 4096, &i));
  VirtualFree(page, 0, MEM_RELEASE);
}

static void aligned(void) {
  MemstatsInfo i;
  memstats_asking(11, 0);
  void* a = _aligned_malloc(1000, 4096);
  memstats_asking(0, 0);
  CHECK(a && ((uintptr_t)a & 4095) == 0 && memstats_peek(a, &i) && i.kind == 11 && i.size == 1000);
  CHECK(i.padding > 0);
  memset(a, 1, 1000);
  void* b = _aligned_realloc(a, 5000, 4096);
  CHECK(b && ((uintptr_t)b & 4095) == 0 && ((char*)b)[999] == 1 && memstats_peek(b, &i) && i.size == 5000);
  _aligned_free(b);
  uint64_t n, by; memstats_live(11, &n, &by); CHECK(n == 0 && by == 0);
  void* c = _aligned_malloc(64, 64); CHECK(c && ((uintptr_t)c & 63) == 0); free(c);   // free() takes a tagged aligned block
  void* e = _aligned_malloc(64, 16); CHECK(e); _aligned_free(e);
  CHECK(_aligned_malloc(8, 3) == NULL);
}

#define NT 16
#define ROUNDS 20000
static void* volatile handoff[NT];
static DWORD WINAPI worker(LPVOID arg) {
  int id = (int)(intptr_t)arg;
  memstats_actor((size_t)(id + 1) * 64);
  for (int r = 0; r < ROUNDS; r++) {
    memstats_asking(30 + (id & 3), 16);
    size_t n = 16 + (r & 255);
    unsigned char* p = (unsigned char*)malloc(n);
    memstats_asking(0, 0);
    memset(p, id, n);
    if (r % 7 == 0) {                       // pass to the next thread's slot, free what it left
      void* old = InterlockedExchangePointer((PVOID volatile*)&handoff[(id + 1 + r / 7) % NT], p);
      if (old) free(old);
    } else {
      for (size_t k = 0; k < n; k++) if (p[k] != id) { printf("corrupt\n"); fails++; break; }
      p = (unsigned char*)realloc(p, n * 2);
      free(p);
    }
  }
  return 0;
}
static void threads(void) {
  HANDLE h[NT];
  for (int i = 0; i < NT; i++) h[i] = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
  WaitForMultipleObjects(NT, h, TRUE, INFINITE);
  uint64_t other = 0, allocs = 0, frees = 0;
  for (int k = 30; k < 34; k++) { other += memstats_counter(MS_FREED_OTHER_WORKER, k); allocs += memstats_counter(MS_ALLOCS, k); }
  for (int i = 0; i < NT; i++) if (handoff[i]) { free(handoff[i]); handoff[i] = 0; }
  for (int k = 30; k < 34; k++) frees += memstats_counter(MS_FREES, k);
  CHECK(allocs == (uint64_t)NT * ROUNDS);
  CHECK(frees == allocs);
  CHECK(other > 0);
  for (int k = 30; k < 34; k++) { uint64_t n, b; memstats_live(k, &n, &b); CHECK(n == 0 && b == 0); }
  printf("threads: %llu allocs, %llu freed by another worker\n", (unsigned long long)allocs, (unsigned long long)other);
}

int main(void) {
  tags(); foreign(); aligned(); threads();
  printf(fails ? "FAILED %d\n" : "memstats shim test: all passed\n", fails);
  return fails != 0;
}
