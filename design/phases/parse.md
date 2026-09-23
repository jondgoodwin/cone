Parsing turns source text into IR nodes. It is the only phase that reads files,
and it finishes with every module in the program loaded.

Read this before changing syntax, before adding a node the parser must build,
or when trying to work out whether a problem is the parser's or name
resolution's — the boundary is not where most readers expect it.

*Provenance: read from source; the precedence cascade, `parseType`'s delegation
to `parsePrefix`, and parse-time namespace population were traced end to end.
Section 11 is what is unverified. See [Measuring](../diagnostics/measuring.md).*

## 1. Principles — [derived]

⚠ **Read from source and traced end to end.** **Unchecked with the author is the
claim that these rule rather than describe.**

1. **One grammar for types and values.** `parseType` dispatches every
   type-starting token to `parsePrefix` — the *value* expression parser. No
   separate type grammar, no backtracking. ▸ **Forbids** a syntax that can only
   be disambiguated by knowing whether a type or a value is expected, and
   **settles** that a new type form costs an arm in the value parser. The one
   bit of position the parser carries is `ParseState.inrettype`, set while
   `parseFnSig` reads a return type: a `{` there opens the body of the function
   being declared, so `&fn` is read as a signature alone and leaves the block to
   its owner.
2. **The lexer is line-blind.** It counts lines for diagnostics and nothing
   else: a block is delimited by braces and a statement ends at `;`, so
   indentation, line ends and columns carry no meaning to the grammar. ▸
   **Forbids** any syntax that reads where a line begins or ends, and
   **settles** that the token stream is separable from parse state — the lexer
   never needs to be told what the parser is doing.
3. **The parser desugars.** `match`, `each`, `while`, `with`, bound patterns and
   several prefix forms are lowered here into blocks and `if` chains. ▸
   **Settles** that later phases never see those forms, so a new sugar costs no
   node, no dispatch arm and no phase work.
4. **The parser binds module-level names.** Module namespaces are populated,
   hooked into the global name table, and duplicate-checked *during* parsing. ▸
   **This is what lets name resolution have no lookup routine** — by the time it
   runs, every module-level name is already in its slot.

▸ **Principles 3 and 4 are the two most readers get wrong**, and both move the
boundary with name resolution earlier than expected.

## 2. The lexer

**One token of lookahead, and no more.** The global `Lexer *lex` holds exactly
one current token — `toktype`, a value union, and `langtype` for a literal's
explicit type suffix. There is no peek, no pushback, no token buffer. The parser
is strictly LL(1) at the token level. What lookahead exists is character-level
inside the scanner: a few characters for maximal-munch operators (`<=>`, `+[]`,
`&[]`, `>>=`), and a rewind inside `lexScanChar`, which scans an alphanumeric
run and then checks for a closing `'` to tell a lifetime (`'a`) from a character
literal. One reaches past the current token, still at the character level and
lexing nothing: `lexNextIsWord` reads the source after it for a keyword, which
`parseIsFoldClause` asks of a `pub` written after a declaration. `pub` comes
first, so a fold clause may begin `pub use`; any other `pub` there begins the
next statement after a missing `;`, which is still reported as the missing `;`.
Only white space may come between the two words.

**`..` and `...` are the range tokens** (`DotDotToken`, `EllipsisToken`), read
today only by a match's range pattern. A number stops scanning at a `..`, so
`0..3` is two integers and a range, not the float `0.`.

**One `Lexer` per source, on a linked list.** `lexInject` pushes, `lexPop`
restores. **Blocks are never recycled**, deliberately: every IR node stores the
`Lexer` current when it was built and reads `url` from it whenever a diagnostic
is reported, so reusing a popped block rewrites the file name out from under
every node still pointing at it. A block can also exist before it is current:
`lexLoadPath` reads a file into one without pushing it, so a module can take
its designated file's first line as its position before that file is parsed,
and `lexPush` makes the same block current later — see `nodes/module.md`,
"Parse".

**An identifier may be spelled in any letters UTF-8 can carry**, which is
`utf8IsLetter`: ASCII letters, or the start of a well-formed multi-byte
character. **Well-formed is the load-bearing word.** A byte is only a character
if it leads a sequence whose continuation bytes are actually there, so a stray
byte is refused as a token (`ErrorBadTok`) rather than absorbed into a name, and
the scan resumes at the byte after it rather than at the length its lead byte
claimed. `utf8ByteSkip` never advances past the character in front of it, which
is what keeps a malformed byte from consuming the source that follows.
`lexical-reject-tokens` holds both shapes.

**An integer literal is 64 bits wide at most.** `lexScanNumber` accumulates
into a `uint64_t` and refuses a digit that would carry past it
(`ErrorLitOverflow`), once per literal and after every digit and the suffix
have been consumed, so the token still ends where it should and the parser
carries on with it. The digits of a float are exempt: they are read again by
`lexToFloat`, so a mantissa wider than 64 bits is a value, not an overflow.
`lexical-reject-overflow` holds the boundary in both bases.

**Names are interned at scan time, and `Name.node` is the binding slot.**
`nametblFind` returns one immovable `Name*` per unique string. That same
`node` field is what makes classification O(1) in the scanner: `keywordInit`
binds each keyword's `Name.node` to a `KeywordTag` node whose `flags` carry the
token type. Permissions reach the same effect by a different route:
`stdPermInit` binds each permission name's `node` to the `PermNode` itself, and
`lexScanIdent` has a separate branch turning a `PermTag` binding into a
`PermToken`. So `mut` and `uni` are lexically distinguished without being
keywords — copy the right one of these two patterns if you add a third family. A reserved word is reported once,
at first use, and then **released** — `Name.node` is cleared and the word
continues as an ordinary identifier, so the rest of the compile is not derailed
by it.

**Every token the lexer returns has a reader.** A spelling no feature reads is
reported in the lexer and never reaches the parser, because a token nothing
consumes is a cascade of parse errors behind it. Three are refused this way, each
with one diagnostic. An attribute is a keyword (`@move`, `@opaque`, `@unsized`),
so any other `@` word is `ErrorUnkAttr` and is dropped, the declaration read
without it; `@samesize` gets its own wording, since an enum is same-size by
default and `@unsized` declines it. A `#` word is held for metaprogramming:
`ErrorReserved`, and it is dropped with the rest of its line, so what `#if`
was followed by is not reported again. `?.` is held for None propagation:
`ErrorReserved`, and it is read as `.`. `lexScanIdent` returns 0 for a dropped
word and `lexNextToken` scans on from where it stopped.
`lexical-reject-unbuilt` holds all three.

**A word held for an unimplemented feature is reserved; a word that names an
unbuilt *kind of declaration* is a token.** `mod` and `actor` are the two kinds
that carry abstractions, so both are ordinary keywords with an arm of their own in
the global dispatch. `mod` builds a declaration — it declares the module a
folder's files belong to, [module](../nodes/module.md), "The `mod` declaration".
`actor`
does not, and its arm reports `ErrorUnbuiltKind` where the declaration is written
and names the abstraction's spelling, `actor trait`; the two shapes of `mod` that
are unbuilt, a nested block and `mod trait`, report the same code the same way.
Admitting a shape is what settles its spelling now instead of leaving it to be
designed when the semantics land, and reporting it is what keeps a declaration
from being accepted with nothing under it. The cost is the standing one in the
hazards: holding a word takes it away from every program.

### Blocks and statement ends

A block is `{`, statements, `}`. A statement that does not end in a block ends
at `;`, and nothing stands in for the `;`: not the end of a line, not the `}`
that closes the block, not the end of the file. The lexer records no
indentation and no column; `lexNewLine` counts the line for diagnostics and
that is all, so a statement runs on across lines until its `;` arrives, and an
operator that starts a line — `-`, `*`, `&`, `.`, `(`, `[`, each of which also
reads as a prefix — continues the expression before it. The `;` before a
closing `}` is required like any other; a block's value is its last
statement's value with or without one (see [block](../nodes/block.md)), so
requiring it costs nothing.

`parseBlockStart` consumes the `{` and `parseBlockEnd` the `}`, reporting
`ErrorNoRCurly` at end of file. `parseEndOfStatement` consumes the `;`, and
otherwise reports `ErrorNoSemi` **after the token the `;` should have
followed**: `errorMsgLexAfter` reads the end of the previous token, which the
lexer keeps in `prevend`, so the diagnostic lands at the end of the statement
rather than on whatever the next line happens to start with. A `:` where a
block should start is `ErrorColonBlock`, reported by name because that is the
one syntax a reader of older Cone will reach for.

## 3. The precedence cascade

Hand-written recursive descent, one function per level. A level that **loops**
over the next tighter one is left-associative, and most are. Loosest to
tightest, in `parseexpr.c`:

`parseAssign` → `parseTuple` → `parseOrExpr` → `parseAndLogic` →
`parseNotLogic` → `parseCmp` → `parseOr` → `parseXor` → `parseAnd` →
`parseShift` → `parseAdd` → `parseMult` → `parseCast` → `parsePrefix` →
`parseSuffix`(`parseTerm`)

**Three things differ from C and will surprise you:**

- **Bitwise binds tighter than comparison.** `a | b == c` is `a | (b == c)`
  in C and `(a | b) == c` here.
- **`not` binds looser than comparison**, and `parseCmp` is non-associative —
  one comparison per level, so `a < b < c` does not chain.
- **Assignment is right-associative.** `parseAssign` does not loop; it recurses
  into `parseAnyExpr` for the right-hand side, as do `:=`, `<=>` and every
  op-assign form.

`parseCmpOp` is the comparison level's one list of operators — `==`, `!=`,
`===`, `!==` and the four orderings — and a `match` pattern reads the same list
(section 6). The lexer takes `===` and `!==` whole ahead of `==` and `!=`.

Two entry points: `parseAnyExpr` (= `parseAssign`) is the full expression;
`parseSimpleExpr` (= `parseOrExpr`) excludes comma and assignment and is what
arguments, conditions and array elements use.

**`parseSimpleExprFrom` resumes the cascade** above an operand already parsed
by `parseOr`: the comparison, `and` and `or` levels each have a `...From` form
taking their left operand. It exists for a match's case, which cannot tell a
range pattern (`0 .. 3`) from a condition (`n > 3`) until it has read the first
operand and seen whether `..` or `...` follows. With one token of lookahead,
reading the operand and then continuing is the only way to decide.

## 4. What the parser leaves undecided

A name is left **bound to nothing**: `NameUseTag` with `dclnode` NULL. Name
resolution binds it and changes nothing else; what the name is — a type, a
value, a macro — is asked of the declaration from then on. Seven other node
kinds are **shape-stable but tag-unstable**: the parser builds the right fields
and the wrong tag, and name resolution retags once the names inside have bound.
`inode.h` labels `NameUseTag`, `TupleTag` and `StarTag` explicitly as
"parser-ambiguous".

**The period is undecided in the same way.** `a.b` is a member of a value when
`a` is one and a path through a namespace when `a` names a module or a type,
and the parser builds the member-access shape for both. Name resolution folds
away the ones that were paths.

| Built as | Becomes | Retagged in |
| --- | --- | --- |
| `TupleTag` | `TTupleTag` (all types) or `VTupleTag` (all values); mixed is `ErrorBadElems` | `ttupleNameRes` |
| `StarTag` | `PtrTag` if the operand is a type, else `DerefTag` | `ptrNameRes` |
| `ArrayTag` | stays a type, or becomes `ArrayLitTag` | `arrayNameRes` |
| `RefTag` | stays a ref type, or becomes `BorrowTag`/`AllocateTag` by region | `refNameRes` |
| `ArrayRefTag` | stays a ref type, or becomes `ArrayBorrowTag`/`ArrayAllocTag` | `arrayRefNameRes` |
| `QuesTag` | `FnCallTag` for `Option[T]`, or folds into an `AllocateTag` with `FlagQues` | `allocateQuesNameRes` |
| `FnCallTag` holding `a.b` | a bound name use, when `a` names a module or a type: the period was a path | `fnCallNameResPath` |
| `FnCallTag` | `ArrIndexTag`, `FldAccessTag`, `TypeLitTag`, an instantiation, or a real call | `fnCallTypeCheck` |

This is what principle 1 costs, and it is the whole cost: because a type and a
value parse identically, `*T` and `*p`, `&T` and `&x`, `(A,B)` and `(a,b)` are
one production each, and one retagging pass settles all of them.

Also left undecided: **what a pattern's bare name means.** It may be a variant of
the matched value's enum, which only type check knows, so the parser marks it
(`FlagPattern`) and leaves it. And **which method an operator names** — every operator is an
`FnCallNode` with `methfld` set to the operator's interned name and
`FlagOperator` set, and selection is type check's. And **whether `&fn` is a
closure or a function-signature type** — `parseAmper` decides by whether a body
follows, except in a return type (`ParseState.inrettype`), where it is always a
signature because the block that follows is the declared function's own.

## 5. Adding an operator: the six edits

The trail crosses parse, corelib and generation, and no single phase owns it.

| Step | Where |
| --- | --- |
| 1. Lex the spelling | `lexNextToken`, if it is not already a token. Maximal munch — a longer operator must be tested before its prefix |
| 2. Intern a name for it | `nametblInit` in `ir/nametbl.c` (`plusName = nametblFind("+", 1)`), declared `extern` in `ir/name.h` |
| 3. Give it a precedence | a level in the `parseexpr.c` cascade (section 3), building `newFnCallOpname(lhnode, plusName, 2)` — which sets `FlagOperator` |
| 4. Declare the method on each type that offers it | `corenumber.c`, `iNsTypeAddFn(..., newFnDclNode(plusName, FlagMethFld, sig, newIntrinsicNode(AddIntrinsic)))`. **This is C, not Cone source** |
| 5. Add the intrinsic | the `Intrinsic` enum, then an arm in `genlFnCallInternal`'s switch — which dispatches on the LLVM *type kind* of argument 0, not the Cone tag |
| 6. Map its assignment form | `fnCallOpEqMethod`, if there is a `+=` counterpart |

A user type opts in by declaring a method under the operator's backticked name
(`` fn `+`(self, other Self) Self ``), so steps 4 and 5 are only for the
built-in types. Selection among candidates is
[Type Check Reasoning](type-check-reasoning.md) section 7.

## 6. What the parser decides that you would expect it not to

**It desugars.** `match` becomes a block holding an anonymous capture variable
plus an `if` chain, each case's patterns becoming its condition: `is T` an `is`
node, `<v` (and every comparison) the operator call with the captured value on
the left, `a .. b` and `a ... b` two calls joined by `and`, `or` between patterns
a logical `or`, and an `if` guard an `and` after them ([if](../nodes/if.md)).
`while c {…}` becomes a loop block with `if not c {break
nil}` inserted first. `each x in a < b by s` becomes an outer block holding the
loop variable plus a loop block whose last statement is the synthesized step,
flagged `FlagLoopStep`. That step is a block wherever the value it steps to could
wrap past the type's extreme and pass the guard again. For an inclusive range
without `by` — `a <= b`, `a >= b` — it is `{ if x == b {break}; x++ }`, with the
bound cloned for the second comparison. With `by` it is `{ imm prev = x; x += s;
if x < prev {break} }`, `>` for a range counting down, since a step of more than
one need not land on the bound and the wrap is only visible after the step, as a
move against the range's direction; `prev` is a phantom variable the parser
resolves itself. Either block stays one statement so a `continue` carries the
guard with the step. `with e {…}` becomes a block with a `this` declaration
first. Prefix `.f` becomes `this.f`. `else if` folds into `elif`. Unary minus on
a literal is constant-folded in place.

**It binds module-level names.** `modAddNode`, `modAddNamedNode` and `modAddFn`
run *during* parsing, so by the time a module's parse finishes its namespace is
populated, `ErrorDupName` and `ErrorOverloadClash` have already been reported,
overload sets have their `FnOverloadDclNode`, and every declaration records the
module as its owner. The stated reason is that permissions and allocators do not
support forward references, so their names must be in the table as they are
read. **A `mod` declaration respects that**: it binds the module's own name the
same way, at the point it is read, which is why it has to be the file's first
statement.

This is why `nameUseNameRes` is a single assignment from `namesym->node` —
see [Name Resolution](name-resolution.md).

**It loads every module it must, and it finds a module's files itself.** `import`
answers the name it is given in the **registry** first — the importing module's
parent's namespace, where a sister is already bound — and loads nothing when that
answers. Otherwise the name is a path, and the module is recursively loaded and
*fully parsed* during the parse of the importing one. Either way an `ImportNode`
joins a list kept separate from the module's own nodes, so folding can run before
any module's names resolve. `include` is different: it injects the file and parses
its global statements straight into the **current** module, producing no node.
Corelib is parsed before the main file and wildcard-imported into every module.

**A module is the files of a folder**, so loading one module means reading
several files. Loading is three steps — locate the file, ask the **file registry**
which module holds it, parse each of the module's files into it — and the registry
is keyed by the file's **canonical** path, so a file is read exactly once and
belongs to exactly one module however the path to it was spelled. What decides
whether a folder is swept is the **designated-file convention**: the file the
compiler is given sweeps its folder when it is the file named for that folder.

**And the same probe decides every subfolder, so the parse builds a module TREE.**
A subfolder holding its own designated file draws a submodule — a module of its
own, owned by its parent and bound in its namespace — and any other subfolder is
organisational, its files joining the enclosing module at any depth. **Every
submodule of a level is drawn before any of them is parsed**, so a sister is a
name of the parent's namespace, and her files are registered, before any file can
name her; and all of them are drawn before the parent's own files are parsed, so
a name a subfolder put in the namespace is there before any statement can collide
with it. [module](../nodes/module.md), "The folder tree", owns the rules and the
diagnostics.

## 7. Contract

**True when `parsePgm` returns:**

- One `ProgramNode`; every module reachable by import is parsed, and every file
  of every one of those modules with it. No later phase reads a source file, and
  the file registry holds every file that was read.
- Every node carries `lexer`, `srcp`, `linep`, `linenbr`, `tag`, `flags`, and
  `instnode == NULL`.
- Every identifier is an interned `Name*`. String comparison never happens
  again.
- Module namespaces are populated and hooked; duplicate globals already
  reported.
- A few `NameUseNode`s are **pre-resolved** — the anonymous variables desugaring
  synthesizes — with `dclnode` already set. `nameUseNameRes` returns immediately
  for these.
- Blocks always have a non-NULL `stmts` list.

**Not yet true:**

- No name other than those pre-resolved few is bound.
- Nothing has a real type; `vtype` is `unknownType` almost everywhere, and
  `parseType` returns `unknownType` for "no type written".
- It is not settled which nodes are types and which are expressions.
- No lowering that needs a type: no `FldAccessTag`, `ArrIndexTag`, `TypeLitTag`,
  no method or overload selection, no coercion, no generic instantiation, no
  macro expansion, no `BlockRetTag`, no alias or drop nodes.
- The tree may contain `NULL` children where recovery gave up — `parseTerm`,
  `parseTypedef` and `parseLifetime` each return `NULL` on a bad input.

## 8. Errors and recovery

Parsing **always runs to EOF**. There is no error limit and no cascade
suppression; `conec.c` gates on `errors == 0` only after `parsePgm` returns, so
nothing downstream ever sees a tree with parse errors. That is what lets
recovery be aggressive: a wrong-but-walkable tree costs nothing, because it will
never be analyzed.

| Mechanism | Behavior |
| --- | --- |
| a spelling the lexer refuses | a reserved word, `?.`, or a `@` or `#` word that names nothing is reported in the lexer and handed on as what it stands for or not at all (section 2), so the parser never sees it |
| `parseSkipToNextStmt` | the main resync; consumes through the next `;`, or stops short of a `}` or EOF for the enclosing block to handle |
| an unbuilt form's body | `parseSkipDclBody` skips a following `{ … }` whole, counting depth, so nothing inside is read as a global statement and reported again, and resyncs at the next `;` where there is no block. `actor`, a nested `mod` block and `mod trait` all use it, so each is reported once, where it is written |
| `parseCloseTok` | reports `ErrorNoRParen`, scans for the closer, gives up at `;`, `}`, EOF |
| `parseBlockStart` | on `:`, reports `ErrorColonBlock` and reads what follows as the block; on anything else that is not `{`, reports `ErrorNoLCurly` and scans forward for one |
| `parseTerm` default | reports `ErrorBadTerm`, consumes one token to avoid an infinite loop, returns `NULL` |
| anonymous placeholders | the declaration parsers substitute `anonName` so the caller always gets a node |
| `parsePgm`'s end-of-file test | `ErrorNoEof` when the main file's global statements stopped short of EOF — a stray `}` ends `parseGlobalStmts`, and this is the one place the parser refuses to finish quietly |

The rationale is written into `parseStruct`: an unnamed type is built under the
anonymous name rather than abandoned, so the body is still parsed — leaving
would drop the whole body on the floor and turn its opening brace into the next
global statement.

**Two conditions abort the process outright**, with no recovery:
a source file `fileFindSrc` cannot find, or `lexInjectPath` cannot read (`ExitNF`), and
`parseFilename` when `import` or `include` is followed by something that is
neither an identifier nor a string (`ExitNF` as well, despite being a malformed
token rather than a missing file).

Diagnostics come from `errorMsgLex` (position from the lexer — the parser's
workhorse), `errorMsgNode` (position from a node, plus the instantiation trace),
and `errorMsg` (no source context). The parser owns roughly two dozen
`ErrorCode`s; `shared/error.h` is the list, and `test/codes.toml` pins the
numbers.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `parser/lexer.c` | `lexInject`, `lexInjectPath`, `lexPop`; `lexLoadPath`, `lexPush` | push and pop a source on the lexer chain. `lexInjectPath` reads an already-located file: locating one is the caller's, since the path is what the file registry is keyed by. `lexLoadPath` and `lexPush` are its two halves apart — read a file into a block that is not yet current, and later make that block current |
| | `lexNextToken` | the scan dispatch; whitespace, comments, maximal-munch operators |
| | `lexScanIdent` | identifier scan and name-table classification; reserved-word release; a `@` or `#` word that names nothing reported and dropped |
| | `lexScanNumber`, `lexScanString`, `lexScanChar`, `lexScanEscape` | literals; UTF-8 re-encoding of escapes; lifetime-vs-char disambiguation |
| | `lexNewLine`, `lexBlockComment` | line counting for diagnostics, inside comments included |
| `parser/parsemod.c` | `parsePgm` | **entry point** — tables, program, main module, corelib, main file |
| | `parseGlobalStmts` | the global statement dispatch loop; `trait` by itself enters `parseStruct` with `TraitType` already set, and `actor` is the unbuilt kind refused here. It is told whether it is reading the start of a module's own source, which is what decides where a `mod` declaration may stand |
| | `parseModuleDcl` | `mod name;`, the declaration a module's designated file makes: the placement rule, the check against the folder's name, the rename a one-file module still gets, the module's own name bound into its namespace, what `pub` does for a submodule and why it is refused on any other module, the `extends` it records for name resolution to resolve, and the refusal of the nested block and `mod trait` |
| | `parseSkipDclBody` | skip an unbuilt form's `{ … }` whole, or resync at the next `;` |
| | `parseLoadAndParseModuleFile` | per-module unit: locate, register by path, naming, the folder sweep, corelib import, `modHook`, and a parse per file |
| | `parseDesignatedFolder`, `parseCollectFolder`, `parseModuleFiles` | the folder sweep: whether the file is its folder's designated file, which files the folder brings in, which subfolders draw submodules, and the designated file too deep to draw one |
| | `parseSubmodule`, `parseModuleTree` | the module tree: the submodule a subfolder draws — owned by its parent, bound in its namespace, private to it unless `pub` — and the order, submodules before the module's own files |
| | `parseRegisterModuleFiles` | the file registry entries for a module's files, and the two collisions that stop a file joining |
| `shared/fileio.c` | `fileFindSrc`, `fileFolderScan`, `fileDesignatedFile` | locate a source file without reading it; list a folder's `.cone` files and subfolders, sorted; probe a folder for the designated file that makes it a module folder |
| | `parseImport`, `parseInclude` | the two source-composition forms |
| `parser/parsehelper.c` | `parseBlockStart`, `parseBlockEnd` | `{` and `}`, with recovery |
| | `parseEndOfStatement`, `parseSkipToNextStmt`, `parseCloseTok` | the required `;`, and the two resyncs |
| `parser/parseexpr.c` | `parseAnyExpr`, `parseSimpleExpr` | the two expression entry points |
| | `parseAssign` … `parseMult`, `parseCast` | the precedence cascade (section 3) |
| | `parsePrefix`, `parseAmper`, `parsePlus` | prefix operators; borrowed and region-managed references |
| | `parseSuffix`, `parseDotCall`, `parseArgs`, `parseArg` | postfix `.`, `()`, `[]`, `++`, `--`; named values. The `.` production serves a member of a value and a path through a namespace alike |
| | `parseTerm`, `parseNameUse`, `parseArrayLit` | literals, parens, blocks-as-expressions, names |
| `parser/parsetype.c` | `parseType` | the type dispatcher that delegates to `parsePrefix` — principle 1 |
| | `parseStruct` | struct/trait/enum: the optional `trait` modifier on the kind, generics, the base clauses — the `is` list and the `extends` base, read in a loop so that a type may write both, each once, with `extends` refused on a trait and meaning *the enum whose variants join this one's set* on an enum — fields, methods, macros (a method when parameter 0 is `self`), an enum's variants in both spellings, tag-field synthesis and the `IsTagField` mark on an enum's discriminant |
| | `parseAddVariant`, `parseVariantTagPin` | joining a variant to its enum: the closed flags, the synthesized base link, the tag value written or assigned in sequence, and its name, bound in the enum's namespace while the node joins the module's list. A variant of an enum that *extends* another keeps the unassigned sentinel unless a value was written, because that enum's numbering continues from its base's last and the base is not resolved yet |
| | `parseEnumExtensionMember` | what an enum extending another may not declare — a member of any kind, since a field or method every variant carries would have to reach its copies of the base's variants too, and a requirement declared there would need every copy to implement it; such a member belongs on the base, and comes along with the copies. Nor a discriminant: none is synthesized for such an enum either, its base's arriving with the fields name resolution splices in |
| | `parseIsTagType`, `parseTagType` | `tag` recognized where a field's type is written and nowhere else, so it is not a reserved word |
| | `parseFnSig` | parameters, `Self` inference, single or tuple return type |
| | `parseVarDcl`, `parseFieldDclBody`, `parseConstDcl`, `parsePerm` | the declaration forms; a field's or a global's trailing `use` clause goes to `parseFoldClause`, and one on a local, a parameter or a static is `ErrorBadFold`. A field's node is built while the lexer is still on its name, both so a diagnostic points there and so an enum's body can decide between a field and a bare-name variant afterwards |
| | `parseFoldClause` | `use *` with an optional `but` list, or a list of names each with an optional `as`; builds the clause on the field and an alias per listed name, bound by name resolution |
| | `parseUseSibling`, `parseUseEnum`, `parseUseAdmits` | a `use` standing as a statement: in a type body it folds a sibling in, and at module scope (`parseGlobalStmts`) it folds an enum's variants in, held on an `EnumUseNode`. Both name their source and then share what follows it — every member by default, `*` refused as saying nothing more, a list with `as`, a block, or `but` |
| `parser/parsefnflow.c` | `parseFn` | function/method declaration — **despite the file name, this is where declarations and control flow are parsed, not data flow analysis** |
| | `parseGenericParms`, `parseMacro` | the type parameter list, shared by `fn`, `struct` and `macro`: comma-separated names only, with a constraint or a parameter type refused as `ErrorGenParmConstr` |
| | `parseExprBlock` | the statement-block loop — the parser's second dispatch table |
| | `parseIf`, `parseMatch`, `parseBoundMatch` | `if`/`elif`/`else` and the `match`-to-`if` desugaring; every pattern's root name is marked (`castPatternMark`) to be looked up in the matched value's enum at type check, as `parseCmp` marks an `is` test's |
| | `parseMatchPattern`, `parseMatchRange` | one pattern of a case — `is`, a comparison, a range — lowered to the condition that tests the captured value; a value alone is `ErrorPatBare`, since whether it means `==` is undecided |
| | `parseWhile`, `parseEach`, `parseWith`, `parseLifetime` | loop and scope desugaring |
| `ir/stmt/module.c` | `modAddNode`, `modAddNamedNode`, `modAddFn`, `modHook` | parse-time namespace population and hook-stack swapping |
| `ir/nametbl.c` | `nametblFind`, `nametblHookPush`, `nametblHookNode`, `nametblHookPop` | interning and the binding stack |

## 10. Hazards

- **`parsefnflow.c` is not flow analysis.** It is function, statement and
  control-flow *parsing*. Flow analysis is `ir/flow.c`. The name has misled
  readers before.
- **A node built after its construct was consumed points at the wrong place.**
  The lexer has moved on. `parseEach` documents the case: position the
  synthesized nodes on the range expression with `inodeLexCopy`, or diagnostics
  land on the token after the body's `}`.
- **Adding a keyword takes a name away from every program.** `keywordInit`'s
  standing argument is that holding a word now costs one rename in a program
  written today, while letting a program bind it costs that program a rewrite
  when the feature arrives.
- **`FlagOperator` is the only record that the source wrote an operator.** An
  operator application and a member access build the same node.
- **Do not add a second type grammar.** Principle 1 is load-bearing; the cost is
  paid once, in the retagging table of section 4.

## 11. Known gaps

None recorded.

## 12. What lives elsewhere

| Question | Note |
| --- | --- |
| What binds the names the parser left unbound | [Name Resolution](name-resolution.md) |
| Lookup, visibility, imports, aliases | [Names and Namespaces](../phases/names-and-namespaces.md) |
| Node header, tags, sentinels, injection hazards | [IR Nodes](../nodes/_index.md) |
