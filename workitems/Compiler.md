
### Compiler Options
- No array bounds checking
- Target conditionals

### How the compiler reacts to reaching a state it believes impossible

**Decided and applied.** A state the compiler had ruled out is reported through
`errorUnreachable` and the compile stops. All 22 `assert(0 && "...")` sites are
converted; none is left able to fall through.

The problem it closes: the Release configuration compiles with `/DNDEBUG`, so
every one of those asserts was `((void)0)` in the shipping compiler and control
simply carried on. Four of them ended a `case` without a `break`, so the node was
reinterpreted as whatever the next label expected -- the shape that segfaulted on
`&t.0`, one line of ordinary source. Nine more ran the rest of their function on
a value the assert had just declared impossible. `test/run.py` only ever runs
`build/x64-release`, so no assert in this tree had ever fired in a tested
configuration.

`errorUnreachable(INode *node, const char *msg)` reports `ErrorUnreachable`
through `errorMsgNode` and exits `ExitGen`. Going through `errorMsgNode` is what
buys the source position **and the instantiation trace**: a defect inside a
generic expansion prints the call that expanded it, which is usually the only
thing a bug report can act on. It is `void`, called before the inert `return`
each site already needs, because six sites return six different types and no
single return type fits.

The other two options on the table were rejected. Building Release with asserts
enabled aborts rather than diagnoses and hands the user a stack trace. Widening
`--checktree` is still the better end state and is not excluded by this -- it
finds a node left with no type or no body, which is a different class of
malformation from the ones here.

**What the audit found.** Three sites a source actually reaches, all closed
upstream rather than at the assert, in the commit "Close the three assert sites a
real source can reach":

- `genlConvert`'s reference arm. `p into &i32` was accepted by a fall-through in
  `castTypeCheck`'s conversion table and **segfaulted the compiler**. A reference
  carries a region, a permission and a lifetime that a raw pointer cannot supply,
  so the conversion is not one; `castTypeCheck` now says so and `p as &i32`
  remains the spelling that keeps the bits.
- `fnCallArrIndex`'s reference arm. `&Alias` to an aliased array read the pointee
  tag unresolved, so a valid index came out as a return-type mismatch. It now
  resolves the pointee the way its caller did.
- `itypeMangle`'s default. A generic instantiated at a tuple, an array, a
  function reference or void mangled to nothing, so every such instance of one
  generic shared a symbol. The four now mangle.

Nine sites were classified unreachable from the guard that makes them so:
`genlConvert`'s two reference arms, `genlIsType`'s tag-field fallthrough,
`genlAddr`'s index default, both `Invalid FldAccess methfld` arms,
`fnCallArrIndex`'s two, and `genericSubstitute`'s default. Each argument is one
step: `fnCallArrIndex` is the only thing that builds an `ArrIndexTag` and admits
four receiver types; all three builders of a `FldAccessTag` set `methfld` to a
member use or a tuple index; `genericGetInfo` answers for a function and a struct
and nothing else.

**Thirteen sites remain unclassified**, and that is the open half of this item.
No source found in the pass reaches them and no guard was located that rules them
out: `genlIntrinsicFn`'s call-kind default, `genlIsType`'s vtable lookup,
`genlAddr`'s default, `genlExpr`'s declaration and node defaults,
`genlGlobalImpl`'s default, `genlType`'s default, `flowLoadValue`'s default,
`iexpGetTypeDcl`'s non-expression arm, `inodeNameRes`' and `inodeTypeCheck`'s
defaults, `inodeGetName`'s default, and `itypeMangle`'s default. They now abort
with a report rather than continue, which is an improvement in every case, but a
reachable one would tell a user "compiler defect" for a program that deserves a
diagnostic. Two of them -- `genlGlobalImpl` and `flowLoadValue` -- previously
fell through harmlessly, so converting them is the one place this change could
turn a working compile into a failing one.

Reaching them is the work that is left: the method that found the three was to
put a `fprintf` in the arm, rebuild, and compile adversarial sources against it,
which is far more reliable than reading. `design/diagnostics/measuring.md`
describes it.


