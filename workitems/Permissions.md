
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

## What should a field's permission mean?

**Undecided, and two code defects wait on it — and the question is now live
rather than academic.** Measured: a field's permission used to be write-only
state. `parseFieldDcl`'s validity check is degenerate —
`permdcl != mutPerm && permdcl == immPerm` reduces to `== immPerm`, so it
rejects exactly `imm` and admits `uni`, `opaq`, `mut1` and `ro`. Its `defperm`
argument is passed and never read, so an unwritten permission stays
`unknownType`. And the sole enforcement site compared by pointer identity
against the raw `immPerm` singleton, which a written permission never is — so a
`ro` field was writable.

**The third is fixed**, and that is what makes this urgent: `iexpGetLvalInfo`
now asks the permission whether it may write, so a field's permission governs
writes for the first time. `ro` and `opaq` fields are read-only from today;
`mut`, `uni` and `mut1` are not. Nobody decided that — it fell out of choosing
`MayWrite` as the test, which was the only choice that made the reported defect
go away. Whether it is the *right* rule is exactly this question.

**The other two are not fixable without the answer**, because flipping the parse
check inverts which permission is legal, and wiring `defperm` decides what an
unannotated field gets. [[Bugs]] carries the first as a defect that is not in
doubt while its fix is.

Answer it, then land all three together with scenarios in the `struct` group
covering each permission keyword on a field and a write through a restricted
one.
