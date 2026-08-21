Two changes, and the second is the prerequisite of the first.

## What has to be true

A namespace entry must be able to bind a name to something declared elsewhere
while carrying its own spelling and its own visibility.
[[packages-and-separate-compilation|Packages and Separate Compilation]] needs that for import folding — the binding
for an imported package is private inside the importing module while the package
itself is public, and no spelling can say so. Name-folding into a **type** needs
the same record, where it is delegated inheritance.

Visibility becomes a bit on every named node. The `_` spelling writes it once
when a declared name is built and is never read again; every check reads the bit.
`0x0100` looks free in `flags` across every named family — the low byte and
`0x4000`/`0x8000` are taken — but `flags` is not one namespace and a collision
has no diagnostic, so confirm rather than assume.

## NameAlias, not a universal binding record

Declarations keep their representation. Nothing is converted. Add one node:
`NameAliasNode`, holding its own `namesym`, its own flags including the private
bit, and a target `INode*`.

**Identity questions stop at the alias; substance questions pass through.**

| Question | Answered by |
| --- | --- |
| Is it reachable from here? | the alias's bit |
| What is it called, and where written? | the alias — diagnostics want the target's too |
| Uniqueness in this namespace | the alias's spelling |
| Type, value, callable, namespace? | the target |
| Signature, layout, generated symbol | the target |
| Assignment or declaration target | never an alias |

Four consequences worth having:

- **An alias emits nothing, so it carries no code-generation provenance.** That
  stays on the declaration, where `FlagExtern` and `FlagGenMod` already are.
- **`typedef` becomes an ordinary name alias.** It is already an
  alias-to-a-type that resolves through.
- **`FnOverloadDclNode` is already a proto-alias** — a namespace entry that is
  not a declaration, holding a name and what it refers to. Generalize from it
  rather than inventing beside it.
- **Alias cycles are caught by type check's `TypeChecking`/`TypeChecked` marks**,
  the same machinery that reports `ErrorCircular` for a type containing itself.
  No separate cycle detection.

## Capability predicates replace tag-group arithmetic

`isExpNode`, `isTypeNode` and `isMetaNode` stop being mask tests over the tag's
group bits and answer from the node's characteristics, following the chain —
name use to alias to declaration.

**This already exists once, by hand.** `itypeIsGenericType` is the only thing
making `isTypeNode` more than a mask test, and what it does is follow `objfn`'s
`dclnode` to see whether it lands on a `StructTag` carrying generic info. It
exists because a tag cannot express "an unlowered `Box[i64]` is a type."
Generalizing that special case is the change.

**No third "unknown" answer is needed.** A node's role is never asked before name
resolution finishes: parse builds, name resolution binds, and from type check
onward the chain exists to answer from. The walk costs more than a mask test,
and that cost is accepted deliberately.

`NameUseNode` then stops being retagged, and its five destinations go:
`VarNameUseTag`, `MbrNameUseTag`, `TypeNameUseTag`, `MacroNameTag`,
`GenericNameTag`. Measured: **`GenericNameTag` is assigned zero times and read
three.** A classification predicted at retag time rather than asked at use time
is one that gets forgotten.

## Stages

Add characterization cases for method calls, operators, qualified names,
shadowing, aliases, imports and duplicate names before the first behavioral
change. Each stage below changes something a scenario can assert.

1. **Capability predicates, alongside the existing retagging.** They answer
   correctly whether or not a node was retagged, so nothing breaks while
   consumers still dispatch on tags. Observable: a generic instantiation used as
   a type answers from its characteristics rather than from a special case.
2. **Move consumers off the retagged tags**, family at a time. Measured reads to
   migrate: 26 of `TypeNameUseTag`, 44 of `VarNameUseTag`, 17 of
   `MbrNameUseTag`.
3. **Stop retagging.** `NameUseTag` becomes one tag.
4. **Add `NameAliasNode` and the private bit, for struct name folding first.**
   Delegated inheritance does not exist yet, so this is additive and lands with
   its own scenarios rather than replacing working behavior. Route the six
   visibility decisions off the spelling and onto the bit — `nameUseNameRes`,
   `fnCallLowerMethod`, `typeLitStructReorder` and `genlGloVarName` read
   `namestr == '_'` directly; only `genlProgram` and `importNameRes` go through
   `inodeIsPrivate`, which inverts into the initializer that stamps the bit.
5. **Convert module import folding onto it.** Observable: whether a folded name
   transits stops depending on module load order.
6. **`typedef` becomes an alias.**

**Stage 4 needs 1–3 first.** While `isTypeNode` is a mask test, an alias to a
struct answers wrong, and does so silently.

## Dispatch on the declaration, not the node shape

The same principle applied to the call node, where it is measurable today.

**How far `fnCallTypeCheck` is from resolving its callee before it dispatches.**
Measured by instrumenting its front end and compiling all 122 corpus sources.
[[analysis-re-factor|Analysis re-factor]] made the *guarantee* available that a reached declaration
is analyzed; the dispatch was not restructured to use it.

- **The callee is not checked for the very case that needs it.**
  `calleeIsOverload` peeks at `dclnode->tag` and then does *not* type check the
  callee. That path fires 21 times, and in all 21 the callee had not been checked
  by anything else — 0 of 21 arrived already checked. So the ~50 lines below it
  run in two modes, callee-resolved and callee-unresolved, with nothing marking
  which. The downstream `self.method` test confirms it: 2946 arrivals with a
  checked declaration, 22 without. An invariant held by convention rather than by
  construction is what the [[analysis-re-factor|Analysis re-factor]] revert was about.
- **Arguments are checked in the wrong order.** They are type checked *above* the
  line that resolves the callee, so argument checking cannot see parameter types
  and no expected type can be pushed down into an argument. Recorded from the
  coercion side in [[type-inference-and-coercion|Type Inference and Coercion]].
- **The dispatch leads with syntax.** The first two decisions — macro, and `<-`
  on a value tuple — are pattern matches on `objfn->tag` and `methfld`'s
  namesym, taken before anything is analyzed. At entry the callee's declaration
  is already `TypeChecked` in 3736 cases and not in 41, so the front end cannot
  rely on it and does not try.

**What would collapse the front end.** Resolve `objfn` first, macros excepted —
they must not have arguments checked before substitution — and then dispatch on
what the *declaration is* rather than on what the *node looks like*. It composes
with [[lexer-and-parser|Lexer and Parser]] giving `()` and `[]` distinct node shapes: fewer
syntaxes per shape makes the leading tests smaller, and resolve-first makes the
rest answer from the declaration instead of re-deriving from syntax. Neither
alone is enough.

## Owned elsewhere

- `extern` merging and code-generation provenance:
  [[packages-and-separate-compilation|Packages and Separate Compilation]], step 9.
- Selective folding, `as`, `except` and `pub` re-export:
  [[using-and-module-name-folding|Using and Module Name-folding]], and step 5 of the same item.
- Generic, macro and lexical namespaces, and extensions contributing members:
  their own items, once the record exists.
