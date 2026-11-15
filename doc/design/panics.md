# Panics

A panic is the program saying that something which must not happen has: an
index past the end, memory exhausted, an invariant broken. It is a bug report,
not error handling — a failure a program expects and recovers from is an
`Option` or a `Result` (`safe-panics.html`), passed on with `?` and caught by
`match` or a `try` block, never thrown. This note
follows panics through the language, core, the compiler and the C runtime.

## Principles

- **A panic ends the program, at once.** Nothing unwinds: no finalizer runs,
  no caller sees it, no thread goes on without it. (At once means once its
  output is written: with the actors runtime, what the program printed before
  is queued for the console's owner, and a panic that finds the owner has not
  written it writes it itself, on its own thread, before its message. As in
  Rust and Go, that write can block for ever on a destination that never
  drains, such as a pipe whose reader is stuck; only an owner stuck inside its
  own write is given up on, at a ceiling of 10 s, and the last line counts the
  lines left unwritten.) This keeps every function
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
`assert(cond, msg)`, checked in every build, release included;
`unreachable(msg)` and `todo(msg)`, each with a message of its own by default;
and `setPanicHook(hook)`. A message is text (`&str`), and so are the message
and the file a hook is handed; the `file` a function forwards stays a slice of
bytes, which any `&str` converts to.

**Checks a debug build alone makes** [Jon 27 Sep]: `assertDebug(cond)` and
`assertDebugMsg(cond, msg)` are core **macros**, not functions, so the
condition is substituted where they are used instead of evaluated before a
call. Each expands to `#if #debug {assert(cond, ...)} #else {}`.
`#debug` is a fact of the compile: true under
`conec --debug` or `build: debug` (Congo's default for `build`, `run` and
`test`; `--release` for the other), false otherwise. In a release build the
branch is dropped by the parser, so the condition is
never evaluated and nothing of the use reaches the object;
`exception_assertdebug_release` pins both, the second with an IR check. A
macro takes no default argument, so the form with a message has a name of its
own. Plain `assert` stays a function: as one it takes a default message and
the forwarded `file` and `line`, which a macro cannot, and it loses nothing by
evaluating its condition, which it always does. The camelCase `assertDebug`
is for now: whether Cone has a naming convention is a question Jon has queued
for review. The build's facts (`#os`, `#arch`, `#debug`, `#defined`, `#define`)
are read by the parser (`doc/manual/mod-generativity.html#meta`); the earlier
`isDebugBuild`, `isWindows` and kin intrinsics are gone.

**The location is a default argument.** `srcFile()` and `srcLine()` are core
intrinsics answering where their call is written, the file's name without its
folders and the line. Written as a parameter's default value, each answers
where the call taking the default is. Each function above declares
`file &Array[u8] = srcFile(), line u32 = srcLine()` and so reports its caller, and
any function can do the same and pass the two on — Swift's `#file`/`#line`,
Odin's `#caller_location`; what Rust's `#[track_caller]` does out of sight.
Written in a macro's body, the two answer where the macro is used (at the
outermost use, for a macro inside another's body), as Rust's `file!()` and
`line!()` do: expanding a macro places its body's calls to them at the
outermost use (`macroSrcSite`, `CloneState.srcsite`, `cloneFnCallNode`), while
its arguments keep their own places. That is how `assertDebug` reports
its caller with no parameters for it. Code a program compiles from a package's
include file — a generic type's method, an inline function — answers with the
package's source file and line, which the include file's line marks give it
(`compiler/c/doc/nodes/module.md`, "Generating the include file").

**The report** (`packages/conestd/panic.cone`): stdout is flushed; one line goes
to stderr, `panic at <file>:<line>: <message>`, or `panic in thread <id> at
...` on a thread other than the one the program started on; the hook, if one
is set, is called with the message and the location (when `CONE_BACKTRACE` is
on, the backtrace, below, comes between the line and the hook); then the C library's
`abort` ends the program (SIGABRT; on Windows a fail-fast, exit status
`0xC0000409`, where a debugger stops). The line is written before the hook
runs, so a hook that fails cannot lose it. Setting the hook is atomic; the last
one set is called. A panic on a thread already panicking — the hook's own —
writes one line and aborts at once.

**The backtrace** (`packages/conestd/backtrace.c`, `printBacktrace` in
`panic.cone`). Off unless the environment variable `CONE_BACKTRACE` is set to
something other than empty or `0`; it is read at each panic, so a program may
set it itself. When on, the panic line is followed by `stack backtrace:` and one
line a frame, `  N: 0xADDRESS module+0xOFFSET`, at most 64, the panic
machinery's frames left out, so frame 0 is the function that called `panic` (or
whose check failed). Which frames are the machinery's does not depend on what the
compiler inlined: each check's entry (`cone_panicIndex` and the others) records the
address of a local in `panicAnchor`, `cone_panic` its own if none is set, and the
walk drops every frame whose stack pointer is at or below that address, and the
function holding it. The frames sit in a fixed array on the panicking thread's stack and
go through the same text buffer as the panic line: nothing is allocated and no lock
is taken, so the backtrace adds no deadlock path beside the console's (the #1180
take-over). It is written before the hook runs. A panic inside a panic prints none.
Windows walks the stack by its unwind data (`.pdata`) with
`RtlCaptureStackBackTrace`, naming the module with `RtlPcToFileHeader`; Linux
walks `.eh_frame` with `_Unwind_Backtrace` (glibc's `backtrace()` may allocate on
its first call) and names the module with `dladdr`. Neither needs a frame pointer,
the compiler's `uwtable` or any change to generated code; a function marked
`nounwind` is the one case a walk could stop at. Module and offset keep the frames
valid under address randomisation; the offset is from the module's load base, so
`llvm-symbolizer --obj=<exe> --relative-address` and `addr2line -e <exe>` take it,
one byte less than the frame's return address (the call, not what follows it: a call to
a function that never returns can end a function). The frames carry no names: debug
information is the symbolizer's, and a release build has it unless it was built
with `--strip`. Congo sets the variable for the programs it runs and names the frames
(`congo run`, `congo test`; below). A frame is the physical frame: inlined calls
show as lines of their own once symbolised. A task resumed from an `await` shows
the resuming stack, not the awaiting caller's.

Congo (`congo.py` and the Cone Congo) sets `CONE_BACKTRACE=1` for a program it
runs unless the caller has set it, writes each block as `N: function at file:line`
with `llvm-symbolizer` (or `addr2line` on Linux where that is absent; the frames
of the program's own executable only), and leaves the raw lines with one note when
neither is found. A test's `.err` comparison leaves the block out, since
addresses cannot be pinned; a failing test shows it named. The Cone Congo's `congo run`
sets the variable but its program's stderr is shared, so the frames stay raw.

**The compiler's checks** (`genlPanic`) — an index at or past its count, a
range not within its count, a region's `alloc` answering null — call conestd's
entry for each, handing it the values compared and the location:
`index 5 is out of bounds for a count of 3`, `slice 2..<7 is out of bounds for
a count of 5`, `slice 4..<2 starts after it ends`, `out of memory allocating 24
bytes`. On WebAssembly, which links no conestd, each is a trap.

## What is built, and what is not

Everything above is built. Not built:

- **Recovering from a panic** — Rust's `catch_unwind`, Go's `recover` — and
  therefore unwinding; and **per-actor failure isolation**, which waits on
  actors.
- The column in the location. Of backtraces: one on demand (`core.backtrace()`),
  frames in the hook's arguments, names in-process (no symbolizer needed),
  other threads' stacks, stack overflow (no stack is left to run this), and
  a trace through an `await` chain.
- **`requires` and `ensures` contracts.** The manual first had `assert` as a
  test-build check and a `requires` statement as the always-on one; Jon's
  ruling of 27 Sep made `assert` the always-on check and `assertDebug` the
  debug-only one, and left `requires`/`ensures` for contracts, not built.

## What lives elsewhere

- What a panic looks like to a programmer: `doc/manual/safe-panics.html`
- `srcFile` and `srcLine`: `doc/manual/safe-intrinsics.html`
- Where a failed check branches and what it calls: [Generation](../../compiler/c/doc/phases/generation.md), "Statements and expressions"
- How a block ending in a call returning `Never` becomes a `return`: [block](../../compiler/c/doc/nodes/block.md), [return](../../compiler/c/doc/nodes/return.md)
- Which checks exist: [Safety](safety.md), the scorecard
