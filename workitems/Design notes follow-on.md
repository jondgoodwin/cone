What `design/` still owes, after [[Design Documentation]] closed.

That item built the folder out — five phase notes, twelve per-node notes, four
northstar notes, two compiler notes, three diagnostics notes, and the conventions
they follow. It is in `workitems/done/` and holds the reasoning for how a design
note is scoped, which still applies to everything below.

## 1. Per-node notes, as they earn one

Twelve are written: `fncall`, `struct`, `references`, `vardcl`, `nameuse`,
`assign`, `cast`, `return`, `block`, `if`, `literals`, `generic`.

**The rule lives in `design/nodes/_index.md`, "Per-node notes"** — one note per
source pair, not per tag, and only for a node whose behavior is spread across
phases and is not recoverable from one file. Not written, and each to be judged
against that rule rather than written reflexively: `array`, `number`, `enum`,
`permission`, `region`, `fnsig`, `fndcl`, `module`, `import`, `logic`, `deref`,
`sizeof`, `swap`, `ttuple`/`vtuple`, `typedef`, `void`. Several are probably
fully explained by their header plus one `*TypeCheck` function and deserve a
manifest row and nothing more.

This needs no scheduling: the change discipline in `CLAUDE.md` already says a
change to a node lands with its note, so these arrive when the code is touched.

## 2. Split `phases/names-and-namespaces.md`

Five phase notes link to it, always for *current mechanism* — lookup,
visibility, imports. What a reader lands in opens with the unimplemented NameDef
design and interleaves aspiration with reality throughout, so someone chasing a
visibility bug cannot tell which half describes the compiler they are debugging.

Its stale rows were corrected and a header now states the split against
`phases/name-resolution.md`, but the real fix is to separate the current rules
from the NameDef design and move the latter to [[Namedef Refactor]]. It also
lacks the provenance line every other note carries, and its source-code manifest
duplicates the phase note's pointer map in a different format.

## 3. A link checker

The notes cross-link heavily and nothing verifies them; `design/nodes/_index.md`
carried eight dangling links for a long time precisely because nothing looked.
Two rules to enforce:

- every markdown link in `design/` resolves;
- **no `design/` note contains a wiki-link to a work item** — the dependency runs
  one way, work item to design note, so that closing an item prompts updating its
  notes rather than leaving a note pointing at something that no longer exists.

Both are currently clean and both were checked by hand.

## 4. Northstar: what the sources say that the notes do not yet

The `northstar/` notes were corrected against the author's own writing —
`conesite/public/*.html` outside `coneref/`, and the posts under
`ProgLing/plingsite/content/post/`. These surfaced during that pass and each
needs a judgement call rather than transcription:

- **Two of the four stated aims are uncovered.** *Fit* — "programs pack a lot of
  power for their size, both as source files and as delivered executables" — has
  no note, and it is unclear whether it is a design principle with content or a
  positioning claim. *Friendly* is covered only on its "easy to change" side, by
  modularity; the readability decisions have rationale in the posts and no note
  here: significant indentation and braces both supported deliberately,
  semicolon inference, `this` in `with` blocks justified linguistically, implicit
  return argued for refactoring consistency.
- **The author's vocabulary is not consistently in the notes**: *gradual memory
  management* (now used), *infectious typing* (his coinage — he is still looking
  for prior art), *delegated inheritance* (now used), the *alias stack*,
  *scope-surfing moves*, and the reference's *quarks*. A note that describes a
  mechanism without his term for it makes the writing and the code harder to
  connect.
- **Flow analysis has a stated organizing spine the note does not use**: four
  contexts — loading, copying/moving, storing, and discarding a value. The alias
  machinery also has a documented rationale (unbound versus stored references,
  returns pre-aliased) that makes the injected nodes look deliberate rather than
  arbitrary, and explains why automatic aliasing is a deliberate divergence from
  Rust: reference use should look the same whichever region allocated it.
- **Other stated commitments with no note**: no exceptions and no default
  nullability (failures return `Result`); a lean, opt-in runtime rather than a
  monolithic framework; the 3D web as the killer app that sets the requirements,
  with a 17ms frame budget as the origin of the memory design; and an explicit
  target user who is *not* a beginner, with the language optimized for reading
  and refactoring over time-to-first-program.
- **One contradiction in the sources, unresolved.** `practical-subtyping.md`
  calls region-owned to borrowed a *runtime* coercion in one paragraph and a
  compile-time recast in its own summary. The code treats it as compile-time.
  Neither is cited as settled.
- **The site says it is aspirational.** `status.html` states outright that it
  "sometimes depicts the language as it is envisioned to be", so any aim taken
  from it needs its distance measured rather than assumed.

## Related

[[Vault and repo sync]] — the same notes exist in `Cone Vault/` and have
diverged. Not urgent, recorded separately.
