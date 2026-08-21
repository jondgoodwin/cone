
- Have flowLoadValue do read permission checks on “lval” node types (nameuse, deref, arrindex, strfield) and move iexpGetPermFlag to flow?
	- **`iexpGetPermFlags` is dead today — nothing but itself calls it.** Its
	  fall-through bug is fixed: the deref arm ended in an assert that Release
	  compiles out, so control fell into the array-index case and reinterpreted
	  the `StarNode` as a `FnCallNode`. It now answers for a slice and a virtual
	  reference, and returns rather than falling through for anything else.
	  Read that arm before this item picks the function up. Fixed by [[Bugs]].
	- The sibling it was modelled on, `iexpGetLvalInfo`, has now had three missing
	  arms repaired (`RefTag` on array indexing, and a `((StarNode*)lval)->vtexp`
	  cast that was silently reading `methfld`). Read those fixes before extending
	  either.

**The meet of two permissions is undefined, and inference and coercion disagree
because of it.** Coercion accepts a `&mut T` where a `&T` is wanted —
`collection-success` asserts exactly that for slices — but `refFindSuper` refuses
outright when the two permissions differ, so branch inference cannot find a type
in common:

| | |
| --- | --- |
| `if c { &a } else { &b }`, same permission | works |
| `if c { &[]a } else { &[]b }`, same permission | works |
| `if c { &mut a } else { &a }` | `ErrorInvType` "Branch's expression type inconsistent" |
| `if c { &[]mut a } else { &[]a }` | same |

The rule that would settle it is small — the meet of two references differing
only in permission is the weaker permission, which `permMatches` already orders —
but it is a statement about the language rather than a repair, so it wants
deciding here rather than being assumed by a bug fix. `itypeFindSuper` also has
no arm for `ArrayRefTag` or `PtrTag` at all; identical slices only meet because
`itypeIsSame` catches them first. See [[Type Inference and Coercion]] for the
inference side.
- Permission:  addr-of restriction on dependent types
- Programmable Lock permissions
- Conc (concurrent) permission
- [[Managed Reference Metadata Access Prototype]]

Field permissions blog/doc Necessary for safety. Special case of uni owner and mut fields: can’t allow shared/mut here for thread safety (we want isolated data that moves, can allow shared immutable, uni). Restricts us to hierarch graphs. To solve, we want to annotate these references as belonging to a region (=lifetime!). If field perm=mut w/ uni container we permit if lifetime matches (must be specified). This allows us to do scoped arenas!

## What a declaration's permission means: `mut` or `imm`, and nothing else

**Decided 2026-08-21, for now, with caveats recorded below.** A declaration —
field, local, parameter or global — names one storage location that only it
owns, so the aliasing distinctions the reference permissions draw have nothing
to say about it. What is left to choose is whether the value may change, and
`mut` and `imm` are the two that say it. Everything else is `ErrorInvType`.

The defaults differ, deliberately: a variable defaults to `imm`, so mutation is
opted into, while a field defaults to `mut`, which is what
`coneref/refstruct.html` already says and which means the container's permission
governs.

`parseDclPerm` is the one place the rule is stated; `parseVarDcl` and
`parseFieldDcl` both call it. That closed three defects at once — the parse
check was `permdcl != mutPerm && permdcl == immPerm`, which reduces to
`== immPerm`, so it rejected the one permission that is legal and admitted the
four that are not; `parseFieldDcl` took a `defperm` and never applied it; and
`ro` was accepted on a variable and nowhere else, as a second spelling of the
constant that `const` already is. `ParseMayConst` existed only for that and is
gone. Scenarios: `struct-parse` and `core-parse-decls` for what is refused,
`struct-flow-fieldperm` for what a legal one then governs.

The enforcement side was fixed just before: `iexpGetLvalInfo` compared node
pointers against the raw `immPerm` singleton, which a permission written in
source never is, so an `imm` field was writable. It asks `permGetFlags` for
`MayWrite` now.

### Three caveats on the record

Jon's, recorded at the moment of deciding, because none of them is settled by
the decision and each would reopen it.

**1. `mut` is probably the wrong word; `uni` is the accurate one.** There is only
ever one owner of a declaration's storage, and `uni` is the permission that says
so. The evidence agrees and is stronger than "probably": `MayAlias` is read in
exactly one place in the compiler — `refAdoptInfections`, off a *reference
type's* permission — and `MayAliasWrite` is read nowhere at all. Those two flags
are the whole difference between `mut` and `uni`, and both are inert on a
declaration. So `mut` on a declaration carries reference-shaped flags that
nothing reads there, which is exactly why the inaccuracy has cost nothing yet.
What keeps `mut` is ergonomics and the manual, not meaning. **The seam:** if
`&mut x` is ever checked against the *variable's* `MayAliasWrite` rather than
against the borrow machinery, `uni` would forbid it and `mut` would not. Nothing
does that today.

**2. It may not be worth having the annotation at all.** Nothing is unsafe about
a declaration always being mutable; the only thing the annotation buys is
knowing the value cannot change, and an editor could derive that from the code
alone without the author having to decide up front and then change the decision.

*Where that holds:* **locals**. Within one function body it is exactly
computable and nothing outside the body depends on it.

*Where it does not:* **fields and parameters**, for two reasons. It is a
contract rather than an inference — it constrains code the type's author never
sees, and inference from local code cannot produce a promise. And `imm` is not
only "does not change": it carries `RaceSafe` and `MayIntRefSum`, which `mut`
does not. An immutable field is safely shareable across threads, which no editor
can derive because it is a guarantee the author is making. Under a two-word
vocabulary `imm` is the only way to say it, so it becomes load-bearing the
moment [[Concurrency Threads]] lands rather than decorative. A lesser point: an
inferred permission makes the declaration a consequence of its body, so a line
changed deep in a function silently flips it — fine as an editor annotation,
poor in a diff and poor in a signature.

**3. A global needs a third answer this vocabulary cannot give.** A mutable
global is shared mutable state by definition, so `mut` on one is sound only
while the program has a single thread. Making it safe otherwise needs a dynamic
permission with synchronization — and none exists: `IsLockless` is set on all
six permissions in `corelib.c` and read nowhere, which [[Regions]] also records.
The rule above is applied to globals today because doing so forecloses nothing —
adding a third permission later is additive — but **the question is open for
globals in a way it is not for the other three**, and this note is what says so.

### What is still undecided beneath all of it

**Viewpoint adaptation.** `coneref/refstruct.html` says the governing permission
is "derived from both the field's and the struct's permissions using a mechanism
called viewpoint adaptation". The word appears nowhere in `src/` or `design/`:
it is documented and unimplemented. The decision above is compatible with adding
it, because `mut` on a field already *means* "the container governs" and is
where adaptation would attach — but until it exists, a field's permission does
one thing, which is to refuse a write.

