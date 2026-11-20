# The 23 `volatile` sites of cn_shim.c, and what replaced them (row 550)

Before row 550 `cn_shim.c` had 23 lines that used `volatile`, written for MSVC, where on x64 (`/volatile:ms`) a volatile load is an
acquire load, a volatile store a release store and `_Interlocked*` a full barrier. GCC and Clang give `volatile` no ordering against
other accesses, so on Linux every one of them is now an explicit access with its ordering in the macro's name (`cn_plat.h`), and no
declaration in the shim says `volatile` any more. On MSVC each macro is the same volatile access or `_Interlocked*` intrinsic as before
(the object code's locked instructions and their order are unchanged: see the PR, "Windows codegen").

Orderings are those of `WI\cnmalloc-second-design-spec.md` section 11: the detacher's `state` exchange and its loads of `bits`, and the
freer's `fetch_or` and its following load of `state`, are sequentially consistent; make-ready's and attach's stores are release stores,
the allocator's load of the slot and of `bits` acquire loads, its compare-and-swap a full read-modify-write (seq_cst here, as
`_InterlockedCompareExchange64` is, which is at least the acquire-release the spec names); `genclass` and `state` are written only under a
class lock (the Cone side, all seq_cst). Model steps are those of the model checks of rows 455 and 490
(`Ruminations\cnmalloc-second-design-model-check-2026-11-17.md`, `cnmalloc-multibit-model-check-2026-11-18.md`): A0 load of the slot, A1 load of
`genclass` (g1), A2 load of `bits[j]`, A3 compare-and-swap, A4 load of `genclass` (g2) and its undo R0; F0 free's load of `genclass`, R0
`fetch_or`, R1 load of `state`; RT-G/RT-L/RT-D the grouped return's rules; MB-G/MB-W the multi-bit fill's. "not modelled" means the model
has no such step (statistics, start-up, the worker cache's own counters).

| # | Line (old) | Site | Old | Ordering (section 11) | Macro now (GCC form) | Model step |
|---|---|---|---|---|---|---|
| 1 | 63 | `g_heap`, the process's heap pointer | `volatile uintptr_t` | published once by `cn_start` before `g_state` becomes 2: release store; read by `H()` after the acquire load of `g_state`, and plain after the acquire load of `g_fast` / `g_state` on every other path | `cn_st64_rel(&g_heap)`, `cn_ld64_rlx(&g_heap)` (in `H()`, after `cn_ld32_acq(&g_state)`) | not modelled (start-up) |
| 2 | 64 | `g_state` (0 not started, 1 starting, 2 ready) | `volatile LONG` | the start's compare-and-swap and the final exchange to 2 are full barriers; every reader's load is acquire | `cn_cas32`, `cn_xchg32` (seq_cst RMW), `cn_ld32_acq` | not modelled (start-up) |
| 3 | 77 | `LfD2.genclass` | `volatile uint64_t` | `g1` (A1), `g2` (A4), free's `g` (F0) and the grouped return's `g` (RT-G): acquire loads. `g2` must be fresh: it follows the A3 compare-and-swap, a full barrier, so it cannot be satisfied before it. `g` of a free precedes the `fetch_or`, a full barrier, which no earlier load passes. Stores are the Cone side's, under a class lock, seq_cst | `cn_ld64_acq(&P->genclass)` in `lf_alloc`, `lf_fill`, `lf_free_i`, `lf_give_group`, `lf_cellclass`; `cn_ld64_rlx` in the test peek | A1, A4, F0, RT-G |
| 4 | 78 | `LfD2.state` | `volatile uint64_t` | the freer's load after its `fetch_or` is sequentially consistent (the Dekker rule's second half); the detacher's exchange is the Cone side's | `cn_ld64_sc(&P->state)` in `lf_release_i`, `lf_give_group` | R1 |
| 5 | 79 | `LfD2.ncells` | `volatile uint64_t` | never read or written by the shim (the Cone side's field; the declaration only fixes the layout) | none (a plain `uint64_t`) | not used |
| 6 | 82 | `LfD2.bits[64]` | `volatile uint64_t` | allocator's load of a word: acquire (it may be what lets a thread that saw a set bit rely on seeing the `genclass` stored before it: make-ready's stores of `bits` are release stores); every change is a full read-modify-write | `cn_ld64_acq(&P->bits[j])` (A2) in `lf_alloc`, `lf_fill`; `cn_ld64_rlx` in the test peek; changes through the macros of sites 15, 16, 17 and 22 | A2 |
| 7 | 86 | `LfCls.slot[8]` | `volatile uintptr_t` | the allocator's load of the slot's page is an acquire load (attach's store of the page is a release store, the Cone side's) | `cn_ld64_acq(&L->slot[si])` in `lf_alloc`, `lf_fill` | A0 |
| 8 | 87 | `LfCls.active` | `volatile uint32_t` | read only to choose a slot and to compare against `cap`; a stale value is harmless (the Cone side recomputes it under the lock, spec section 2): relaxed | `cn_ld32_rlx(&L->active)`; `cn_ld32_rlx` in `cnmalloc_lfActive` | slot choice: the model's load of the active count |
| 9 | 91 | `LfCls.used[8]` | `volatile uint8_t` | a hint "this slot was used since the last collect", set by an allocation, cleared by collect under the lock; races only cost a missed give-back round: relaxed | `cn_ld8_rlx`, `cn_st8_rlx(&L->used[si], 1)` | not modelled (collect's idle hint) |
| 10 | 94 | `LfCls.collisions` | `volatile LONG` | a counter that triggers a slot add; incremented by a read-modify-write, reset to 0 by a relaxed store when the slot count is at its cap | `cn_inc32` (seq_cst RMW), `cn_st32_rlx(&L->collisions, 0)` | not modelled (slot growth, spec section 2) |
| 11 | 96 | `LfCls.st[10]` | `volatile int64_t` | statistics (stats runs only), incremented by a read-modify-write, read relaxed by the test exports | `cn_inc64`, `cn_add64` (via `lf_count` and sites 14, 18, 23), `cn_ld64_rlx` in `cnmalloc_lfTotal` | not modelled (counters) |
| 12 | 120 | `g_callocs`, `g_callocBytes`, `g_nonWorker` | `volatile LONG64` | statistics, as 11 | `cn_inc64`, `cn_add64`, `cn_ld64_rlx` (in `cn_publish`) | not modelled |
| 13 | 121 | `g_mbUndos` | `volatile LONG64` | statistics (the multi-bit undos), as 11 | `lf_count(&g_mbUndos)`, `cn_ld64_rlx` | not modelled |
| 14 | 152 | `lf_count`'s parameter | `volatile int64_t*` cast to `volatile LONG64*` | a statistics increment: a full read-modify-write | `cn_inc64` | not modelled |
| 15 | 173 | `lf_release_i`: the `fetch_or` of a free (and of an undo) | `_InterlockedOr64((volatile LONG64*)&P->bits[j], bit)` | **sequentially consistent** (the freer's side of the ordering rule); never retried | `cn_or64(&P->bits[j], bit)` = `__atomic_fetch_or(..., __ATOMIC_SEQ_CST)` | R0 (F0 then R0 then R1) |
| 16 | 234 | `lf_alloc`: the single-cell compare-and-swap | `_InterlockedCompareExchange64((volatile LONG64*)&P->bits[j], ...)` | acquire-release at least; seq_cst here, as the intrinsic is | `cn_cas64(&P->bits[j], old, old & ~bit)` = `__atomic_compare_exchange_n(..., SEQ_CST, SEQ_CST)` | A3 |
| 17 | 320 | `lf_fill`: the multi-bit compare-and-swap | as 16 | as 16 | `cn_cas64(&P->bits[j], old, old & ~want)` | A3 (multi-bit model) |
| 18 | 339 | `lf_fill`: the allocation counter `st[0]` | `_InterlockedExchangeAdd64((volatile LONG64*)&L->st[0], n)` | statistics, full read-modify-write | `cn_add64(&L->st[0], n)` | not modelled |
| 19 | 399 | `CnSlot.bytes, hits, misses, returns, fills` (a worker's cache counters) | `volatile int64_t` | only the owning worker writes them, with a plain read-modify-write; `cn_publish` reads them from another thread for a statistics line: relaxed on both sides (no ordering is claimed or needed) | `cn_add64_rlx(&s->hits, 1)`, `cn_st64_rlx`, `cn_ld64_rlx` | not modelled |
| 20 | 403 | `g_slotUsed[512]` (the cache slot table) | `volatile LONG` | a slot is claimed by a compare-and-swap and released by an exchange; the pre-test is a relaxed load | `cn_ld32_rlx`, `cn_cas32`, `cn_xchg32` | not modelled |
| 21 | 404 | `g_endHits, g_endMisses, g_endReturns, g_endFills` (workers that ended) | `volatile LONG64` | statistics, full read-modify-write; read relaxed in `cn_publish` | `cn_add64`, `cn_ld64_rlx` | not modelled |
| 22 | 514 | `lf_give_group`: the grouped return's `fetch_or` | `_InterlockedOr64((volatile LONG64*)&P->bits[j], mask)` | sequentially consistent, as 15 (RT-G: `genclass` read before it) | `cn_or64(&P->bits[j], mask)` | R0 for a group (RT-G, RT-D) |
| 23 | 521 | `lf_give_group`: the freed-count `st[1]` | `_InterlockedExchangeAdd64((volatile LONG64*)&g_lfc[..].st[1], popcount)` | statistics, full read-modify-write | `cn_add64(&g_lfc[g & 0xFF].st[1], cn_popcnt64(mask))` | not modelled |

## Flags that were never `volatile`, and are now explicit on GCC

Five words are written once in `cn_start` and read, before `H()`, on the allocation paths. They were plain `int` / `size_t` (not among the
23); on MSVC they stay plain reads and writes, which is today's code, and on GCC they are acquire loads and release stores through
`CN_FLAG_LD` / `CN_FLAG_ST`, so a reader that sees the flag set sees every word written before it:

| Word | Written | Read | Macro (GCC) |
|---|---|---|---|
| `g_fast` | last, in `cn_start`, after the fence that follows `g_lf` | `malloc`, `free` before `H()` | `CN_FLAG_ST` (release) / `CN_FLAG_LD` (acquire) |
| `g_cap` | in `cn_start`, after the fence that follows the class tables | `malloc`, `calloc`, `free`, `workerBegin` | same |

`g_lf`, `g_stats`, `g_pbase`, `g_psize`, `g_d2base`, `g_lfc`, the class tables and `g_cls`/`g_csize` are written before `g_state` becomes 2 and
before `g_fast`/`g_cap` are set, and are read only after one of them (or `H()`) has been read: plain reads, ordered by that acquire.

## Mutants

The mutants of `tests/mutants.json` are applied to this text; a (`a`), a2m, a2s, b, m1 to m7 and h1 each fail 5 of 5 on Linux in the PR's
run (`tests/mutants.py`), the test side of the same check.
