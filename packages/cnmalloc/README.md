# cnmalloc

One shared heap for a process, in Cone, that replaces the C runtime's `malloc` family at the link. Standard C
allocation API only. **Off by default:** nothing links it unless a program asks.

## What is here

| File | What it is |
|---|---|
| `src/os.cone` | `mod trait Os`: reserve, commit, decommit, release. The OS layer's interface. |
| `src/oswin.cone` | The Windows implementation (`VirtualAlloc`, `VirtualFree`) and the few other calls the heap makes: clock, SRW lock, thread-pool timer, stderr. Linux's `mmap` is named in `os.cone`'s comment, not built. |
| `src/cnmalloc.cone` | The heap (a C-named module: no prelude, no core, no globals, nothing from Cone's runtime). Its header comment is the design: the page pool and reservation interfaces, the lock order, the concurrency rule. |
| `shim/cn_shim.c` | The C part (language gaps, below): the one global heap pointer, the lazy compare-and-swap start, the `malloc`/`free`/... names, the `__imp_` pointers, the exit-time statistics hook, the workers' cache, and (row 454) the second design's common paths. |
| `shim/build-cn.ps1` | Builds `cn.lib` (the package's release object plus `cn_shim.c`) and `cnd.lib` (the same with `-DCN_HOOKS -DCN_DEBUG`: the second design's test hooks, and a double free that aborts). |
| `tests/` | `edges` (class table, aligned to 1 MiB, API edges, large grow/shrink, foreign blocks, struct layout), `dirty` (the dirty-page test under each give-back setting), `stress` (threads, cross-thread frees, realloc, pattern checks), `idle` (give-back by the timer when the process goes quiet), `spare` (at `giveback=now`, a one-block loop makes no system calls and the page goes back after a quiet gap), `cache` (the workers' cache, through the C runtime's malloc family: checks only when linked with `cn.lib` and `CNMALLOC=cache=<n>`; `CNCACHE_SECS`, `CNCACHE_THREADS`, `CNCACHE_MODE=random\|pc`), and `edges`' batch checks. Row 454's three need the second design linked (`cnd.lib`, `CNMALLOC=design=2,stats,giveback=now`; each says how in its header, and each passes without testing under a plain `congo test`): `lfrace` (retire against late allocation: Rule G undos must be counted, thousands of them, or it fails), `lfpage` (a free raced against a detach: the page must be listed afterwards, every round), `doublefree` (the debug build's double-free check). |

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
| `giveback=now` / `delay:<ms>` / `never` | `delay:1000` | When empty pages and freed large blocks are decommitted. Address space is never released (but see Large blocks). At `now`, memory no live block uses goes back at the next collect (a worker's park, or a 1 ms timer, the shortest the thread-pool timer takes), within a few milliseconds of the process going quiet, not on the free itself. Each class keeps its last empty page until then, so a loop that allocates and frees one block makes no commit or decommit calls after its first pair. That is at most one page per class beyond a delay of 0: 72 x 64 KiB at worst. |
| `reserve=<n>[k\|m\|g]` | `64g` | The address space for small-block pages (64 MiB to 1 TiB). |
| `largecap=<n>[k\|m\|g]` | `64m` | Committed bytes of freed large blocks kept for reuse. |
| `largereserve=<n>` | `2` | Row 446: a large block's own reservation is n times its size (n from 1 to 64; out of range is clamped), so a block that grows stays in place until it outgrows that. At 2 the reservation is the size plus the size, as it always was. |
| `largereservecap=<n>[k\|m\|g]` | `64m` | The most a reservation may exceed its block's size (1 MiB to 64 GiB). With the default, `largereserve=32` stops helping above 2 MiB blocks; raise this to let big blocks keep their room. |
| `stats` | off | Print the counters to stderr at exit (no allocation). |
| `design=2` | off (the lock design) | Row 454: the second design of the general set, no lock on the common path (below). Nothing else about the lock design changes when it is off. |
| `slots=<n>` | `8` | Design 2: the most slots a class may hold (1 to 8, never more than the processor count). |
| `collisions=<n>` | `64` | Design 2: lost compare-and-swaps on a class, since its slot count last changed, that add a slot. |
| `cache=<n>` | `0` (off) | Row 426: blocks per size class that a worker thread may keep in its own cache (see below). 0 touches no thread-local. |
| `cache_max=<bytes>` | `1024` | Only requests up to this size, and their classes, are cached (at most 32768). |

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

## The second design (row 454): `CNMALLOC=design=2`

The specification is `WI\cnmalloc-second-design-spec.md`; the comment above "THE SECOND DESIGN" in `src/cnmalloc.cone` and
the one above `lf_alloc` in `shim/cn_shim.c` hold the layout and the rules. Off by default; with it off nothing here is reached.
One shared heap, the same 64 KiB pages, size classes, large blocks, page pool and give-back; the lock per class stays, for the slow
paths only.

* **A page's free cells are a bitmap** in a second descriptor (576 bytes a page: `genclass`, `state`, `ncells`, the page's address,
  64 words of bits, set means free), committed for good as the page comes into use, so a stale pointer to it is always readable.
* **Allocate is one compare-and-swap** (`lf_alloc`): a thread takes its class's slot (`tnum mod active slots`), reads the slot's
  page and its `genclass`, clears the lowest free bit of a word, and reads `genclass` again (Rule G: if it changed, the page was
  retired and came back to life in between, so the bit is given back by the ordinary free and the thread starts again). Two plain loads
  and no atomic besides the compare-and-swap.
* **Free is one `fetch_or`** (`lf_free`, `lf_release`): the page is found by arithmetic on the address and `genclass` is read
  *before* the `fetch_or`. Only when the word was empty before (the page may have been full) or is now wholly free (the page may be
  empty), and the page is not in a slot, does the free go to the class lock ("after a free"), carrying the `genclass` it read, which
  the slow path re-checks under the lock.
* **Slow paths, under the class lock** (`cnm_lf_attach`, `cnm_lf_after_free`, `lfDetach`, `lfRetire`, `lfCollect` in Cone): a
  slot's page has no free bit (detach it, attach the lowest page of the class's tree that has a free bit, else the spare, else a
  page from the pool); a full page gets a cell back (listed in the tree); a page is retired. The Dekker rule: the lock holder
  exchanges a page's `state` and only then scans its bits; a freer ORs its bit and only then reads `state`. A page is retired by
  claiming every word with a compare-and-swap from its mask to 0 (put back with `fetch_or` if one fails), then its generation rises.
* **Slots**: a class starts with one; a class whose slot pages lose `collisions` compare-and-swaps adds one, up to `slots`. A slot
  not used for the give-back delay is dropped by collect (its page detached; slot 0 keeps its page unless it is wholly free).
* **`calloc` fills every small cell** (the written-mark of the lock design is not kept); large blocks keep their skip-fill.
* **The worker cache, mended** (`cache=<n>` with this design): a worker is parked by the actors scheduler with a timeout of the
  give-back delay, and empties its cache and collects only if that passes with no work; a fill is 2 cells on the first miss after the
  cache was empty, doubling up to half the cap; small `realloc`, `_recalloc` and the aligned calls go through the cache.
* **The memory bound.** Beyond what the lock design held, this design can hold at most one partly used 64 KiB page for each slot
  above the first, in each class where collisions were seen. With the default cap and all 72 classes contended at once that is
  72 x 7 x 64 KiB, about 32 MiB, and it falls back within one give-back delay of the contention ending. Slot 0's page of each class is
  also held while the class is in use (as the lock design's current page is); a wholly free one goes back to the pool a delay after
  the last allocation from it. The statistics print the active slots of each class and the count of attached pages that are wholly free.
* **Counters** (`CNMALLOC=stats`, never a timed run): per class allocations and frees, collisions (one atomic add each on the common
  paths, only then), slots added and dropped, pages attached, listed and retired, retires abandoned, Rule G undos; `calloc` calls and
  bytes filled; cache hits, misses and fills; small allocations by threads that are not workers.

## Counters

`cnm_stats(heap, buffer, max)` copies 64-bit counters out: per class (72): allocations, frees, live cells, active
pages, lock waits (a lock take whose first try failed); then the pool (committed bytes, empty pages, decommitted
pages, pages reused, commit calls, decommit calls); then large blocks (live, committed bytes, in-place grows, moves,
cache hits, system calls); then foreign blocks forwarded; then the workers' caches (bytes held now, hits, misses,
batch returns, batch fills; published by the shim when the counters are read); then (row 446) bytes copied by large-block
moves and the large blocks' system calls by kind (reserve, commit, decommit, release; they add up to the system-call count).
Cache hits are blocks served from the one-second cache of freed large blocks. `CNMALLOC=stats` prints them at exit; `cn_print_lock_waits()`
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
10. (Row 454) The second design's common paths are C: `lf_alloc` (section 3, with Rule G), `lf_free` and `lf_release`
    (section 4), `lf_classof`, `lf_cellclass`, `lf_setup`, `lf_take_class`, `lf_realloc_small`, `cn_lf_give_chain`, and, in the
    debug and hook builds, `lf_double_free` and the `LF_HOOK` calls. They are C because they keep a thread-local (a number for the
    thread, and where it started in each class's page: gap 8 again) and because the test hooks must cost nothing in the library
    that ships. The atomic operations themselves are not a gap: a C-named module gets `mem.atomicLoad`, `atomicSwap`, `atomicOr`
    and `atomicCompareSwap` by `import core use mem, MemOrder;` (found building row 454; core adds no symbol to `cnmalloc.obj`),
    and the second design's slow paths, which are all under the class lock, are Cone and use them. Gap 2 (the lazy start) is
    unchanged. The worker cache's changes (a fill sized to demand, small `realloc`/`_recalloc`/aligned cells through the cache) are in
    `cn_cache_alloc_cl`, `cn_cache_free` and `cn_cache_flush`.
11. (Row 454) The shim's exports for tests and for the actors scheduler, found by `GetProcAddress` on the program's own module
    as gap 9: `cnmalloc_workerParkMs` (the park timeout, 0 unless the second design is on), and for the tests only `cnmalloc_design`,
    `cnmalloc_heap`, `cnmalloc_collect`, `cnmalloc_lfTotal`, `cnmalloc_lfActive`, and in `cnd.lib` `cnmalloc_lfSetHooks` and
    `cnmalloc_setDoubleFreeHandler`.
