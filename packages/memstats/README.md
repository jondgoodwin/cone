# memstats

Memory instrumentation, the supply side (work item B1; lane-queue row 423). Permanent in the source, off by default: a normal build has none of it in its binary. Design: `Incubator\Cone\Backlog\cnmalloc-foundation.md`, Track B.

**Platforms:** `congo.toml` declares no `os`, which means Windows and Linux, but only the Cone package alone builds and passes its test on Linux. The tagging shim (`shim\memstats.c`, `build.ps1`) is Windows only: it takes over UCRT names (`_msize`, `_aligned_*`) and reads `QueryPerformanceCounter`, so the instrumentation does nothing on Linux `[differs: Windows only for the shim; the declaration says both]`.

## What it records

Every block that reaches `malloc`, tagged with who asked. A link shim (`shim\memstats.c`) takes over `malloc`, `calloc`, `realloc`, `free`, `_msize`, `_recalloc`, `_expand`, the `_aligned_*` functions and the C runtime's `_malloc_base` family, forwards each to `ucrtbase`'s own, and puts a **32-byte tag** in front of every block:

| word | holds |
|---|---|
| 0 | the magic word and a check of the block's address (one word: the address xor the magic) |
| 1 | the size asked for (40 bits), the region kind (16), the growth count (8) |
| 2 | the first allocation's time, in `QueryPerformanceCounter` ticks (the clock `trace.ticks()` reads, so the two streams merge by time) |
| 3 | the allocating actor (32: its header address >> 4), the worker (16: a number given to each thread on its first use), the padding in front of an aligned block (16, in 16-byte units) |

A block the C runtime made inside its own DLL (`_strdup`, `_getcwd`, `fopen`) has no tag; at `free`, `realloc` and `_msize` the check fails (read under SEH, because a block that starts a page can have an unreadable page before it) and the block is forwarded untouched. An aligned block's tag sits just before its aligned address, with padding before the tag; alignments over about 512 KiB are not tagged.

**Who is asking.** `memstats_asking(kind, elemSize)` sets a C thread-local just before a region calls down, and `memstats_asking(0, 0)` clears it after; the **outermost non-zero setter wins**, so a collection's kind survives its region calling `RawBlocks`. Core marks `RawBlocks.alloc` (kind 22), `Uni.alloc` (13), `Rc.alloc` (14), `MemRegion.allocate` (32), sync marks `Arc.alloc` (15); the actors runtime calls `memstats_actor(header)` around each actor's batch. A block allocated with no kind set is **stray** (kind 0): the shim records the return address into `malloc` and three callers above it, so the report can sort strays by site through the program's PDB. Today most traffic is stray: the heap region's functions (`heapregion.c`), futures, message boxes and the collector call C or `libc.malloc` directly, below any marker; row 424's demand side names them.

`kinds.toml` is the one place the kind numbers live (K01..K22 are the region-kind table of the allocation inventory; 22 is `RawBlocks`, 32 is `MemRegion`). `check_kinds.py` checks that core's literals agree with it.

**Counters** are per thread (a slot of 160 KB of `VirtualAlloc`'d memory per thread), summed by the sampler; the shim never allocates from the C heap, takes no lock, and calls no sink. Per kind and size class (bit length of the size): allocations, frees, reallocs in and out; per kind: frees by another actor, frees by another worker, lifetimes (histogram of bit length of ticks), growths, moves, bytes copied by moves, allocations by element size.

**Output.** The sampler thread starts on the **first non-zero `memstats_asking`** (never from inside a `malloc`: the first one runs during the C runtime's start-up), and writes one JSON line every 100 ms to the file `MEMSTATS_OUT` names (a folder: `memstats-<pid>.jsonl` in it): `{"t":ticks,"live":[[kind,class,count,bytes],...],"live_n":,"live_b":,"private":,"unaccounted":}`. `private` is `GetProcessMemoryInfo`'s `PrivateUsage`; **unaccounted** = private minus the tagged-plus-stray live bytes (the C libraries' own heaps, stacks, loaded images, the IoBuf pool's `VirtualAlloc`, GPU memory). At exit a totals line is written (to the same file; to stderr when `MEMSTATS_OUT` is unset, with no samples). `summary.py <file>` prints a first look.

## Building a program with it on

One script, one setting:

```
powershell packages\memstats\memstats.ps1 run --release path\to\prog.cone -- args
powershell packages\memstats\memstats.ps1 test        (in a package folder)
```

It builds `shim\out\memstats.lib` when missing (`shim\build.ps1`, VS 2022 x64), puts it on the link line through the **`_LINK_` environment variable** (`link.exe` appends it after the objects and `conestd.lib` and before the default libraries, so every `malloc`, `conestd`'s own included, binds to the shim; the `LINK` variable put it ahead of `conestd.lib`, where conestd's own calls fell through to the C runtime), sets `MEMSTATS_OUT`, and runs `congo <verb> -D MEMSTATS ...`. By hand: build the library, `$env:_LINK_='"...\memstats.lib"'`, and give congo `-D MEMSTATS`. The define makes `#if #defined("MEMSTATS")` take the markers in core, sync, collections, actors and the collector (List, String, Deque, Dict/Set, OrderedDict, futures, message blocks, actor regions, the collector's objects); a block freed needs none, since the tag keeps the kind. The totals line also counts `large_aligned`: blocks aligned over about 512 KiB, which go untagged; without the shim on the link line the program fails to link (`memstats_asking` undefined), which is the right failure. A build without `-D MEMSTATS` is unchanged. **Instrumented runs are never timed:** a tag changes the layout, and the scheduling.

`congo` rebuilds only what changed: change the define or the link line and the cached objects of the packages are rebuilt (the define is a compiler argument), but a cached executable is not relinked, so clear `build\` when switching.

## Known limits

- conestd's own Cone code (compiled when the compiler is built, without `-D`) carries no markers; `rawblocks.cone`'s C-named functions and `heapregion.c` are below the markers. So the markers for raw blocks sit in core's inline `RawBlocks.resize` (22) and `MemRegion.resize` (32), which the collections reach through the heap region; Pool (6), GenPool (7), DensePool (8) and IdPool (9) mark their own resizes, the IoBuf chain's ring is Chain (12), and the jobs posted to the I/O thread are HostJob (33).
- **The instrument is not counted.** Kind 34 (memtrace's own tables) is passed to the C runtime untagged and uncounted: it appears in no kind, no stray row, and only in `unaccounted`.
- **Markers nest.** The shim keeps a per-thread depth: a non-zero `memstats_asking` takes the kind only at depth 0, and a `memstats_asking(0, 0)` ends the marker only when it brings the depth back to 0. So one set and one clear may bracket several calls, each of which sets and clears inside; every set needs exactly one clear on every path (a clear at depth 0 is ignored).
- Beyond 2048 threads the last counter slot is shared and its plain increments may lose counts.
- A `realloc` of an aligned block counts as an allocation plus a free, and a move.
- `RawBlocks.allocAligned` goes through `cone_alignedAlloc` (`_aligned_malloc` on Windows), which the shim tags with the thread's kind like the others.

## Tests

`shim\build.ps1 -Test` builds the shim and runs its own C test (`shim\shimtest.c`): tags, the outermost-asker rule, calloc, realloc growth and moves, foreign blocks (`_strdup` then `realloc` and `free`), a block that starts a page with nothing readable before it, aligned blocks and `_aligned_realloc`, and 16 threads handing blocks to each other (counts must add up, live must return to zero). `congo test` here runs `tests\enabled.cone`, which says whether the build has the markers.
