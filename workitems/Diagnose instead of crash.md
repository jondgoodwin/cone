# Diagnose instead of crash

Eight places where the compiler works out that a program is wrong, reports it or
returns an error, and then dereferences the NULL its own error path left behind.
Found by building the test suite; the evidence is under "Found while building the
groups" in [[Add test suite]], shape B.

## Why these are grouped, and why they are urgent out of proportion to their size

**The test suite structurally cannot hold any of them.** A scenario asserts
something about a compile: its exit status, its diagnostics, its output. An
access violation produces an exit status outside the `ErrorCode` taxonomy and
nothing else, so there is no assertion to make. It cannot even be marked `xfail`,
because `xfail` asserts that a case *fails*, and a process death fails nothing.

Every other defect the survey found is either fixed, pinned by an `xfail`, or
excluded with a written reason that a future reader will find. These eight are
excluded with a reason and **nothing watches them**. If one silently starts
crashing differently, or a ninth appears, the suite will not notice. That is the
argument for fixing them as a group rather than as they are encountered.

Each fix lands with the scenario that fails without it (R6.3) — and for these,
the scenario cannot be written first.

## The sites

| Construct | Where the NULL comes from |
| --- | --- |
| `struct {` with no name | `parseStruct` reports `ErrorNoIdent` and returns NULL; the caller dereferences it |
| `trait {` with no name | Same path |
| `each x in <anything not a range>` | `parseEach` allocates a wrapping `BlockNode` and fills it only on the range path, so the loop *and its body* are discarded and `stmts` stays NULL for the first analysis pass |
| `[2, 3; 0]` — a two-dimension array literal | `arrayLitTypeCheckDimExp` (`ir/exp/arraylit.c:20-23`) returns after the error without assigning `arrlit->vtype` |
| A wrong number of generic type arguments | `genericMemoize` returns NULL; `genericSubstitute` (`ir/meta/generic.c:248`) type-checks it |
| A non-type generic argument | Same |
| `fn f[]()` — an empty type parameter list | Crashes in the parser with no diagnostic at all |
| Narrowing to a *structurally* conforming target | Type checks, then dies in LLVM generation. Works only when the type declares `extends` |
| Fallible allocation `?+rc-mut v` | |
| Cross-module private access | Name resolution accepts `mod::_privateName`; codegen never generated a symbol, so the call site uses a null `llvmvar`. See [[Unenforced language rules]] — the real fix may be to reject it at name resolution instead |

## The shared question

Two remedies are available and they are not exclusive.

**Per site**, make the error path leave a well-formed node — an error node, or a
node with a placeholder type — so that analysis can continue and report more than
one problem. This is the ordinary fix and it also improves recovery, since
several of these currently abort a compile that could have reported four more
things.

**Once**, make the invariant checkable. `--checktree` already verifies IR
well-formedness and runs on every `compile` and `run` scenario. If it also
verified that no node reachable after a reported error carries a NULL where a
type or a body is required, a ninth site would be caught by the existing corpus
rather than by whoever writes the next scenario. That is the change that stops
this list from regrowing.

CLAUDE.md's "Keep type safety explicit; do not hide invalid IR states with
unchecked casts or placeholder values" bears on the first remedy and should be
read before choosing the shape of the placeholder.

## Related

`ErrorGenErr` is one of the two diagnostics no scenario can cover, because the
only construct that raises it — a nested allocation — kills the compiler
immediately afterwards. Fixing that site closes a coverage gap as a side effect.
