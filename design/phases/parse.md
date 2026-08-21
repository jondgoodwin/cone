Parsing turns source text into IR nodes. It is the only phase that reads files,
and it finishes with every module in the program loaded.

Read this before changing syntax, before adding a node the parser must build,
or when trying to work out whether a problem is the parser's or name
resolution's — the boundary is not where most readers expect it.

*Provenance: read from source; the precedence cascade, `parseType`'s delegation
to `parsePrefix`, and parse-time namespace population were traced end to end.
Section 11 is what is unverified. See [Measuring](../diagnostics/measuring.md).*

## 1. Key principles

1. **One grammar for types and values.** `parseType` dispatches every
   type-starting token to `parsePrefix` — the *value* expression parser. There
   is no separate type grammar and no backtracking.
2. **The parser drives the lexer's block mode.** Indentation is not tokenized.
   The parser tells the lexer when a block starts and what kind it is, and asks
   whether it has ended.
3. **The parser desugars.** `match`, `each`, `while`, `with`, bound patterns and
   several prefix forms are lowered here, into blocks and `if` chains.
4. **The parser binds module-level names.** Module namespaces are populated,
   hooked into the global name table, and checked for duplicates *during*
   parsing, not during name resolution.

Principles 3 and 4 are the two most readers get wrong.

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

Cone has brace blocks *and* significant-indentation blocks, mixable **per
block**, not per file. The lexer emits no INDENT/DEDENT tokens. Instead it keeps
a block-mode stack the parser drives:

| Mode | Started by | Ends when |
| --- | --- | --- |
| `FreeFormBlock` | `{` | never, by indentation — only `}`, tested by `parseBlockEnd` |
| `SigIndentBlock` | `:` at end of line | a line's first token is indented no deeper than the block |
| `SameStmtBlock` | `:` with statements following on the same line | at the first newline it **converts itself** into `SigIndentBlock` |

That conversion is what makes `if x: a` followed by an indented continuation
work. A token-emitting INDENT/DEDENT design could not do it without the parser
telling the lexer what it is doing, which is the whole argument for this shape.

Statement ends are inferred rather than required. `lexStmtStart` records the
statement's indent; `lexIsStmtBreak` is true when the current token is first on
its line, is not indented past that, and the current block's paren count is
zero. A semicolon is optional. `parseAdd` consults it for `+` and `-` alike, to
decide whether a leading sign continues the previous statement or starts a new
one — both are prefix operators, so guarding only one of them absorbs a line
that begins with the other.

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

Seven node kinds are **shape-stable but tag-unstable**: the parser builds the
right fields and the wrong tag, and name resolution retags once the name binds.
`inode.h` labels the first three explicitly as "parser-ambiguous".

| Built as | Becomes | Retagged in |
| --- | --- | --- |
| `NameUseTag` | `VarNameUseTag`, `TypeNameUseTag`, `MacroNameTag`, `GenVarUseTag` | `nameUseNameRes` |
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
was parsed.

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
flagged `FlagLoopStep`. `with e {…}` becomes a block with a `this` declaration
first. Prefix `.f` becomes `this.f`. `else if` folds into `elif`. Unary minus on
a literal is constant-folded in place.

**It binds module-level names.** `modAddNode`, `modAddNamedNode` and `modAddFn`
run *during* parsing, so by the time a module's parse finishes its namespace is
populated, `ErrorDupName` and `ErrorOverloadClash` have already been reported,
overload sets have their `FnOverloadDclNode`, and linker names are mangled with
the module prefix. The stated reason is that permissions and allocators do not
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
  synthesizes — with `dclnode` already set and the tag already `VarNameUseTag`.
  `nameUseNameRes` returns immediately for these.
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
| `parseSkipToNextStmt` | the main resync; guarantees forward progress, then consumes to `;`, end of line, `}` or EOF |
| `parseCloseTok` | reports `ErrorNoRParen`, scans for the closer, gives up at `;`, `}`, EOF |
| `parseBlockStart` | on a missing `{`/`:`, pretends an indented block started, else scans forward for one |
| `parseTerm` default | reports `ErrorBadTerm`, consumes one token to avoid an infinite loop, returns `NULL` |
| anonymous placeholders | the declaration parsers substitute `anonName` so the caller always gets a node |
| `parsePgm`'s end-of-file test | `ErrorNoEof` when the main file's global statements stopped short of EOF — a stray `}` ends `parseGlobalStmts`, and this is the one place the parser refuses to finish quietly |

The rationale is written into `parseStruct`: an unnamed type is built under the
anonymous name rather than abandoned, so the body is still parsed — leaving
would drop the whole body on the floor and turn its opening brace into the next
global statement.

**Three conditions abort the process outright**, with no recovery:
`lexInjectFile` on a source file that cannot be found or read (`ExitNF`),
`parseFilename` when `import` or `include` is followed by something that is
neither an identifier nor a string (`ExitNF` as well, despite being a malformed
token rather than a missing file), and `lexBlockStart` past 1024 nested blocks
(`ExitIndent`).

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
| | `lexBlockStart`, `lexBlockEnd`, `lexIsBlockEnd` | the parser-driven block-mode stack |
| | `lexStmtStart`, `lexIsStmtBreak`, `lexNewLine` | statement-end inference and indent tracking |
| `parser/parsemod.c` | `parsePgm` | **entry point** — tables, program, main module, corelib, main file |
| | `parseGlobalStmts` | the global statement dispatch loop |
| | `parseLoadAndParseModuleFile` | per-module unit: de-dup, name prefix, injection, corelib import, `modHook` |
| | `parseImport`, `parseInclude` | the two source-composition forms |
| `parser/parsehelper.c` | `parseBlockStart`, `parseBlockEnd` | `{` vs `:` entry and exit, with recovery |
| | `parseEndOfStatement`, `parseSkipToNextStmt`, `parseCloseTok` | statement termination and the two resyncs |
| `parser/parseexpr.c` | `parseAnyExpr`, `parseSimpleExpr` | the two expression entry points |
| | `parseAssign` … `parseMult`, `parseCast` | the precedence cascade (section 3) |
| | `parsePrefix`, `parseAmper`, `parsePlus` | prefix operators; borrowed and region-managed references |
| | `parseSuffix`, `parseDotCall`, `parseArgs`, `parseArg` | postfix `.`, `()`, `[]`, `++`, `--`; named values |
| | `parseTerm`, `parseNameUse`, `parseArrayLit` | literals, parens, blocks-as-expressions, qualified names |
| `parser/parsetype.c` | `parseType` | the type dispatcher that delegates to `parsePrefix` — principle 1 |
| | `parseStruct` | struct/trait/union: generics, `extends`, fields, methods, tag-field synthesis |
| | `parseFnSig` | parameters, `Self` inference, single or tuple return type |
| | `parseVarDcl`, `parseFieldDcl`, `parseConstDcl`, `parsePerm` | the declaration forms |
| `parser/parsefnflow.c` | `parseFn` | function/method declaration — **despite the file name, this is where declarations and control flow are parsed, not data flow analysis** |
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

Of the five things read off the source while writing this note, one is still
**unverified**: four tokens that are lexed and never consumed. It needs a probe
before it is trusted.

The other four were probed and fixed. `parseArrayLit` and `parseFnSig` call
`lexIncrParens` now, so a bracketed construct split across an unindented line
parses and stops decrementing an enclosing paren's count. `parseAdd` guards `+`
as it already guarded `-`. The two dead parameters are gone from `parser.h` and
every call site. And a stray `}` at global scope is `ErrorNoEof` — the symptom
was **not** the block stack going negative: `parsePgm` simply had no end-of-file
check, so the rest of the main file was discarded in silence, exit 0, object
emitted.

## 12. What lives elsewhere

| Question | Note |
| --- | --- |
| What binds the names the parser left unbound | [Name Resolution](name-resolution.md) |
| Lookup, visibility, imports, aliases | [Names and Namespaces](../phases/names-and-namespaces.md) |
| Node header, tags, sentinels, injection hazards | [IR Nodes](../nodes/_index.md) |
