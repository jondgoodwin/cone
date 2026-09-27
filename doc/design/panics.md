# Panics

A panic is the program saying that something which must not happen has: an
index past the end, memory exhausted, an invariant broken. It is a bug report,
not error handling — a failure a program expects and recovers from is an
`Option` or a `Result` (and, unbuilt, `throw`; `refexcept.html`). This note
follows panics through the language, core, the compiler and the C runtime.

## Principles

- **A panic ends the program, at once.** Nothing unwinds: no finalizer runs,
  no caller sees it, no thread goes on without it. This keeps every function
  free of landing pads and unwind tables, which is the speed half of Cone's
  aims, and it is what the compiler assumes elsewhere — a function's frame of
  traced roots is never popped by a panic (`genlRootFrame`), and the locks in
  `sync` need no poisoning. Isolating one failure from the rest of a program
  belongs to actors, where a failed actor's memory can be discarded whole,
  not to unwinding inside a thread.
- **A panic says what failed and where.** The where is the source line that
  asked for the thing that failed, so a helper that panics on its caller's
  behalf reports its caller: `list[9]` names the line of the index, not a line
  inside `collections`.
- **The failure path costs the hot path nothing but its test.** A check the
  compiler inserts is a compare and a branch LLVM is told is never taken; the
  report is built out of line, in a cold function that does not return.
- **A call that does not return is known not to**, so it can end any block,
  one that must produce a value included, and the code after it is known dead.

## The design

**`Never`** is the return type of a function that does not return: core's
`panic`, `unreachable` and `todo`, and libc's `abort` and `exit`. It is built
in, as `void` is, and a program may write it on a function of its own, which
must then end in a call returning `Never`, or in an `if` every path of which
does (`ErrorNeverReturns`). A call to one may end a block that must produce a
value — the block takes no value from that path — and so a branch of an `if`
or an arm of a `match`. It is not a type that coerces to every other, as
Rust's `!` does: it is `void` wherever a value is asked of it, and a call
returning it is only known not to return where it ends a block.

**Core's functions** (`packages/core/src/core.cone`): `panic(msg)`;
`assert(cond, msg)`, checked in every build since Cone has no build that leaves
it out; `unreachable(msg)` and `todo(msg)`, each with a message of its own by
default; and `setPanicHook(hook)`.

**The location is a default argument.** `srcFile()` and `srcLine()` are core
intrinsics answering where their call is written, the file's name without its
folders and the line. Written as a parameter's default value, each answers
where the call taking the default is. Each function above declares
`file &[]u8 = srcFile(), line u32 = srcLine()` and so reports its caller, and
any function can do the same and pass the two on — Swift's `#file`/`#line`,
Odin's `#caller_location`; what Rust's `#[track_caller]` does out of sight.

**The report** (`packages/conestd/panic.c`): stdout is flushed; one line goes
to stderr, `panic at <file>:<line>: <message>`, or `panic in thread <id> at
...` on a thread other than the one the program started on; the hook, if one
is set, is called with the message and the location; then the C library's
`abort` ends the program (SIGABRT; on Windows a fail-fast, exit status
`0xC0000409`, where a debugger stops). The line is written before the hook
runs, so a hook that fails cannot lose it. Setting the hook is atomic; the last
one set is called. A panic on a thread already panicking — the hook's own —
writes one line and aborts at once.

**The compiler's checks** (`genlPanic`) — an index at or past its count, a
range not within its count, a region's `alloc` answering null — call conestd's
entry for each, handing it the values compared and the location:
`index 5 is out of bounds for a count of 3`, `slice 2..7 is out of bounds for
a count of 5`, `slice 4..2 starts after it ends`, `out of memory allocating 24
bytes`. On WebAssembly, which links no conestd, each is a trap.

## What is built, and what is not

Everything above is built. Not built:

- **Recovering from a panic** — Rust's `catch_unwind`, Go's `recover` — and
  therefore unwinding; and **per-actor failure isolation**, which waits on
  actors.
- **Backtraces**, and the column in the location.
- **Checks only some builds make**: the reference manual's `assert` is a
  test-build check and `requires` the always-on one; with no build modes,
  core's `assert` is always on. `requires` and `ensures` contracts are not
  built.
- **`?` on a `None`**, which the manual says panics, is not built, and nor are
  `throw` and `catch`.
- **A panic in code an importer expands from a package's include file** — a
  generic type's method, an inline function — **reports the include file's
  line** where it reports its own location rather than a caller's: the include
  file drops the text it does not carry and adds a header, so its lines are
  not the source's. The package functions that panic on a caller's behalf
  report the caller instead, which is unaffected.

## What lives elsewhere

- What a panic looks like to a programmer: `doc/reference/refexcept.html`
- `srcFile` and `srcLine`: `doc/reference/refintrinsic.html`
- Where a failed check branches and what it calls: [Generation](../../compiler/c/doc/phases/generation.md), "Statements and expressions"
- How a block ending in a call returning `Never` becomes a `return`: [block](../../compiler/c/doc/nodes/block.md), [return](../../compiler/c/doc/nodes/return.md)
- Which checks exist: [Safety](safety.md), the scorecard
