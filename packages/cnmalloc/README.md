# cnmalloc

One shared heap for a process, in Cone, that replaces the C runtime's `malloc` family at the link. Standard C
allocation API only. **Off by default:** nothing links it unless a program asks.

## What is here

| File | What it is |
|---|---|
| `src/os.cone` | `mod trait Os`: reserve, commit, decommit, release. The OS layer's interface. |
| `src/oswin.cone` | The Windows implementation (`VirtualAlloc`, `VirtualFree`) and the few other calls the heap makes: clock, SRW lock, thread-pool timer, stderr. Linux's `mmap` is named in `os.cone`'s comment, not built. |
| `src/cnmalloc.cone` | The heap (a C-named module: no prelude, no core, no globals, nothing from Cone's runtime). Its header comment is the design: the page pool and reservation interfaces, the lock order, the concurrency rule. |
| `shim/cn_shim.c` | The C part (language gaps, below): the one global heap pointer, the lazy compare-and-swap start, the `malloc`/`free`/... names, the `__imp_` pointers, the exit-time statistics hook. |
| `shim/build-cn.ps1` | Builds `cn.lib` (the package's release object plus `cn_shim.c`). |
| `tests/` | `edges` (class table, aligned to 1 MiB, API edges, large grow/shrink, foreign blocks, struct layout), `dirty` (the dirty-page test under each give-back setting), `stress` (threads, cross-thread frees, realloc, pattern checks), `idle` (give-back by the timer when the process goes quiet), `cache` (the workers' cache, through the C runtime's malloc family: checks only when linked with `cn.lib` and `CNMALLOC=cache=<n>`; `CNCACHE_SECS`, `CNCACHE_THREADS`, `CNCACHE_MODE=random\|pc`), and `edges`' batch checks. |

## Linking it

```
powershell -File packages\cnmalloc\shim\build-cn.ps1 -Out <dir>        # writes <dir>\cn.lib
```

* A Cone program, the language suite or a package's tests: put `cn.lib` after the program's objects and before the
  default libraries with the `_LINK_` environment variable (`link.exe` appends it to its command line), e.g.
  `$env:_LINK_ = '"<dir>\cn.lib"'`. (Not `LINK`: that puts the shim ahead of `conestd.lib`, so conestd's own `malloc`
  calls fall through to the C runtime.) Delete a package's `build\` folder to make congo relink.
* A C program: compile with `/D_ACRTIMP=` (else `malloc` is a dllimport and the swap silently does not happen) and
  link `cn.lib`.
* `aio`'s test `alloc` and `iocore`'s test `jobrecord` define their own counting `malloc` family and cannot link with
  any shim.

## Settings (`CNMALLOC` environment variable, comma separated)

| Setting | Default | Meaning |
|---|---|---|
| `giveback=now` / `delay:<ms>` / `never` | `delay:1000` | When empty pages and freed large blocks are decommitted. Address space is never released (but see Large blocks). |
| `reserve=<n>[k\|m\|g]` | `64g` | The address space for small-block pages (64 MiB to 1 TiB). |
| `largecap=<n>[k\|m\|g]` | `64m` | Committed bytes of freed large blocks kept for reuse. |
| `stats` | off | Print the counters to stderr at exit (no allocation). |
| `cache=<n>` | `0` (off) | Row 426: blocks per size class that a worker thread may keep in its own cache (see below). 0 touches no thread-local. |
| `cache_max=<bytes>` | `1024` | Only requests up to this size, and their classes, are cached (at most 32768). |

`CONE_HEAP_CLASS_SIZE=1`, a separate environment variable and not one of the `CNMALLOC` settings, is read by conestd's
heap region (`packages/conestd/heapregion.c`), not by cnmalloc: with it set, and cnmalloc linked, the heap answers a
size request with cnmalloc's class size (the shim's `cnmalloc_sizeFor`), so a collection made at an exact fit grows into
its cell before it moves. Off by default; Windows only; a trial.

## The workers' cache (row 426)

Only a thread the runtime declares a worker has one; every other thread goes straight to the shared heap. A free on a
worker pushes the block on a short LIFO list for its class, with no lock; an allocate pops. Over the cap, the older
half of the list goes back to the heap under one take of the class lock; on an empty list, one take of the class lock
fills the list to half the cap. A cached block is dirty, so `calloc` clears it. Large blocks and classes above
`cache_max` are never cached. The slack is bounded in bytes: with the default `cache_max`, about 240 KiB per worker at
`cache=7` and 1.1 MiB at `cache=32`.

**A cached block stays counted as used.** Taking a cell from the heap adds one to its page's used count (under the
class lock) and only the batch free subtracts it (under the class lock, retiring the page if that was the last live
cell). The cache never touches a page's used count, free list or bitmap, so a page with a cell in some cache cannot
retire, and retiring is exactly the heap's. A cached cell counts in the class's `live`; the per-class allocation and
free counters count heap takes and gives, not cache hits.

The runtime's three calls are exported from the program's own module when `cn.lib` is linked: `cnmalloc_workerBegin()`
(this thread is a worker), `cnmalloc_workerPark()` (empty the cache, then `cnm_collect`: give back what is due; the
heap's timer handles what falls due later) and `cnmalloc_workerEnd()` (empty the cache, free the worker's slot). The
actors runtime finds them by `GetProcAddress` on its own module when each worker starts, so a program without cnmalloc
calls nothing. A thread that ends without `workerEnd` keeps its cached cells counted as used for good. Up to 512
workers are cached at once; more run uncached.

## Counters

`cnm_stats(heap, buffer, max)` copies 64-bit counters out: per class (72): allocations, frees, live cells, active
pages, lock waits (a lock take whose first try failed); then the pool (committed bytes, empty pages, decommitted
pages, pages reused, commit calls, decommit calls); then large blocks (live, committed bytes, in-place grows, moves,
cache hits, system calls); then foreign blocks forwarded; then the workers' caches (bytes held now, hits, misses,
batch returns, batch fills; published by the shim when the counters are read). `CNMALLOC=stats` prints them at exit; `cn_print_lock_waits()`
prints the lock-wait lines the benchmark runner collects.

## Language gaps (what is in C and why)

1. A plain mutable global (the process's heap pointer): error 1626.
2. The compare-and-swap lazy start: no atomics in a prelude-less (C-named) module.
3. The `__imp_<name>` pointers: a data symbol holding a function's address has no spelling.
4. The exit-time statistics hook (`atexit`, registered only when `CNMALLOC=stats`): Cone has no spelling for handing
   a function to the C runtime's exit table from a prelude-less module.
5. Not a gap but a choice: the C names `malloc` and friends are defined in the C file too, so the library has one place
   that decides what a standard-API call does. Cone's C-named module could export them.
6. Found while building: a string literal cannot be cast to `*u8` (1013), so literals reach `memcpy`-like code
   through a `cstr` parameter of an extern with another C name (`strncpy`), and the settings parser compares with `strncmp`.
7. A module trait (`Os`) cannot be chosen by anything but naming the conforming module (`oswin`) in the code that
   uses it: there is no way yet to select the implementation by a build setting.
8. (Row 426) The workers' cache is a C thread-local: a C-named module has no globals, so no thread-locals. The calls
   into the heap that the cache needs (`cnm_cell_class`, `cnm_batch_alloc`, `cnm_batch_free`) are Cone.
9. (Row 426) The actors runtime reaches the three worker calls by `GetProcAddress` on the program's own module (the
   shim's `__declspec(dllexport)`), not by a weak symbol: a Cone package has no way to declare an extern that may be
   absent at link time.
