
### Compiler Options
- No array bounds checking
- Target conditionals

### How should the compiler react to reaching a state it believes impossible?

Undesigned today, and the current answer is "silently continue with garbage."
There are **22 `assert(0 && "...")` sites** across the parser, the IR and LLVM
generation, used to mean *unreachable*. The Release configuration compiles with
`/DNDEBUG`, so every one of them is `((void)0)` in the shipping compiler, and
control simply carries on — falling into the next `switch` case where the assert
was a case's last statement.

**Three of them were reachable, and two of those were the crash.** [[Bugs]]
repaired `iexpGetPermFlags`' deref arm and both `Invalid FldAccess methfld.`
sites, each of which fell into the following case: the one in `genlAddr` read a
field access as a string literal and segfaulted on `&t.0`, one line of ordinary
source. The remaining sites are unaudited and the count above is only smaller,
not safer -- a fall-through is invisible until something reaches it.

**This is not hypothetical.** `genlAddr`'s `ArrIndexTag` case had no arm for a
reference to a fixed-size array; the moment the type checker stopped rejecting
`v[0] = x` on a `&mut [3; i32]`, legal source reached that `default: assert(0)`
and the compiler died with an access violation instead of saying anything. Two
more sites of the same shape are recorded in [[Permissions]] and were repaired in
[[Compiler defect backlog]].

This is the systematic form of what [[Diagnose instead of crash]] closed one site
at a time. That item fixed the error paths that dereferenced their own NULL and
built `--checktree` and `errorType` to catch a malformed tree — but `--checktree`
only finds a node left with no type or no body, so it would not have caught the
crash above.

What wants deciding is the mechanism, not whether. Options, roughly:

- A helper — `unreachableNode(node, msg)` — that reports through the normal
  diagnostic path and exits non-zero. Cheap, mechanical to apply to all 24,
  and turns a crash into a filed bug report with a source location.
- Keep the asserts but build Release with them enabled. Loud, but aborts rather
  than diagnosing, and gives a user a stack trace instead of a message.
- Widen `--checktree`/`ErrorBadTree` to cover more invariants, and treat the
  assert sites as genuinely unreachable once it does. Most work, best end state.

Whichever is chosen, the sites should be audited rather than mechanically
converted: several of them are reachable from bad *source* rather than from a
compiler defect, and those want a real diagnostic with an `ErrorCode` instead of
an internal-error message. Found by [[Compiler defect backlog]].


