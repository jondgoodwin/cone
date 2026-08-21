The `use` statement and its clause form, and what folding a name means.

**Mostly decided.** The model is in `design/nodes/module.md`
([[module|Module]]), "Composing packages" and "Visibility"; the ordered work is
step 5 of [[packages-and-separate-compilation|Packages and Separate Compilation]]. Depends on
[[tag-group-and-name-aliasing-refactor|Tag Group and Name Aliasing Refactor]] for the binding a folded name needs.

Settled: `use` as a clause of `import` for a package and standing alone for a
namespace already in scope; selective names, `as` renaming, `except` against a
wildcard, a block form for a long list; a fold is private to the module that made
it unless marked `pub`; folding never widens visibility beyond the origin; folds
from every file accumulate into the one module namespace.

Still open here:

- **Name resolution in dependency order, with an error on a recursive cycle.**
  The DAG rule is decided and nothing enforces it. Step 2 owns the enforcement;
  what belongs here is what the diagnostic says when the cycle runs through folds
  rather than through plain imports.
- **Folding into a type is delegated inheritance**, and whether that is literally
  the same operation as folding into a module is open — item 3 of step 0. If it
  is, the `use` grammar has to read sensibly inside a type declaration too.
