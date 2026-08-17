
- Have flowLoadValue do read permission checks on “lval” node types (nameuse, deref, arrindex, strfield) and move iexpGetPermFlag to flow?
	- **`iexpGetPermFlags` is dead today — nothing but itself calls it — and it
	  carries a bug that would bite the moment this item picks it up.** Its
	  `DerefTag` arm sets the permission for `RefTag` and `PtrTag` only, then ends
	  in `assert(0 && "Should be ref or ptr")`. The Release build defines `NDEBUG`,
	  so that assert is nothing and control **falls through into
	  `case ArrIndexTag:`**, which reinterprets the `StarNode` as a `FnCallNode`.
	  A slice or virtual-reference deref takes that path. Add the `ArrayRefTag` and
	  `VirtRefTag` arms when moving it, and do not trust the assert to have been
	  guarding anything. Found by [[Compiler defect backlog]]; not deleted, because
	  this item wants the function.
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
