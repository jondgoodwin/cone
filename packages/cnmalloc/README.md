# cnmalloc

One shared heap for a process, in Cone, that replaces the C runtime's `malloc` family at the link. Standard C
allocation API only. **Off by default:** nothing links it unless a program asks.

**Systems:** Windows (x64, MSVC) and, since row 550, **Linux (x64, glibc 2.34 or newer)**. The glibc floor is `dlsym` and
`pthread_atfork` living in libc itself (older glibc needs `-ldl`, and its `dlsym` allocated on first use), and `__libc_free`,
`__libc_realloc` and `__libc_memalign` being exported. Not supported: macOS, any other processor, a program that links the C library
statically (the shim replaces the names the dynamic loader resolves), and a shared library that loads its own allocator and
interposes `malloc` itself. Making cnmalloc the default on Linux is a separate decision.

## What is here

| File | What it is |
|---|---|
| `src/os.cone` | `mod trait Os`: reserve, commit, decommit, release (with the length). The OS layer's interface. |
| `src/oswin.cone` | The Windows implementation (`VirtualAlloc`, `VirtualFree`) and the few other calls the heap makes: clock, SRW lock, thread-pool timer, stderr, the settings text, the processor count, the C runtime's functions that foreign blocks go to. |
| `src/oslinux.cone` | The Linux implementation (row 550): `mmap` PROT_NONE over-mapped and trimmed to 64 KiB for a reserve, `mprotect` to commit, `madvise(MADV_DONTNEED)` plus PROT_NONE to decommit, `munmap` to release; a three-state futex lock on the low 32 bits of the lock word; the give-back timer as one thread of its own parked on a futex; `clock_gettime`, `write`, `_exit`, `getenv`, `sysconf`; foreign blocks to `__libc_free`/`__libc_realloc` and `dlsym(RTLD_NEXT, "malloc_usable_size")`. Each call was probed not to allocate (`Ruminations\cnmalloc-linux-step0-2026-11-19.md`). |
| `src/osplat.cone` | The one name the heap calls: each function forwards at compile time to `oswin` or `oslinux`. A platform with neither fails at link on `cnmalloc_has_no_os_layer_for_this_platform`. |
| `src/hosttest.cone` | What the tests ask of the system (sleep, clocks, stderr, a file append, `dlsym`/`GetProcAddress` of the program's own exports, `_msize`/`malloc_usable_size`, `setenv`, private bytes, a foreign block), on both systems, so no test names a system function. Part of the package's root module: `cnmalloc.cnm_host*`. |
| `src/cnmalloc.cone` | The heap (a C-named module: no prelude, no core, no globals, nothing from Cone's runtime). Its header comment is the design: the page pool and reservation interfaces, the lock order, the concurrency rule. |
| `shim/cn_shim.c` | The C part (language gaps, below): the one global heap pointer, the lazy compare-and-swap start, the `malloc`/`free`/... names, the `__imp_` pointers, the exit-time statistics hook, the workers' cache, and (row 454) the second design's common paths. |
| `shim/cn_plat.h` | (Row 550) The platform layer of `cn_shim.c`, which is one source for both systems: the thread-local, inline and calling-convention words and every atomic access. A shared word is never `volatile`: each load or store is a `cn_ld*`/`cn_st*` form with its ordering in its name (acquire, relaxed, sequentially consistent, release); on MSVC each is the volatile access or `_Interlocked*` intrinsic the shim used before, on GCC the `__atomic` builtin. `shim/volatile-sites.md` lists the 23 sites. |
| `shim/build-cn.ps1`, `shim/build-cn.sh` | Build the link shim. Windows: `cn.lib` (the package's release object plus `cn_shim.c`) and `cnd.lib` (the same with `-DCN_HOOKS -DCN_DEBUG`: the second design's test hooks, and a double free that aborts). Linux: `cn_shim.o`, `cn_shim_d.o` (the same two) and `libcnpkg.a` (the package's release object as an archive). |
| `tests/` | `edges` (class table, aligned to 1 MiB, API edges, large grow/shrink, foreign blocks, struct layout), `dirty` (the dirty-page test under each give-back setting), `stress` (threads, cross-thread frees, realloc, pattern checks), `idle` (give-back by the timer when the process goes quiet), `spare` (at `giveback=now`, a one-block loop makes no system calls and the page goes back after a quiet gap), `timer` (row 550: the timer's creation with a nested arm, on both systems), `forksafe` (row 550, Linux: fork while threads allocate), `cache` (the workers' cache, through the C runtime's malloc family: checks only when linked with the shim and `CNMALLOC=cache=<n>`; `CNCACHE_SECS`, `CNCACHE_THREADS`, `CNCACHE_MODE=random\|pc`), and `edges`' batch checks. **A test that needs the shim reports itself skipped, by name, when the shim is not linked (or its names are not exported), on both systems** (row 550: they used to pass without testing; a plain `congo test` is now 7 passed and 8 skipped, `cache`, `doublefree`, `lfmulti`, `lfpage`, `lfrace`, `lfreturn`, `lfstale` and `forksafe`), so a pass always means cnmalloc ran. Row 454's four need the second design linked (`cnd.lib` / `cn_shim_d.o`, `CNMALLOC=design=lockfree,stats,giveback=now`; each says how in its header): `lfrace` (retire against late allocation: Rule G undos must be counted, thousands of them, or it fails), `lfpage` (a free raced against a detach: the page must be listed afterwards, every round), `lfstale` (a freer whose page was retired and came back as another class's page between its `fetch_or` and "after a free" must not act on it: every step driven by two park points, deterministic, run with `slots=1`), `doublefree` (the debug build's double-free check, and with a cache a grouped return's). Row 506's two, with `cache=<n>`: `lfmulti` (a multi-bit fill whose compare-and-swap lands on the page's next life gives every bit back in one `fetch_or`, writes no cell, and, when its take emptied the word and the page was detached under it, leaves the page listed; driven, `slots=1`), `lfreturn` (a grouped return reads every link before its first release, so cells another thread allocates and overwrites meanwhile lead nowhere; driven). Variants: `CNRACE_WORKERS=1` (lfrace with worker fills), `CNPAGE_GROUPED=1`, `CNSTALE_GROUPED=1` (the free is a grouped return). `tests/mutants.ps1` (Windows) and `tests/mutants.py` (Linux), which read one data file, `tests/mutants.json`, break the shim twelve ways (Rule G's undo, Rule G's generation half, the freer's genclass read after its `fetch_or`, and row 506's m1 to m7 on the fill and the return) and checks that the test meant for each fails on it and that all of them pass unmutated; run it again after a change to the shim's common paths or to "after a free". |

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

**Linux** (row 550):

```
bash packages/cnmalloc/shim/build-cn.sh <dir>        # writes <dir>/cn_shim.o, cn_shim_d.o, libcnpkg.a
CONGO_LINK_EXTRA="<dir>/cn_shim.o <dir>/libcnpkg.a" python3 tools/congo/congo.py test        # any congo build or test
```

* `CONGO_LINK_EXTRA` is read by congo's Linux link only: the files go on the link line after the program's objects. The shim is a
  **plain object**, never an archive member (an archive member is pulled only for a symbol the program names, and a program need not
  name `malloc`) and never a shared library: its thread-locals are `-ftls-model=initial-exec`, so that no access calls
  `__tls_get_addr`, which allocates. `libcnpkg.a` supplies `cnm_*` to a program that does not import the package and is not pulled by
  one that does. The shim is built with `-fno-builtin-malloc` and friends (it is the definition of those names). Like `_LINK_`, the
  setting is not part of congo's cache key: delete the package's `build/` folder, or pass `--no-cache`, after changing it.
* Every congo link on Linux exports the shim's own names, `cnmalloc_*`, with `-Wl,--dynamic-list` (not `--export-dynamic`, which would
  export every symbol of every program), so that `dlsym(RTLD_DEFAULT, name)` finds them: the actors runtime and conestd's heap region
  look up `cnmalloc_workerBegin/Park/End/ParkMs` and `cnmalloc_sizeFor` that way, as they use `GetProcAddress` on Windows. A test that
  does not find a name reports itself **skipped**, saying so; it cannot tell "not linked" from "linked but not exported".
* **What the Linux shim adds.** `malloc_usable_size`, `memalign`, `aligned_alloc`, `posix_memalign`, `valloc`, `pvalloc` and
  `reallocarray` (the Windows `_msize`, `_aligned_*` family is Windows only). `errno` is set to `ENOMEM` when an allocation answers
  null. A **foreign block** (made by the dynamic loader or glibc's own start-up before the shim ran, or by `__libc_malloc`) is
  recognised by not being in the heap's reservation or its large table, and is freed, resized and measured by glibc through
  `__libc_free`, `__libc_realloc` and `malloc_usable_size` (looked up once, by `dlsym(RTLD_NEXT, ...)` on a name that exists; `dlerror`
  is never called, since a failed lookup allocates). A **bootstrap buffer** (8 KiB, static) serves the starting thread alone while
  the heap starts (`g_state` 1), should something under `pthread_atfork` or the timer's `pthread_create` allocate then; a block in it is
  never freed; `cnmalloc_bootUsed()` says how much of it was ever used.
* **Fork.** The shim registers `pthread_atfork` handlers: before the fork the calling thread takes every heap lock in the heap's one
  order (each class lock by number, the pool's, the large table's, the timer's), the parent and the child release them, and the child
  forgets its timer thread (a later arm makes another). `tests/forksafe.cone` forks 60 times while four threads allocate and checks that
  every child can allocate.
* **The timer.** One thread of the heap's own, made by `pthread_create` on the first arm and parked on a futex with a timeout. It is
  created with the heap's timer lock **not held**, behind a sentinel handle (a nested arm, from the allocation that `pthread_create`
  makes, returns and the creator arms right after); this is in the shared code, and `tests/timer.cone` forces a nested arm on both systems.
* **Decommit and the mapping count.** Decommit is `madvise(MADV_DONTNEED)` then `mprotect(PROT_NONE)`, so a stale access faults as on Windows.
  Each alternation of committed and decommitted pages is a separate mapping, and a process has `vm.max_map_count` of them (usually
  65,530). The PR reports the peak line count of `/proc/self/maps` of the stress run and of bintrees. If it nears a tenth of the
  limit, the switch is to `madvise` alone (still zero on recommit, one mapping; a stale access then reads zero instead of faulting).

## Settings (`CNMALLOC` environment variable, comma separated)

| Setting | Default | Meaning |
|---|---|---|
| `giveback=now` / `delay:<ms>` / `never` | `delay:1000` | When empty pages and freed large blocks are decommitted. Address space is never released (but see Large blocks). At `now`, memory no live block uses goes back at the next collect (a worker's park, or a 1 ms timer, the shortest the thread-pool timer takes), within a few milliseconds of the process going quiet, not on the free itself. Each class keeps its last empty page until then, so a loop that allocates and frees one block makes no commit or decommit calls after its first pair. That is at most one page per class beyond a delay of 0: 72 x 64 KiB at worst. |
| `reserve=<n>[k\|m\|g]` | `64g` | The address space for small-block pages (64 MiB to 1 TiB). |
| `largecap=<n>[k\|m\|g]` | `64m` | Committed bytes of freed large blocks kept for reuse. |
| `largeslack=<n>[k\|m\|g]` | `256k` | Row 547. A large block taken from the cache of freed blocks keeps at most its request plus this many bytes committed while it lives (rounded up to 4 KiB); the rest of its committed tail is decommitted. The bound: no live large block holds more than request + `largeslack`. `0` decommits the whole tail (the rule before row 547, which made a growing `List` fault the same pages twice: havlak took 80,000 page faults, 3,100 with the slack). Why `256k`: it is the smallest slack that keeps the whole saving (havlak's faults and decommits match no trim at all), so the saving doesn't hang on one program's sizes; `128k` keeps 99.7% but sits on the knee, still decommitting and regrowing about 1,070 times on havlak. It costs about 2 MiB more than `128k` on a workload built to stress the bound (+4.1 MiB, +1.7% over `0`). |
| `largereserve=<n>` | `2` | Row 446: a large block's own reservation is n times its size (n from 1 to 64; out of range is clamped), so a block that grows stays in place until it outgrows that. At 2 the reservation is the size plus the size, as it always was. |
| `largereservecap=<n>[k\|m\|g]` | `64m` | The most a reservation may exceed its block's size (1 MiB to 64 GiB). With the default, `largereserve=32` stops helping above 2 MiB blocks; raise this to let big blocks keep their room. |
| `stats` | off | Print the counters to stderr at exit (no allocation). |
| `design=lockfree` / `design=lock` | `lock` | Row 454: `lockfree` is the second design of the general set, no lock on the common path (below); `lock` is the first (the default). Nothing about the lock design changes when `lockfree` is off. |
| `slots=<n>` | `min(processors, 32)` | `design=lockfree`: the most slots a class may hold, each slot one partly used page the threads allocate from (1 to 32; an explicit value is clamped to 32 only, not to the processor count). More than 32 is not supported: nothing above 16 threads has been measured. It bounds the memory the design holds beyond the lock design (below). |
| `collisions=<n>` | `64` | `design=lockfree`: lost compare-and-swaps on a class, since its slot count last changed, that add a slot (up to `slots`). It bounds how fast the slot count grows. |
| `cache=<n>` | `0` (off) | Row 426: blocks per size class that a worker thread may keep in its own cache (see below). 0 touches no thread-local. |
| `cache_max=<bytes>` | `1024` | Only requests up to this size, and their classes, are cached (at most 32768). |

`CONE_HEAP_CLASS_SIZE=1`, a separate environment variable and not one of the `CNMALLOC` settings, is read by conestd's
heap region (`packages/conestd/heapregion.c`), not by cnmalloc: with it set, and cnmalloc linked, the heap answers a
size request with cnmalloc's class size (the shim's `cnmalloc_sizeFor`), so a collection made at an exact fit grows into
its cell before it moves. Off by default; a trial (Windows and Linux).

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
actors runtime finds them by `GetProcAddress` on its own module (`dlsym(RTLD_DEFAULT)` on Linux) when each worker starts, so a program without cnmalloc
calls nothing. A thread that ends without `workerEnd` keeps its cached cells counted as used for good. Up to 512
workers are cached at once; more run uncached.

## The second design (row 454): `CNMALLOC=design=lockfree`

The specification is `WI\cnmalloc-second-design-spec.md`; the comment above "THE SECOND DESIGN" in `src/cnmalloc.cone` and
the one above `lf_alloc` in `shim/cn_shim.c` hold the layout and the rules. Off by default; with it off nothing here is reached.
One shared heap, the same 64 KiB pages, size classes, large blocks, page pool and give-back; the lock per class stays, for the slow
paths only.

* **A page's free cells are a bitmap** in a second descriptor (576 bytes a page: `genclass`, `state`, `ncells`, the page's address,
  64 words of bits, set means free), committed for good as the page comes into use, so a stale pointer to it is always readable.
* **Allocate is one compare-and-swap** (`lf_alloc`): a thread takes its class's slot (`tnum mod active slots`), reads the slot's
  page and its `genclass`, clears the lowest free bit of a word, and reads `genclass` again (Rule G: if it changed, the page was
  retired and came back to life in between, so the bit is given back by the ordinary free and the thread starts again). Two plain loads
  and no atomic besides the compare-and-swap. A thread that loses the swap goes on to the next word, and makes at most 4 passes over the
  page after a lost swap before it gives the page up (each pass re-runs the whole load, swap and load of Rule G).
* **Free is one `fetch_or`** (`lf_free`, `lf_release`): the page is found by arithmetic on the address and `genclass` is read
  *before* the `fetch_or`. Only when the word was empty before (the page may have been full) or is now wholly free (the page may be
  empty), and the page is not in a slot, does the free go to the class lock ("after a free"), carrying the `genclass` it read, which
  the slow path re-checks under the lock.
* **Slow paths, under the class lock** (`cnm_lf_attach`, `cnm_lf_after_free`, `lfDetach`, `lfRetire`, `lfCollect` in Cone): a
  slot's page has no free bit (detach it, attach the lowest page of the class's tree that has a free bit, else the spare, else a
  page from the pool); a full page gets a cell back (listed in the tree); a page is retired. The Dekker rule: the lock holder
  exchanges a page's `state` and only then scans its bits; a freer ORs its bit and only then reads `state`. A page is retired by
  claiming every word with a compare-and-swap from its mask to 0 (put back with `fetch_or` if one fails), then its generation rises.
* **Slow path, step 1.** Under the lock, if the slot still holds the page the thread saw, the thread loads the page's bits words
  and, if any bit is set, unlocks and retries the common path instead of detaching (it may have read zeros only while another lock
  holder was retiring the page or making it ready; without this check two allocators can retire each other's freshly attached page
  for ever). The slot rule that check relies on: whenever the class lock is free, a slot is null or names a page `ATTACHED` to its
  class (a dropped slot is set to null, an added slot starts null). `cnd.lib` asserts the rule at every slow-path entry.
* **Slots**: a class starts with one; a class whose slot pages lose `collisions` compare-and-swaps adds one, up to `slots`. A slot
  not used for the give-back delay is dropped by collect (its page detached; slot 0 keeps its page unless it is wholly free).
* **`calloc` fills every small cell** (the written-mark of the lock design is not kept); large blocks keep their skip-fill.
* **The worker cache, mended** (`cache=<n>` with this design): a worker is parked by the actors scheduler with a timeout of the
  give-back delay, and empties its cache and collects only if that passes with no work; a fill is 2 cells on the first miss after the
  cache was empty, doubling up to half the cap; small `realloc`, `_recalloc` and the aligned calls go through the cache.
* **A fill is one compare-and-swap, a return one `fetch_or` per word** (row 506; the rules are
  `Ruminations\row-460-rules-and-tests-2026-11-18.md`, B1 and B2). A fill (`lf_fill`) takes the lowest free bits of one word, up
  to the fill size, in one compare-and-swap; a word with fewer free bits gives a short fill, never a second word. Rule G follows
  it as for one cell, on the whole `genclass`: if the page changed life, every taken bit goes back in one `fetch_or` passing the
  `genclass` read after the swap, and no cell has been written (the cells are linked through their first words only after the
  test). An over-cap return or an emptied cache (`cn_lf_give_chain`) first reads every link of a batch of up to 256 cells, then
  gives back each (page, word) group with one `fetch_or`, reading `genclass` before it; the double-free check covers every
  bit of the group. The multi-bit fills' Rule G undos are counted on their own as well (`cnmalloc_lfTotal(10)`, a statistics run).
* **The memory bound.** Beyond what the lock design held, this design can hold at most one partly used 64 KiB page for each slot
  above the first, in each class where collisions were seen (a slot is added only on a collision and idles back down). With a cap
  of 32 and all 72 classes contended by 32 threads at once that is 72 x 31 x 64 KiB, about 140 MiB (a cap of 8 held about 32 MiB):
  at most (cap - 8) x 64 KiB more per class than the former cap, and it falls back within one give-back delay of the contention ending. Slot 0's page of each class is
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
7. A module trait (`Os`) cannot be chosen by anything but naming the conforming module in the code that
   uses it: there is no way yet to select the implementation by a build setting. (Row 550: `osplat.cone` names both and forwards each
   call with `#if #os`; each module is all stubs on the other system, because an unreferenced public function is still emitted and
   would carry its undefined externals into the object.)
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
    `cnmalloc_setDoubleFreeHandler`. `cnmalloc_lfSetHooks` takes six functions: before a compare-and-swap, inside a detach,
    inside a retire (after RETIRED), inside a retire before its claims, right after a free's `fetch_or` (`lf_release`; compiled under
    `CN_HOOKS` only, so the shipped `cn.lib` has no such code or symbol), and inside "after a free" past the generation test, under
    the class lock (a heap field like the three before it: one null test and a call, on the free slow path only).
12. (Row 454) The C side reads the slot, `genclass` and `state` with plain `volatile` loads and stores, which MSVC on x64 makes
    acquire loads and release stores (spec section 11 allows it there). An ARM port needs explicit acquire loads (or `/volatile:ms`).
