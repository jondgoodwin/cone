How the compiler says something is wrong: the code, the message, the position,
and the rules for adding one.

Read this before adding a diagnostic, and before reusing an existing code.

*Provenance: read from source, cross-checked against `test/codes.toml`.*

## Key principles

1. **A diagnostic gets its own code.** Do not reuse an unrelated one for a new
   condition. Lookup failures, visibility errors and no-match errors must stay
   distinguishable — a scenario asserts a code, so two conditions sharing one
   cannot be told apart by any test.
2. **The numbers are a published interface.** The compiler prints the number, so
   a renumber invalidates every expectation at once and none of them loudly.
3. **The cause goes in the message, not in a second code** — but only where the
   codes would be indistinguishable except by wording. Section 4 is where that
   line sits.

## The ranges

| Range | Kind | Counted as |
| --- | --- | --- |
| 0–99 | process exit codes | not a diagnostic |
| 1000–2999 | errors | `errors` |
| 3000+ | warnings, `WarnCode` upward | `warnings` |

`code < WarnCode` is the whole of the error/warning distinction. The two
counters are separate, which matters because **warnings never gate**: the flow
analysis gate and the two `fnDclTypeCheck` early returns all compare `errors`.

Exit codes are their own thing and they abort immediately rather than being
counted: `ExitError` (1), `ExitNF` (2, source file not found), `ExitOpts` (4),
`ExitIndent` (5, past 1024 nested blocks), `ExitGen` (6). `ExitGen` covers
both unrecoverable internal failures: code generation that could not proceed,
and a compiler invariant that did not hold.

## Reporting

| Function | Position from | Use for |
| --- | --- | --- |
| `errorMsgLex` | the lexer's current token | the parser — this is its workhorse |
| `errorMsgNode` | a node's stored position, **plus the instantiation trace** | every phase after parsing |
| `errorMsg` | nothing | when there is no position to give, which should be rare |
| `errorExit` | prints and exits | only where continuing is impossible |
| `errorUnreachable` | a node's position, then exits `ExitGen` | a state the compiler had ruled out — never a bad program |

**`errorMsgNode` walks `instnode`** to print "as instantiated by…" outward from
the reported node, capped at `ErrorInstTraceMax` (4). Ordinary code nests one or
two deep; the cap exists so a runaway expansion does not bury the diagnostic
under hundreds of identical frames.

**A node built after parsing takes the lexer's current position**, which is end
of file. Call `inodeLexCopy` on anything injected, or the diagnostic points at
nothing. This is the single most common way a new diagnostic comes out wrong.

## Adding one

1. Add the code to `shared/error.h` **with an explicit value**, at the end of
   its block, in declaration order.
2. Add the same name and number to `test/codes.toml`. The runner compares that
   table against the header **before any case runs** and fails naming exactly
   which codes moved. Regenerate with `python test/run.py --bless-codes` and
   read the diff rather than trusting it.
3. Add a scenario that provokes it, in the owning group. A new `ErrorCode` lands
   with the scenario that produces it — see [Test Suite](test-suite.md).

Explicit values in the header are what remove the renumber hazard at the source;
the pinned table is defence in depth, catching a code added without following
the header's convention.

## When one code carries several causes

`ErrorNoSize` is the worked example, and the reasoning generalizes. A type can
fail to have a size five ways — `@opaque`, a trait that is not `SameSize`, a
function signature, a struct with an unsized field, and a type still being laid
out. All five report `ErrorNoSize`, and the cause is named in the message.

**Five codes would be indistinguishable to everything except the wording**, and
the wording is what the author actually needs — each cause has a different
remedy. So the test is not "is this a different condition?" but "would a caller
ever branch on which?" If nothing would, one code and a specific message is
right.

That is a narrow licence, and `ErrorManyArgs` was the worked example of taking
it too far: it covered wrong arity, a non-type generic argument, "expects
arguments to be provided" and arguments given to a field access — conditions a
reader would absolutely distinguish, and the scenarios asserting them had to be
told apart by message substring. It now means only what it says, and
`ErrorArgCount`, `ErrorNotType`, `ErrorNoArgs` and `ErrorFldArgs` carry the
rest. The tell was the scenarios: when a code needs a substring to be useful,
the substring is doing the code's job.

## The one code with no scenario

`ErrorUnreachable` is reported by `errorUnreachable` and by nothing else. It
means the compiler reached a state it had established cannot happen: not a bad
program, a compiler defect. A source that provoked it would be a bug report
rather than a test case, so it is the one code that lands without a scenario —
deliberately, not by oversight.

Everything about it follows from that. It goes through `errorMsgNode` so a
report carries a source position **and the instantiation trace**, which for a
defect that only appears inside a generic expansion is the only actionable part.
It then exits `ExitGen` — the compiler has just established that its own
invariants do not hold, so anything further it emitted would be guesswork. A
separate exit code was considered and rejected: it would enlarge the `driver`
category's contract for a status nothing can produce.

**Never call it for a condition a source can reach.** That is a missing
diagnostic, and it gets a code and a message of its own, upstream where the rule
lives.

## Suppressing a cascade

One mistake should produce one diagnostic. Two mechanisms:

- **`errorType`**, installed on a node whose error was already reported.
  `itypeMatches` returns `EqMatch` when either side carries it and
  `iexpMultiInfer` drops such a branch, so everything derived from it stays
  quiet. `inodeIsError` is the predicate. See the sentinel section of
  [IR Nodes](../nodes/_index.md).
- **An error-count delta.** `fnDclTypeCheck` records `errors` on entry and
  compares, so "did *this* declaration fail?" is answerable without knowing what
  failed elsewhere. That is what gates the body check and flow analysis.

`iexpTypeCheckCoerce` returning success on an untyped operand is the same idea
by a third route: a deliberate lie so one bad subexpression does not provoke a
complaint from every enclosing node.

## Hazards

- **An `assert` is a no-op in the release build.** `NDEBUG` compiles it to
  nothing, and the release build is the only one the test runner uses, so an
  `assert` has never caught anything in a tested configuration. Do not add one
  expecting it to. The twenty-two sites that meant *unreachable* have been
  converted to `errorUnreachable`; the ordinary value asserts have not.
- **Reusing a code makes a scenario unable to tell two conditions apart.** If
  you find yourself picking an existing code because the new condition is
  "close enough", that is the smell.
- **A diagnostic on an injected node without `inodeLexCopy`** points at end of
  file, and the test suite's per-line annotations will not match.
- **`--checktree` reports `ErrorBadTree` on the error path too**, deliberately —
  a phase that reports and returns early is exactly what leaves a node without
  a `vtype`.

## What lives elsewhere

- Asserting a diagnostic in a scenario, and the annotation format: [Test Suite](test-suite.md)
- Finding out what the compiler actually reports: [Measuring](measuring.md)
- The three type sentinels and `newErrorNode`: [IR Nodes](../nodes/_index.md)
- Which codes each phase owns: the "Diagnostics" section of each phase note
