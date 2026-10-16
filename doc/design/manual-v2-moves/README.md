# Material moved out of the manual during the v2 rewrite (16 Oct 2026)

Each block is verbatim, headed by an HTML comment giving its source page and anchor, a suggested home and why.
Material folded into the manual's pages has been removed from here (git history keeps the original holding files,
commit c0cbdae76).

- `UNPLACED.html`: good material with no clean home yet: first-class patterns with their parser, jump tables, the
  match assignment form, and the `pi as u64` trust example. (The type repertoire and semantics lists left 16 Oct:
  abstract types reached only by reference, and nominal versus structural types, went to
  `mod-subtyping.html#byref` and `#nominal`; the rest repeated the Guide's data-type pages and Memory, and was cut.)
- `pending-subtype-pairs.html` (removed 16 Oct): the old Type safety page's subtype pairs, folded into
  `mod-subtyping.html` and `safe-types.html#coercion` once "Practical Subtyping" settled
  what counts as subtyping; three pairs had their direction corrected (permissions, raw pointers).
- `to-design-notes.html`: designed but not built (exceptions and the `?` and `||` operators, the `=` method,
  property setters, the Option operators, two-way yield, queued methods, region declarations, and more).
- `to-congo.md`: the tool side of packages (manifest, commands, session, link tables, the `.conebuild` format) for a
  separate Congo document.
