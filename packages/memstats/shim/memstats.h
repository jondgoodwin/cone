// memstats shim: the functions a program (or the shim's test) can call. Core's markers
// declare memstats_asking and memstats_actor as Cone externs; the rest are for tests.
#pragma once
#include <stddef.h>
#include <stdint.h>

void __cdecl memstats_asking(uint32_t kind, size_t elemSize);   // 0, 0 clears
void __cdecl memstats_actor(size_t actorHeaderAddress);          // 0 when no actor runs

typedef struct {
  uint64_t size, time;
  uint32_t kind, growths, actor, worker;
  size_t padding;
} MemstatsInfo;

void __cdecl memstats_live(uint32_t kind, uint64_t* n, uint64_t* bytes);
uint64_t __cdecl memstats_counter(int which, uint32_t kind);
int __cdecl memstats_peek(void* p, MemstatsInfo* out);          // 1 and the tag, or 0 for a foreign block
uint64_t __cdecl memstats_this_worker(void);

enum { MS_FREED_OTHER_ACTOR, MS_FREED_OTHER_WORKER, MS_GROWN, MS_MOVED, MS_REALLOCS, MS_FOREIGN_FREES, MS_ALLOCS, MS_FREES };
