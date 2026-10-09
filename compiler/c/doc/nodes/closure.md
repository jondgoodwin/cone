A closure literal is **one node, `ClosureNode`, that lives from the parser until
type check replaces it** with the construction of a struct the compiler writes
(the *hidden struct*). Nothing after type check meets one, `--checktree` reports
one that is left, and that is the whole of the design: the literal's meaning is
a struct, a method and a call that already mean what they mean.

**At a glance.** `parseClosureFn` and `parseClosureArrow` (`parseexpr.c`) build
it. Name resolution (`closureNameRes`) resolves its parts and notes what its
body names of the code around it. Type check (`closureTypeCheck`) builds the
hidden struct, tries the body to find the permissions, and turns the literal into
`new Hidden(...)`. A name inside the body that names something the closure
holds is rewritten to the field's access as the body is checked
(`closureUse`).

*Provenance: read from source and measured by the `closure` group of the test
suite and `packages/closurelib`.*

## The two forms

```
fn (u f32) [ribs, mut n = 0, w = window.handle()] f32 { ... }     // the full form
x => x * 2          (a, b) => a.len() < b.len()          () => 5   // the short form
```

The full form is parameters, an optional **state list** in brackets (a bracket
never starts a type, so it cannot be read as the return type), an optional
return type and a block. A parameter's type is optional where a signature is
expected. A state entry is a field with an initializer; written without one,
`[ribs]`, its initializer is the variable of that name in the code around. The
short form is parameter names only (`ErrorClosureForm` otherwise), one expression
or a block, no list and no written return type. `=>` is the lexer's
`FatArrowToken`. The short form is read in `parseTerm` where a name or a
parenthesised list is followed by `=>`; the body is `parseSimpleExpr` and an
assignment on it, so a comma after it belongs to the list the closure is in.

`&fn(x i32) i32 { ... }` is not this node: it is an anonymous function lifted to
module scope as it is parsed (`parseAmper`), a function reference that sees no
local.

## Shape

`ClosureNode` (`ir/exp/closure.h`):

| Field | Meaning |
| --- | --- |
| `sig` | a `FnSigNode` of the parameters; `unknownType` for a type not written, and for a return type not written |
| `state` | a `VarDclNode` per entry: name, permission (`mut` or `imm`), initializer in `value` |
| `body` | a block (the short form's expression is wrapped in one) |
| `captures` | name resolution: the variables of the code around that the body names, each borrowed |
| `outerself` | name resolution: the `self` of the method it is written in, where the body names a member bare |
| `outer`, `outerscope` | name resolution: the literal it is written in, and the block scope it stands at |

## Name resolution

`closureNameRes` resolves the state's initializers and the parameters' types in
the code around the literal, before the closure's own names are hooked. It then
hooks the parameters and the state entries over the names around and resolves the
body, with `NameResState.closure` set and `loopblock` cleared (a `break` does not
leave a closure).

**What is the closure's own is told by scope, not by walking.** Every local has
the block scope it was declared at. The literal records the scope it stands at;
a variable declared deeper is its body's, and so are its parameters and state
entries (checked by membership, since a parameter's scope is 1). Any other local
a name binds to is of the code around it: `closureNoteUse`, called at the end of
`nameUseNameRes`, adds it to `captures` of every literal it is outside of,
innermost out, which is how a closure inside a closure borrows through the outer
one. A global and a function's `static` are reached by name and are not borrowed.

**A bare member name makes the method's `self` a capture.** Inside a method, a
bare field or method name means `self.name` (`nameUseTypeCheck`), and the
closure's own `()` has a `self` of its own. So the name binding to a member
records the enclosing method's `self` (`selfName->node`, hooked by the method) as
captured and as `outerself`, and the lowering to `self.name` reaches the member
through it (`closureSelfParm`). This is what lets a closure written in a method
use that struct's private fields and methods, and call them from wherever it
is passed: privacy is the module's, and the hidden struct is joined to the module
of the code around it.

`pstate->declaring` is the name of the local whose initializer is being resolved
(`varDclNameRes`). A name inside a closure that is that name and bound to nothing
is `ErrorClosureSelf`: a closure cannot call itself, and recursion uses a named
function. (`selfmethod` stays reserved.)

## Type check

`closureTypeCheck`, in the order it does things:

1. **The signature expected**: a function reference's, `&fn(...)`, when the
   position is a parameter of that type; the bound of a generic parameter, set in
   `closureHint` by the call (below); none elsewhere. Parameters written without
   a type take theirs from it (`ErrorClosureParm` if there is none, or the counts
   differ). A literal that holds and borrows nothing, given where a function
   reference is wanted, is lifted to a plain function and is `&` of it. One that
   holds or borrows is refused there, saying so (`ErrorClosureForm`); `&<fn` is
   what takes it.
2. **The state's values** are checked where the closure is made
   (`iexpTypeCheckCoerce` with no expectation, so a literal takes its default
   type); each value's type is its field's.
3. **The permissions are found by trying.** Whether a borrow is `&` or `&mut`,
   and whether `()` takes `self &` or `self &mut`, is what the body does. The
   body is checked as a clone, its diagnostics counted and not printed
   (`errorSilent`), first with the weakest permissions; if that fails, with
   every borrow the variable allows `&mut` (a variable is borrowed `&mut` only
   if it is `mut`) and `self &mut`; if that passes, each `&mut` borrow is taken
   back in turn while the body still checks, and `self` last, which stays `&mut`
   while any borrow is. A failure that survives the most permissive guess is the
   body's own, and the real check reports it. This asks the type checker itself
   what the body needs, so a method called on a captured variable, an assignment
   through one and a nested closure's needs are all found with no analysis of
   their own. A clone is made of the literal before anything is checked
   (`cloneClosureNode`).
4. **The hidden struct** (`closureBuild`) is a `StructNode` named `closure#N`,
   joined to the module of the enclosing function and added to its nodes so that
   it is generated. Its fields are the state entries, then a field for each
   captured variable holding `&` or `&mut` of its type. Its one method is `()`,
   `pub`, a `FnDclNode` whose `closure` field (a `ClosureInfo`) says what the
   body holds; its parameters are the literal's after a `self` of the struct, its
   body the literal's block. It is checked as any struct is, the method from the
   members queue.
5. **The literal becomes `new Hidden(state values, &variables)`**, with its
   arguments already checked (`typeLitNewArgsChecked`).

**A name in the body is a field.** `nameUseTypeCheck` asks `closureUse` first: a
name use bound to a state entry or a captured variable, in a function that has
a `ClosureInfo` naming it, is replaced by `self.field`, and by the place the
borrow points at, `*self.field`, for a captured variable. Type check replaces the
node through the pointer to it, as it does for a bare field.

**The return type** is the written one, the expected signature's, or read off the
paths. Read off, the signature holds `void` while the body is checked, and
`closureImplicitReturn` and `closureReturnTypeCheck` (called from `fnDclTypeCheck`
and `returnTypeCheck` for a function whose `ClosureInfo` says `retinfer`) set it
from the first return the check meets and hold the others to it. An `if` whose
branches disagree says `ErrorClosureRet`, in the closure's words, from the one
place that finds it (`iexpMultiInfer`, told by `closureInferring`). A body with
nothing to hand back returns nothing.

## A closure given to a call

A closure literal among a call's arguments is not checked with the others when the
callee is generic or an overload set (`fnCallTypeCheck`): it waits for the rest,
which say what its signature is, then `fnCallClosureArgs` checks it.

- **A generic.** `genericClosureSig` finds the signature bound of the type
  parameter the argument's parameter is (or is a reference to), reads the other
  type parameters off the other arguments (`genericInferType`), and clones the
  bound with them. A parameter that is `&F` or `&mut F` is given the literal
  lent as a temporary, a borrow of it, as `&make()` is.
- **An overload set.** The literal is considered only against the overloads whose
  parameter there is callable (a function reference's signature, or a generic
  parameter bound by one), so a number's overload never takes it; among those,
  the ones taking as many parameters as it writes; and among several, the ones
  whose callable has exactly the types the literal writes. One left is chosen;
  more than one is `ErrorClosureOverload` naming them and saying to write the
  parameter's types; none is `ErrorClosureOverload`. The body is checked against
  none of them.

## Across objects

A closure written in a body an importer expands (an `inline` function, a generic's
instance) is made again by every object that expands it. `ClosureInfo.expanded`
says so, and the object defines the hidden struct's methods, internally
(`genlIsDefinedHere`, `genlImportedInstances`). Its name is the object's own
(`closure#N`), so two objects' closures never meet at the linker.

## Hazards

- **An unlowered literal.** A refused closure is replaced by an error node;
  `--checktree` reports one still in a tree that had no errors.
- **The attempts are real checks.** A clone of the body is checked to find the
  permissions, up to the number of borrows plus three times, so a closure inside a
  closure is checked that many times over; depth beyond two is slow. What an
  attempt instantiates stays instantiated.
- **A swap of a captured variable** reports its refusal in flow, after the
  permissions are chosen, so it is judged under the weakest guess that passed
  type check.
- **State initializers see the code around**, not the state before them:
  `[a = 1, b = a]` takes `a` from outside.

## Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `parser/parseexpr.c` | `parseClosureFn`, `parseClosureArrow`, `parseClosureArrowBody` | both forms; `parseAssignFrom` reads a body that changes something |
| `ir/exp/closure.c` | `closureNameRes`, `closureNoteUse` | resolve a literal; note a variable its body names |
| | `closureTypeCheck`, `closureTry`, `closureBuild` | the lowering; a guess under which the body is tried; the hidden struct |
| | `closureUse`, `closureSelfParm` | a name that is a field; the `self` a member is reached through |
| | `closureImplicitReturn`, `closureReturnTypeCheck` | the return type read off the paths |
| | `cloneClosureNode` | a copy of a literal, for a generic's instance and for the attempts |
| `ir/exp/fncall.c` | `fnCallClosureArgs`, `fnCallClosureOverload`, `fnCallCallableParm` | closure arguments of a generic or an overload set |
| `ir/meta/generic.c` | `genericParmBound`, `genericClosureSig` | the signature a generic's bound gives a closure |
| `shared/error.c` | `errorSilent` | diagnostics counted and not printed |
