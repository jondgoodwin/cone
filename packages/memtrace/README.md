# memtrace: the memory sink, and the schema of the "memory" target

Permanent instrumentation of what a program asks of each region (the demand side of the memory workload report;
supply is row 423's, in core, conestd and a C shim under the `MEMSTATS` build). It goes through the `trace` package:
**off unless `CONE_TRACE` asks, with no rebuild**; off, an event costs one relaxed load and a compare and its
arguments are never evaluated.

    MEMTRACE_OUT=run.json CONE_TRACE=memory=trace  myprogram     # every event on (Trace)
    MEMTRACE_OUT=run.json CONE_TRACE=memory=debug  myprogram     # make / grow / drop / phase only
    python packages/memtrace/tools/report.py run.json            # the tables

A program imports `memtrace`; with `MEMTRACE_OUT` set it installs the sink when it starts (or call
`memtrace.install()`). `memtrace.phase("burst")` writes a phase mark that the report cuts by.

## The schema

Target `memory`. Every kind has the same four u64 fields, `elem`, `n`, `inst`, `aux`, except the phase mark.
Kinds are `<region>.<op>`; the region names follow row 421's kind table (`Ruminations\cone-alloc-inventory-2026-11-16.md`,
Part 1, K01-K22). Per-object kinds are `Trace`; make, grow, drop and phase are `Debug`.

| Kind | Level | Region (row 421) | elem | n | inst | aux |
|---|---|---|---|---|---|---|
| `list.make` | Debug | K01 List (K02 String's buffer is a `List[u8]`: elem 1) | bytes of one element | capacity after | instance address (0 when made in an init) | 0 |
| `list.grow` | Debug | K01 | element bytes | capacity after | instance address | capacity before |
| `list.drop` | Debug | K01 | element bytes | length at end | instance address | capacity at end |
| `deque.make/grow/drop` | Debug | K03 | as list | as list | as list | as list |
| `dict.make/grow/drop` | Debug | K04 Dict and Set | bytes of one entry | capacity (entries before a regrow) | as list | make: 0; grow: capacity before; drop: capacity |
| `genpool.take` | Trace | K07 GenPool (also a DensePool's sparse index, elem 4) | cell bytes | live cells after | pool address | free-list depth (cells ever used less live) |
| `genpool.return` | Trace | K07 | as take | live cells after | pool address | free-list depth |
| `genpool.make`, `genpool.grow` | Debug | K07 | cell bytes | capacity after | pool address | 0; capacity before |
| `densepool.take`, `densepool.return` | Trace | K08 DensePool rows | row bytes | rows after | the rows block's address | rows capacity |
| `densepool.make`, `densepool.grow` | Debug | K08 | row bytes | capacity after | the rows block's address | 0; capacity before |
| `pool.take`, `pool.return` | Trace | K06 Pool / Ref | slot bytes | live slots after | pool address | slots ever used less live |
| `pool.make`, `pool.grow` | Debug | K06 | slot bytes | capacity after | pool address | 0; capacity before |
| `iobuf.take` | Trace | K10/K11 IoBuf | block bytes (the size class) | buffers out after | 0 | bytes asked for |
| `iobuf.return` | Trace | K10/K11 | block bytes | buffers out after | 0 | bytes held |
| `bufpool.reserve` | Debug | K10 IoBuf slabs and oversize blocks (VirtualAlloc / mmap: no malloc shim sees them) | slab bytes (2 MiB), or 0 for an oversize block | bytes of address space asked for | 0 | 0 |
| `bufpool.commit` | Debug | K10 | as reserve | bytes made usable (a slab: slab + header page) | 0 | 0 |
| `bufpool.release` | Debug | K10 | as reserve | bytes given back (no partial decommit exists) | 0 | the reservation's bytes |
| `bufpool.cache` | Trace | K10 per-thread caches | 0 | bytes this cache holds after a batch refill or spill (no check on the per-block fast paths) | the cache's address (a key of its own: the report sums each cache's last value at each sample) | the cache's misses so far |
| `arena.grow`, `arena.drop` | Debug | K18 Arena chunks | chunk room in bytes (drop: 0) | same (drop: 1) | 0 | 0 |
| `gc.make`, `gc.free` | Trace | K21 Gc objects | object bytes | collector live bytes after | 0 | live objects after |
| `msg.make`, `msg.drop` | Trace | K20 actor messages (sent / run) | message bytes | 1 | 0 | 0 |
| `arc.make`, `arc.drop` | Trace | K15 Arc | block bytes (drop: 0, the free is not given it) | 1 | 0 | 0 |
| `memory.phase` | Debug | none | text: the phase's name | | | |

Not emitted, and why: **RcPool and UniPool do not exist in the source** (row 421), so there are no such kinds;
**IdPool (K09)** is `libc.realloc` direct and stray, so it is row 423's; **Uni, Rc, Rcw (K13, K14, K16), Futures
(K17), the actor region (K19), the Chain ring (K12), OrderedDict's index (K05) and the IoBuf slabs' VirtualAlloc
(K10 supply)** are core or conestd, or out of this row's reach, or supply; whether a growth *moved* the block is
the supply side's (the shim sees the realloc). There is **no per-push and no per-remove event** on any collection
(design chat, 16 Nov): no check sits on `List.push`.

## The context: who an event is in

The actors package registers `trace.setContextProvider` when its workers start. An event carries the worker's index
and, as its cause, the number of the actor whose behaviour is running (0 off a worker or between messages). The
sink's key calls this the actor (`who`): for `msg.make` it is the sender, for `msg.drop` the receiver; for
`iobuf.take` the taker and for `iobuf.return` the returner.

## What the sink writes

One JSON line per sample, every 100 ms of the events' own clock, holding each key whose count changed since the
last sample (cumulative), and a totals block at the end holding every key:

    {"s":"start","hz":10000000,"t":...}
    {"s":"sample","t":...,"dropped":0,"keys":[
    {"k":"list.grow","e":8,"a":3,"i":0,"c":15,"n":744,"nx":128,"x":372,"xx":64,"xl":64},...]}
    {"s":"phase","t":...,"name":"second burst"}
    {"s":"total","t":...,"dropped":0,"keys":[...]}

A key is `k` (kind), `e` (elem), `a` (actor), `i` (inst: only for the `pool.`, `genpool.` and `densepool.` kinds,
else 0). Under it: `c` count of events; `n` sum of the n field and `nx` its max; `x` sum of aux, `xx` its max, `xl`
its last value. Live counts and bytes come from pairs: make less drop, take less return. A pool's high-water mark
is `nx` of its take; its free-list depth, `xl` of its last take or return. `t` is `trace.ticks()`, which the supply
shim is to stamp with too, so the two streams merge by time.

## Storage that cannot emit

The sink's tables (per thread, 2048 slots, never freed), its merged table and its 64 KiB output buffer are
`RawBlocks` blocks and C `stdio`, below every region; `trace` imports only core, and core and `trace` emit nothing
in the memory target. No call the sink makes can reach an emit site. A thread-local flag also stops `record`
re-entering itself.
