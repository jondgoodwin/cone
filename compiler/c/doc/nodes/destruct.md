`DestructNode` is a field list, `x.(a, b)`: the named fields of a struct, or the
numbered elements of a tuple, `t.(0, 2)`, taken from the value in front of the
dot. One name gives that field's value, not a tuple of one; two or more give a
tuple of them, in the order written. It serves two roles. Where the fields are
read as `x.a` reads one, type check lowers it into those field accesses and the
node is gone. Where the value is consumed, it survives to flow and generation as
the one node in the compiler that **moves a whole value in and gives some of its
fields back**.

*Provenance: read from source and measured on the built compiler; the drop order
is pinned by `struct_fields_take`.*

## Principles — [derived]

⚠ **Read from the code and its tests, not confirmed by the author.**

**A struct is never left with a hole, so a partial take is a whole take.** Flow's
rule is that nothing moves out of a field (`flowRefuseMoveField`, [flow](../phases/flow.md)),
because the struct would be left unusable and with no answer to what its death
finalizes. A field list does not break the rule: the whole value moves into the
node, the named fields leave it as its result, and the rest are ended at once.
▸ **Settles** that this needs no per-field move state, no field-level drop flag and
no hollow struct, and **forbids** adding any to support it.

**The rest of the value dies as the value would have died, minus what was taken.**
The fields not named are discarded the way a `_` target's value is: kept in a
temporary's slot and finalized with the statement's other temporaries, newest
first. ▸ **Settles** that "the order the struct's own drop uses" is not a second
rule but the one in `genlStructDrop`, replayed.

**A value that stays whole is read, not taken.** A copy type kept in a place, and
anything reached through a reference, is read field by field as `x.a` is, because
nothing moves and `x` stays alive. ▸ **Forbids** giving the consuming rules to
those: `p.(x, y)` on a copy `p` must not kill `p`.

## 1. Shape

`DestructNode` (`ir/exp/destruct.h`), an `IExpNodeHdr` and:

| Field | Meaning |
| --- | --- |
| `exp` | the value whose fields are taken |
| `names` | as written: a member name use (`NameUseNode`, built by `newMemberUseNode`) for each field, or a `ULitNode` for each tuple element. Never mixed once type check accepts it |
| `index` | set by type check on a node that survives: for each name, the field's `index` in its struct (the position in the LLVM struct) or the element's position in the tuple |
| `vtype` | the one field's type, or a type tuple (`TTupleTag`) of the fields' types in the order written |

## 2. Constructors

`newDestructNode(exp)` is the only one, called by the parser with the names still
to be added. `cloneDestructNode` copies it (the clone keeps `index` if type check
set one; a generic is cloned before it is checked).

## 3. Parse

`parseDotCall` finds a `(` straight after the `.` and hands over to
`parseFieldList`: one identifier or integer literal at a time, separated by commas,
up to the `)`. Anything else in the list is `ErrorFldListForm`, reported once at the
first thing that is no name, and the list is skipped to its closing parenthesis
(`parseFieldListSkip`, which pairs the parentheses inside it). An empty list is the
same error. `.(` was a syntax error before, so no existing program reads
differently. A `.` with nothing before it is `this`, so `.(x, y)` is `this.(x, y)`.
A method's call parentheses come after its name (`x.f(a)`), which is how the two are
told apart.

## 4. Name resolution

The value is resolved (`destructNameRes`). The names are members, selected by type
check from the value's type, as a call's member slot is.

## 5. Type check

`destructTypeCheck` checks the names for form (all names or all numbers, none
twice; `destructCheckNames`, which leaves a repeat for the type to judge), checks the
value, then decides what the node is:

0. **A vector** (or a reference to one) is not a struct or a tuple: its list picks lanes,
   by position or by the names x y z w, and a repeat is allowed. `vectorPickLower`
   replaces the node with a type literal of the vector and the positions (a number of one
   lane, a vector of several), a value that is never a place ([vector](vector.md)). For
   every other type a repeated name is `ErrorFldListDup`, reported here.

1. **What the names are looked up in** is the value's type with references and
   pointers peeled, as `x.a` peels them. A struct (not a trait or an enum) or a
   tuple has its names checked here (`destructNamesCheck`): a struct's must be
   fields of its own, not a name it holds only through a fold's hop, and a tuple's
   numbers must be elements. A number, an array or a function has no fields
   (`ErrorFldListType`). Anything else (a trait's virtual reference, an enum) is
   left to the ordinary field access below.
2. **The value is taken** when it is a struct or tuple held by value and is not a
   copy type kept in a place: a move type (a variable, a parameter, `self`), or any
   value that is no place (a call's result, a literal). The node survives. The type
   must have no finalizer of its own, found as the `final` in the struct's namespace,
   an enum's spliced into its variants included (`ErrorFldListFinal`); a private
   field is refused outside its module (`ErrorNotPublic`, as for a field access).
   `index` and `vtype` are set.
3. **Otherwise the value is read** (`destructLower`): each name becomes the field
   access `value.name`, built as a `FnCallNode` flagged `FlagRcvChecked` and checked
   as any is, so a reference is reached through, a copy type copies, a private field
   is refused, and a move-type field is refused by flow as `x.a` is. One name is the
   access, several are a value tuple of them. A place (a variable, a field or a
   dereference of one) is named again for each field with a copy of the place;
   anything else is kept first in a local of a block that holds the accesses
   (`FlagKeepTemps`, so its temporaries die with the enclosing statement), so it is
   evaluated once.

A sole owner (`Uni[T]`, a counted `Rc[T]`) is a reference, so it takes the third
path: its copy fields read through it, and a move-type field is `ErrorMoveField`, as
`owner.f` gives. Taking a struct apart through its sole owner would have to free the
allocation without finalizing the fields that were taken, which the hollow release
does for the referent as a whole only (flow, "A move out through a sole owner").

## 6. Flow and generation

Flow (`flowLoadValue`, `DestructTag`) loads the value and then moves or copies it
(`flowHandleMoveOrCopy`) as a call argument is: a move type is moved whole, so a
variable holding it is dead afterwards (`ErrorMove` on any later use, a second list
included), and what does not own its value is refused with the errors a move out of
it gets (`ErrorMoveField` for a field of another struct, `ErrorMoveOut` through a
borrow, a global). The result is a temporary like a call's: thrown away it is wrapped
by `flowTempRead`. The loan walk (`pwValue`) passes the value's loans on to the
result untagged.

Generation (`genlDestruct`) generates the value, reads each named field with
`extractvalue`, and builds the result (an `insertvalue` chain for a tuple). The
fields not named that need finalizing are handed to `genlDiscard`, the `_` target's
routine, in the **reverse** of the order they die: the statement's temporaries end
newest first. A struct's order is `genlStructDrop`'s, the fields that finalize and are
not owning references in field order, then the owning references in field order; a
tuple's is element by element. A variant laid out as a nullable pointer has no
struct: its one nameable field is the whole value, and nothing else is left.
