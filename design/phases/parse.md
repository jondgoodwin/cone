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
literal.

**One `Lexer` per source, on a linked list.** `lexInject` pushes, `lexPop`
restores. **Blocks are never recycled**, deliberately: every IR node stores the
`Lexer` current when it was built and reads `url` from it whenever a diagnostic
is reported, so reusing a popped block rewrites the file name out from under
every node still pointing at it.

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

Two entry points: `parseAnyExpr` (= `parseAssign`) is the full expression;
`parseSimpleExpr` (= `parseOrExpr`) excludes comma and assignment and is what
arguments, conditions and array elements use.

## 4. What the parser leaves undecided

A name is left **bound to nothing**: `NameUseTag` with `dclnode` NULL. Name
resolution binds it and changes nothing else; what the name is — a type, a
value, a macro — is asked of the declaration from then on. Seven other node
kinds are **shape-stable but tag-unstable**: the parser builds the right fields
and the wrong tag, and name resolution retags once the names inside have bound.
`inode.h` labels `NameUseTag`, `TupleTag` and `StarTag` explicitly as
"parser-ambiguous".

| Built as | Becomes | Retagged in |
| --- | --- | --- |
| `TupleTag` | `TTupleTag` (all types) or `VTupleTag` (all values); mixed is `ErrorBadElems` | `ttupleNameRes` |
| `StarTag` | `PtrTag` if the operand is a type, else `DerefTag` | `ptrNameRes` |
| `ArrayTag` | stays a type, or becomes `ArrayLitTag` | `arrayNameRes` |
| `RefTag` | stays a ref type, or becomes `BorrowTag`/`AllocateTag` by region | `refNameRes` |
| `ArrayRefTag` | stays a ref type, or becomes `ArrayBorrowTag`/`ArrayAllocTag` | `arrayRefNameRes` |
| `QuesTag` | `FnCallTag` for `Option[T]`, or folds into an `AllocateTag` with `FlagQues` | `allocateQuesNameRes` |
| `FnCallTag` | `ArrIndexTag`, `FldAccessTag`, `TypeLitTag`, an instantiation, or a real call | `fnCallTypeCheck` |

This is what principle 1 costs, and it is the whole cost: because a type and a
value parse identically, `*T` and `*p`, `&T` and `&x`, `(A,B)` and `(a,b)` are
one production each, and one retagging pass settles all of them.

Also left undecided: **which method an operator names** — every operator is an
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
plus an `if` chain. `while c {…}` becomes a loop block with `if not c {break
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
read.

This is why `nameUseNameRes`'s unqualified path is a single assignment from
`namesym->node` — see [Name Resolution](name-resolution.md).

**It loads every module.** `import` recursively loads and *fully parses* the
imported module during the parse of the importing one, then adds an `ImportNode`
to a list kept separate from the module's own nodes, so folding can run before
the module's own names resolve. `include` is different: it injects the file and
parses its global statements straight into the **current** module, producing no
node. Corelib is parsed before the main file and `foldall`-imported into every
module.

## 7. Contract

**True when `parsePgm` returns:**

- One `ProgramNode`; every module reachable by import is parsed. No later phase
  reads a source file.
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
| `parseSkipToNextStmt` | the main resync; consumes through the next `;`, or stops short of a `}` or EOF for the enclosing block to handle |
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
`lexInjectFile` on a source file that cannot be found or read (`ExitNF`), and
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
| `parser/lexer.c` | `lexInject`, `lexInjectFile`, `lexPop` | push and pop a source on the lexer chain |
| | `lexNextToken` | the scan dispatch; whitespace, comments, maximal-munch operators |
| | `lexScanIdent` | identifier scan and name-table classification; reserved-word release |
| | `lexScanNumber`, `lexScanString`, `lexScanChar`, `lexScanEscape` | literals; UTF-8 re-encoding of escapes; lifetime-vs-char disambiguation |
| | `lexNewLine`, `lexBlockComment` | line counting for diagnostics, inside comments included |
| `parser/parsemod.c` | `parsePgm` | **entry point** — tables, program, main module, corelib, main file |
| | `parseGlobalStmts` | the global statement dispatch loop |
| | `parseLoadAndParseModuleFile` | per-module unit: de-dup, naming, injection, corelib import, `modHook` |
| | `parseImport`, `parseInclude` | the two source-composition forms |
| `parser/parsehelper.c` | `parseBlockStart`, `parseBlockEnd` | `{` and `}`, with recovery |
| | `parseEndOfStatement`, `parseSkipToNextStmt`, `parseCloseTok` | the required `;`, and the two resyncs |
| `parser/parseexpr.c` | `parseAnyExpr`, `parseSimpleExpr` | the two expression entry points |
| | `parseAssign` … `parseMult`, `parseCast` | the precedence cascade (section 3) |
| | `parsePrefix`, `parseAmper`, `parsePlus` | prefix operators; borrowed and region-managed references |
| | `parseSuffix`, `parseDotCall`, `parseArgs`, `parseArg` | postfix `.`, `()`, `[]`, `++`, `--`; named values |
| | `parseTerm`, `parseNameUse`, `parseArrayLit` | literals, parens, blocks-as-expressions, qualified names |
| `parser/parsetype.c` | `parseType` | the type dispatcher that delegates to `parsePrefix` — principle 1 |
| | `parseStruct` | struct/trait/union: generics, `extends`, fields, methods, macros (a method when parameter 0 is `self`), tag-field synthesis and the `IsTagField` mark on a base trait's discriminant |
| | `parseFnSig` | parameters, `Self` inference, single or tuple return type |
| | `parseVarDcl`, `parseFieldDcl`, `parseConstDcl`, `parsePerm` | the declaration forms; a field's trailing `use` clause goes to `parseFoldClause`, and `use` anywhere else is `ErrorBadFold` |
| | `parseFoldClause` | `use *` with an optional `but` list, or a list of names each with an optional `as`; builds the clause on the field and an alias per listed name, bound by name resolution |
| `parser/parsefnflow.c` | `parseFn` | function/method declaration — **despite the file name, this is where declarations and control flow are parsed, not data flow analysis** |
| | `parseGenericParms`, `parseMacro` | the type parameter list, shared by `fn`, `struct` and `macro`: comma-separated names only, with a constraint or a parameter type refused as `ErrorGenParmConstr` |
| | `parseExprBlock` | the statement-block loop — the parser's second dispatch table |
| | `parseIf`, `parseMatch`, `parseBoundMatch` | `if`/`elif`/`else` and the `match`-to-`if` desugaring |
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

**Four tokens are lexed and never consumed.** This is read off the source and
**unverified** — it needs a probe before it is trusted, and it is the one claim
in this note that does.

## 12. What lives elsewhere

| Question | Note |
| --- | --- |
| What binds the names the parser left unbound | [Name Resolution](name-resolution.md) |
| Lookup, visibility, imports, aliases | [Names and Namespaces](../phases/names-and-namespaces.md) |
| Node header, tags, sentinels, injection hazards | [IR Nodes](../nodes/_index.md) |
