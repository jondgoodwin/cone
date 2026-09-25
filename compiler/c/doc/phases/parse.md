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
   never needs to be told what the parser is doing. The one place line ends and
   indentation mean something is *inside* one token, a multi-line string
   literal (section 2), which reads them to build the literal's content and
   never to decide where a token or a statement ends.
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

**One `Lexer` per source, on a linked list.** `lexPush` pushes, `lexPop`
restores. **Blocks are never recycled**, deliberately: every IR node stores the
`Lexer` current when it was built and reads `url` from it whenever a diagnostic
is reported, so reusing a popped block rewrites the file name out from under
every node still pointing at it. A block can also exist before it is current:
`lexLoadPath` reads a file into one without pushing it, so a module can take
its designated file's first line as its position before that file is parsed,
and the folder sweep can read a file's first statement to decide which module
it is, and `lexPush` makes the same block current later — see
`compiler/c/doc/nodes/module.md`, "Parse".

**An identifier may be spelled in any letters UTF-8 can carry**, which is
`utf8IsLetter`: ASCII letters, or the start of a well-formed multi-byte
character. **Well-formed is the load-bearing word.** A byte is only a character
if it leads a sequence whose continuation bytes are actually there, so a stray
byte is refused as a token (`ErrorBadTok`) rather than absorbed into a name, and
the scan resumes at the byte after it rather than at the length its lead byte
claimed. `utf8ByteSkip` never advances past the character in front of it, which
is what keeps a malformed byte from consuming the source that follows.
`lexical_reject_tokens` holds both shapes.

**Only NUL ends the source.** U+001A, the DOS end-of-file mark, used to end it
too, but only between tokens: a string, a character literal and a block comment
ran straight through one. Jon dropped it on 24 September 2026, so it is now a
control character like any other. Between tokens `lexNextTokenx` passes over it
as it does a space, so a file ending in one still compiles and code after one
is read as code; a line comment, a dropped `#` word and a back-ticked identifier
no longer stop at one; and inside a literal it is refused, as every raw control
character but the tab is (below). `utf8ByteSkip` no longer calls it the end
either. `lexical_ctrlz` compiles and runs code after one, ending in one;
`lexical_reject_ctrlz` reports an error written after one.

**An integer literal is 64 bits wide at most.** `lexScanNumber` accumulates
into a `uint64_t` and refuses a digit that would carry past it
(`ErrorLitOverflow`), once per literal and after every digit and the suffix
have been consumed, so the token still ends where it should and the parser
carries on with it. The digits of a float are exempt: they are read again by
`lexToFloat`, so a mantissa wider than 64 bits is a value, not an overflow.
`lexical_reject_overflow` holds the boundary in both bases.

**A string literal whose opening quote ends its line is a multi-line string
literal**, read by the rules of `doc/reference/reftoken.html`, "Multi-line String
Literals". `lexScanString` finds the closing quote first, stepping over each
escape sequence whole, and takes its indentation — the spaces and tabs before it
on its line — as the literal's margin. `lexStringMargin` reads the start of
every content line against it, by Jon's ruling of 24 September 2026, the Swift
and C# rule: a line of nothing but spaces and tabs is an empty line, whatever it
holds, and every other line must begin with exactly the margin — the same
characters in the same order, not just as many — which is stripped. A line that
does not is `ErrorBadTok` on that line, at the first character that differs,
and the message says what the margin is (so many spaces, so many tabs, or both
in order); it is then read as though it began with as much of the margin as it
has white space for. The end of line after the opening quote is dropped; every
other one, LF or CRLF, becomes one `\n` in the content, unless a backslash
precedes it, which joins the line to the next, itself held to the margin. Tabs
past the margin are content. A closing quote with anything but spaces or tabs
before it on its line is `ErrorBadTok`, reported at the opening quote, and the
margin is then empty. This replaced stripping whatever indentation a line had,
up to the closing quote's count with a tab counted as one. `lexical_mlstring`
and `lexical_mlstring_crlf` hold the rules, `lexical_mlstring_margin` the
margin; `lexical_reject_mlstring` and `lexical_reject_mlstring_margin` the
refusals. A literal that spans lines
without its opening quote ending one is read as before: its line ends and the
spaces and tabs that begin each next line are dropped.

**A raw tab in a literal is content; every other raw control character is
refused.** Jon's ruling of 24 September 2026: a tab written as itself is part
of a one-line string, a multi-line string or a character literal alike, and
any other control character — 0x01 to 0x1F but the tab and the line ends, and
0x7F, Ctrl-Z included — is `ErrorBadTok`, since it cannot be seen in an editor.
The message is reported at the character itself, names it by value
(`lexCharDescribe`, the description the unprintable-escape message uses too),
and gives the `\x` escape that writes it: "A string literal cannot hold the
control character 0x01 raw, where it cannot be seen: write it as \x01". The
string leaves it out; the character literal takes it as its value, so the
literal still closes. Before, a one-line string dropped every control
character silently, a tab included, but kept 0x7F; a character literal took
any as its value. A line end is not in this rule: a string that spans lines
drops it (above), a multi-line string makes it a new-line, and a character
literal refuses it in its own words (below). Dropping the white space after a
line end used to skip further line ends uncounted as well, so a blank line
inside such a string put every later diagnostic a line early; the white space
dropped is now spaces and tabs only, and each line is counted. `lexical_raw_tab`
runs the tab in each kind; `lexical_reject_raw_control` holds the refusals.

**A string literal is sized before it is built, by the same walk that ends
it.** `lexScanString` allocates the literal as many bytes as the source holds
between its quotes, found by stepping over each escape sequence whole, which is
how the build steps too; and nothing the build reads becomes more bytes than it
takes, so the allocation always holds what is written. When a backslash is the
last character of the source, `lexScanEscape` stops on the source's closing NUL
rather than stepping past it, so neither a string nor a character literal reads
beyond the source. `lexical_string_escapes` holds a literal beginning with each
escape kind, and runs of escaped quotes, each printed whole.

**A string literal that is never closed is reported at its opening quote.**
When the sizing walk reaches the source's end without finding a closing quote,
`lexScanString` reports `ErrorBadTok` there and then, before the build counts
the lines the literal runs over, so the position is the opening quote's line
whether the literal is ordinary or multi-line. The literal is still built from
what the source holds, and the parser, finding the end of the file after it,
reports what the unfinished statement lacks as well: those are follow-ons, and
they come after. `lexical_reject_unclosed_string` and
`lexical_reject_unclosed_mlstring` hold one literal each, since a literal that
runs to the end of the file is one per file.

**A character literal or back-ticked identifier cut off by the source's end
stays on it.** A character literal missing its closing quote is `ErrorBadTok`,
"Invalid lifetime or too-long character literal", at the opening quote, and its
scan stops at the end of its line. When the opening quote is the last character
of the source, `lexScanChar` takes the value 0 and stays on the source's closing
NUL instead of stepping past it. A back-ticked identifier missing its closing
backtick is `ErrorBadTok` at the backtick; `lexScanTickedIdent` recovers by
taking the next character as the name and the one after as the missing
backtick, unless the source ends before both, when the name is what there is
and the scan stays on the source's end. Either way the parser then reports what
the unfinished statement lacks at the end, as a follow-on.
`lexical_reject_unclosed_char` and `lexical_reject_unclosed_backtick` hold one
each. A file cannot end in the quote or backtick and still carry the
annotation after it, so each source is ended by a null character in that
place, which ends a source as the end of the file does and is the byte the
compiler puts after every file it reads; the annotations follow it.

**Nor does either take a line's end.** A raw line end right after a character
literal's opening quote — a new-line, a CRLF, or a carriage return alone — is
`ErrorBadTok` at the quote, naming `'\n'` (or `'\r'`) as the spelling: Jon's
ruling of 24 September 2026, the rule of C, Go, Rust and Swift. `lexScanChar`
ends the literal there and stays on the line end, so it is counted where any
other is. `'<LF>'` compiled as the value 10 before, and the line went
uncounted, so every later diagnostic came out a line early. An unclosed
back-ticked identifier's recovery, one character as the name and the next as
the missing backtick, takes neither from past the line's end: the name is what
there is before it, and the scan stays on it. `lexical_reject_char_line_end`
holds each, LF, CRLF and a lone carriage return, with a later diagnostic
pinning the line after each.

**`\0` is the null character and nothing more.** `lexScanEscape` reads the
digit `0` after a backslash as U+0000: a 0 byte in a string literal, the value
0 in a character literal. The source's own closing NUL after a backslash is a
different case, the end of the source, on which the reader stays. Cone has no
octal escapes, and a decimal digit right after `\0` is `ErrorBadTok` at the
literal's opening quote (Jon's ruling of 24 September 2026, JavaScript strict
mode's rule), so a C programmer's `"\012"` fails loudly instead of meaning a 0
byte, `1` and `2`. The message names `\x00` then the digit as the spelling of
that; `\x` takes exactly two hex digits, so `"\x001"` is a 0 byte then `1`. The
digit is left to be read as content. `lexical_escape_null` holds both kinds of
literal; `lexical_reject_octal` the refusal.

**A hex escape cut short says so.** `\x`, `\u` and `\U` take exactly 2, 4 and
8 hexadecimal digits (`lexHexDigits`). Where a character that is not a digit
comes first, what is reported depends on the character. One that could have
been meant as a digit is named: "Invalid hexadecimal character 'Z'". One that
ends the escape short — the source's end, a line's end, a space, a quote, any
other control character — is not, since it would print as nothing, as a raw
control byte or as a bare quote; the message names the escape so far instead,
"Escape sequence '\u12' is too short: '\u' takes 4 hexadecimal digits". Both are
`ErrorBadTok` at the literal's opening quote. `lexical_reject_short_escape` holds a closing quote, a space, a tab and a
line's end after one; `lexical_reject_short_escape_end` the source's end, ended
by a null character as the unclosed character literal's file is.

**A backslash before a character that begins no escape names it only if it
prints.** `lexScanEscape` reports `ErrorBadTok` at the literal's opening quote,
"Invalid escape sequence 'q'", naming the character when it is printable. A
tab, a line's end, a control character or a byte that begins no UTF-8
character is described instead, the last two by value: "Invalid escape
sequence: a backslash followed by the control character 0x01". Printed raw, a
control byte would land in the message and a line's end would split it across
two lines. `lexCharIsNameable` is the test, shared with the hex escape message
above, whose ender is likewise named only if it prints; a byte that is not
UTF-8 after too few hex digits is therefore "too short" as well. A space after
a backslash is itself an escape, and the reader stays on the source's end, as
the `\0` paragraph says, so neither reaches the message.
`lexical_reject_bad_escape` holds each kind in a character literal or a string.

**A backslash before a line's end counts the line.** Outside a multi-line
string, where it is the line join above, a backslash before a line's end is the
invalid escape just described, and it takes an LF line end as its character.
It used not to count that line, so every later diagnostic in the file came out
one line early. `lexScanEscape` now counts it and moves the line start past it,
as `lexNewLine` does. A CRLF line end was always right: the escape takes only
its carriage return, and the new-line is counted where any other is. What is
accepted and refused is unchanged, and so is every diagnostic but the line of
those that follow. A character literal left too long by the escape is still
reported at its opening quote, on the line it begins, which `lexScanChar`
remembers across the escape. `lexical_reject_escape_newline` and its CRLF twin
pin a later error's line after each, in a string and in a character literal.

**Names are interned at scan time, and `Name.node` is the binding slot.**
`nametblFind` returns one immovable `Name*` per unique string. That same
`node` field is what makes classification O(1) in the scanner: `keywordInit`
binds each keyword's `Name.node` to a `KeywordTag` node whose `flags` carry the
token type. Permissions reach the same effect by a different route:
`stdPermInit` binds each permission name's `node` to the `PermNode` itself, and
`lexScanIdent` has a separate branch turning a `PermTag` binding into a
`PermToken`. So `mut` and `uni` are lexically distinguished without being
keyword tokens — copy the right one of these two patterns if you add a third
family. To the language they are reserved words all the same: the manual lists
all six static permissions with the keywords (Jon's ruling of 24 September
2026), since a name the lexer always reads as a permission can never be used. A reserved word is reported once,
at first use, and then **released** — `Name.node` is cleared and the word
continues as an ordinary identifier, so the rest of the compile is not derailed
by it.

**Every token the lexer returns has a reader.** A spelling no feature reads is
reported in the lexer and never reaches the parser, because a token nothing
consumes is a cascade of parse errors behind it. Three are refused this way, each
with one diagnostic. An attribute is a keyword (`@move`, `@opaque`, `@unsized`, `@c`, `@initpure`),
so any other `@` word is `ErrorUnkAttr` and is dropped, the declaration read
without it; `@samesize` gets its own wording, since an enum is same-size by
default and `@unsized` declines it. A `#` word is held for metaprogramming:
`ErrorReserved`, and it is dropped with the rest of its line, so what `#if`
was followed by is not reported again. `?.` is held for None propagation:
`ErrorReserved`, and it is read as `.`. `lexScanIdent` returns 0 for a dropped
word and `lexNextToken` scans on from where it stopped.
`lexical_reject_unbuilt` holds all three.

**A word held for an unimplemented feature is reserved; a word that names an
unbuilt *kind of declaration* is a token.** `mod` and `actor` are the two kinds
that carry abstractions, so both are ordinary keywords with an arm of their own in
the global dispatch. `mod` builds a declaration — it declares the module a
folder's files, or one file, belong to, [module](../nodes/module.md), "The `mod`
declaration" — and so does `mod trait`, the module's abstraction, which the arm
tells apart by the word after `mod` and hands to `parseModTrait`
([module](../nodes/module.md), "Module traits").
`actor`
does not, and its arm reports `ErrorUnbuiltKind` where the declaration is written
and names the abstraction's spelling, `actor trait`. An in-file
`mod name { ... }` block is refused under that code too, though it is no unbuilt
shape: it does not exist, since a nested module is a file of its own or a
subfolder.
Admitting a shape is what settles its spelling now instead of leaving it to be
designed when the semantics land, and reporting it is what keeps a declaration
from being accepted with nothing under it. The cost is the standing one in the
hazards: holding a word takes it away from every program.

**A retired statement's word is a token for the same reason.** `include` is
retired — the folder brings a module's files in — and it stays a keyword with an
arm of its own that reports `ErrorInclude` and skips the statement. Released to an
identifier as a reserved word is, `include name;` would be two identifiers at
global scope and a cascade behind the one diagnostic that matters.

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
any module's names resolve. `include` is retired: it stays a keyword, and the
statement it begins is `ErrorInclude` at the word and skipped to its `;` without
the file it names being looked for, because the folder is what brings a module's
files in (`parseRetiredInclude`).
A path is looked for beside the importing file and then on the package search
path, which ends at the packages folder holding `core` and `stdio`. `core`, the
prelude, is loaded from there before the main file is parsed and
wildcard-imported into every module ([Module](../nodes/module.md), "The packages
folder").

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
organisational, its files joining the enclosing module at any depth. **A file
gets a probe too**: one whose first statement is `mod` is a one-file submodule
rather than one of the module's files, which the sweep reads off the file's text
before anything is parsed (`lexOpensWithMod`). **Every
submodule of a level is drawn before any of them is parsed**, so a sister is a
name of the parent's namespace, and her files are registered, before any file can
name her; and all of them are drawn before the parent's own files are parsed, so
a name a submodule put in the namespace is there before any statement can collide
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
| an unbuilt form's body | `parseSkipDclBody` skips a following `{ … }` whole, counting depth, so nothing inside is read as a global statement and reported again, and resyncs at the next `;` where there is no block. `actor` and the refused in-file `mod name { … }` block use it, and so does a member a module trait refuses (`parseModTraitSkipMember`), so each is reported once, where it is written |
| the retired `include` | `parseRetiredInclude` reports `ErrorInclude` at the word, then reads what the statement took — names or quoted paths, comma-separated — and its `;`, so a statement naming one file, a path or a list is one diagnostic and a `pub` before it adds none. A missing `;` ends the statement at its last name rather than swallowing the next declaration; only where no name follows does it resync with `parseSkipToNextStmt` |
| `parseCloseTok` | reports `ErrorNoRParen`, scans for the closer, gives up at `;`, `}`, EOF |
| `parseBlockStart` | on `:`, reports `ErrorColonBlock` and reads what follows as the block; on anything else that is not `{`, reports `ErrorNoLCurly` and scans forward for one |
| `parseTerm` default | reports `ErrorBadTerm`, consumes one token to avoid an infinite loop, returns `NULL` |
| anonymous placeholders | the declaration parsers substitute `anonName` so the caller always gets a node |
| `parseModuleFilesParse`'s end-of-file test | `ErrorNoEof` when a module file's global statements stopped short of EOF — the file the compiler was given or one the folder swept in; a stray `}` ends `parseGlobalStmts`, and this is the one place the parser refuses to finish quietly |

The rationale is written into `parseStruct`: an unnamed type is built under the
anonymous name rather than abandoned, so the body is still parsed — leaving
would drop the whole body on the floor and turn its opening brace into the next
global statement.

**Two conditions abort the process outright**, with no recovery:
a source file `fileFindSrc` cannot find, or `lexInjectPath` cannot read (`ExitNF`) —
a build description, or a file it lists or an import line names, included — and
`parseFilename` when `import` is followed by something that is
neither an identifier nor a string (`ExitNF` as well, despite being a malformed
token rather than a missing file).

**A build description with an error in it stops the compile before any source
is parsed**: `conec.c` reads it ahead of `parsePgm` and ends with the error
summary where `parseBuildDesc` reported anything. Its own recovery is a line at a
time — a malformed line is reported once and passed over up to the next token
that can begin one.

Diagnostics come from `errorMsgLex` (position from the lexer — the parser's
workhorse), `errorMsgNode` (position from a node, plus the instantiation trace),
and `errorMsg` (no source context). The parser owns roughly two dozen
`ErrorCode`s; `shared/error.h` is the list, and `test/codes.toml` pins the
numbers.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `parser/lexer.c` | `lexInjectPath`, `lexPop`; `lexLoadPath`, `lexPush` | push and pop a source on the lexer chain. `lexInjectPath` reads an already-located file: locating one is the caller's, since the path is what the file registry is keyed by. `lexLoadPath` and `lexPush` are its two halves apart — read a file into a block that is not yet current, and later make that block current |
| | `lexNextToken` | the scan dispatch; whitespace, comments, maximal-munch operators |
| | `lexScanIdent` | identifier scan and name-table classification; reserved-word release; a `@` or `#` word that names nothing reported and dropped |
| | `lexScanNumber`, `lexScanString`, `lexScanChar`, `lexScanEscape` | literals; UTF-8 re-encoding of escapes; lifetime-vs-char disambiguation |
| | `lexNewLine`, `lexBlockComment` | line counting for diagnostics, inside comments included |
| | `lexOpensWithMod` | whether a source's first statement begins `mod` or `pub mod`, and not `mod trait`, read off its text past white space and comments with nothing lexed: the folder sweep's probe for a one-file module |
| `parser/parsemod.c` | `parseInit`, `parsePgm`, `parseLoadCore` | **entry point** — `parseInit` sets up the name table and the lexer, ahead of generation's setup since a build description is read with them; `parsePgm` the type tables, program, main module (a source file's, or the one a build description names), the `core` package from the search path, main file |
| `parser/parsebuild.c` | `parseIsBuildDesc`, `parseBuildDesc`, `parseBuildFindImport`, `parseBuildImportModule` | the build description: told apart by its `.conebuild` extension, read by the lexer into a tree of `BuildModule`s — settings, the package lines, each module's files, child modules and import lines, each malformed line `ErrorBuildDesc` — the import line a described module writes for a name, and the entry for an include file an import line loads, whose imports are the package lines |
| `parser/parsemod.c` | `parseBuildModuleTree`, `parseBuildSubmoduleDraw`, `parseBuildFiles`, `parseLoadBuildImport` | a described build's module tree: each module named and filled as the description says, nothing swept, and the file an import line names loaded as a declared module under the import's name. `ParseState.build` is the current module's entry, which `parseModuleDcl` checks the `mod` line against (`ErrorBuildModName`) and `parseImport` answers names from (`ErrorBuildImport`) |
| | `parseGlobalStmts` | the global statement dispatch loop; `trait` by itself enters `parseStruct` with `TraitType` already set, `mod trait` enters `parseModTrait`, and `actor` is the unbuilt kind refused here. It is told whether it is reading the start of a module's first file or of another of its files, which is what decides where a `mod` declaration may stand, which ones a build description checks, and which file must open with one (`ErrorNoModDcl`). It also refuses an `import` after the file's first other declaration (`ErrorImportLate`): the header is the `mod` line, then the imports |
| | `parseModuleDcl` | `mod name;`, the declaration a module's designated file or one file makes: the placement rule, the check against the folder's name, the one-file submodule's file's or the build description's, the rename a lone file still gets, the module's own name bound into its namespace, what `pub` does for a submodule and why it is refused on any other module, the `@c` after `mod` that makes the module C-named (its `DclCName`, `DclSystemCC` and prefix, given only where the declaration is accepted), the `extends` and the `is` it records for name resolution to resolve, in the order `extends`, `is`, `use` (`ErrorModIs`, `ErrorBadFold` otherwise), and the refusal of the in-file block, which does not exist |
| | `parseModTrait` | `mod trait Name { ... }`, a module trait: a function or a global per member, a requirement without a body or initialiser and a default with one; anything else, a generic fn and an overload name `ErrorModTraitBody`, skipped whole (`parseModTraitSkipMember`); no body makes a marker |
| | `parseCAttr` | `@c`, `@c("str")`, `@c(system)`, `@c("str", system)` after `mod` or `fn`, written onto a `DclInfo`: the string is a module's prefix or a function's whole symbol. A malformed one is `ErrorCAttr` and dropped whole |
| | `parseFnOrVar`, `parseExternFnCheck` | a module's `fn` or global, with `extern` (single or block) meaning only "defined elsewhere": a body-less signature, refused on an `inline` or generic fn (`ErrorBadExtern`). A bare `@c` on a fn its C-named module already names is `ErrorCNameTwice`. The retired `extern system` is `ErrorCAttr`, naming `@c(system)` |
| | `parseSkipDclBody` | skip an unbuilt form's `{ … }` whole, or resync at the next `;` |
| | `parseLoadAndParseModuleFile`, `parseLoadModulePath` | per-module unit: locate beside the importer and then on the search path, `FlagGenMod` for what the search path found, register by path, naming, the folder sweep, the `core` import, `modHook`, and a parse per file |
| | `parseDesignatedFolder`, `parseCollectFolder`, `parseModuleFiles` | the folder sweep: whether the file is its folder's designated file, which files the folder brings in, each read into its block as it is found, which files are one-file modules and which subfolders draw submodules, in the order of their names, and the refusals — a designated file or one-file module too deep to be a module, a one-file module beside a module folder of its name |
| | `parseSubmoduleDraw`, `parseSubmoduleParse`, `parseModuleTree` | the module tree: the submodule a subfolder or a one-file module draws — owned by its parent, bound in its namespace, private to it unless `pub`, `FlagGenMod` exactly as its parent — and the order, submodules before the module's own files |
| | `parseRegisterModuleFiles` | the file registry entries for a module's files, and the two collisions that stop a file joining |
| `shared/fileio.c` | `fileFindSrc`, `fileFindLocal`, `fileFindPackage`, `fileFolderScan`, `fileDesignatedFile` | locate a source file without reading it — beside a file, on the package search path, or the one then the other; list a folder's `.cone` files and subfolders, sorted; probe a folder for the designated file that makes it a module folder |
| | `parseImport`, `parseRetiredInclude` | the one source-composition form, and the retired one reported |
| `parser/parsehelper.c` | `parseBlockStart`, `parseBlockEnd` | `{` and `}`, with recovery |
| | `parseEndOfStatement`, `parseSkipToNextStmt`, `parseCloseTok` | the required `;`, and the two resyncs |
| `parser/parseexpr.c` | `parseAnyExpr`, `parseSimpleExpr` | the two expression entry points |
| | `parseAssign` … `parseMult`, `parseCast` | the precedence cascade (section 3) |
| | `parsePrefix`, `parseAmper`, `parsePlus` | prefix operators; borrowed and region-managed references |
| | `parseSuffix`, `parseDotCall`, `parseArgs`, `parseArg` | postfix `.`, `()`, `[]`, `++`, `--`; named values. The `.` production serves a member of a value and a path through a namespace alike |
| | `parseTerm`, `parseNameUse`, `parseArrayLit` | literals, parens, blocks-as-expressions, names |
| `parser/parsetype.c` | `parseType` | the type dispatcher that delegates to `parsePrefix` — principle 1 |
| | `parseStruct` | struct/trait/enum: the optional `trait` modifier on the kind, generics, the base clauses — the `is` list and the `extends` base, read in a loop so that a type may write both, each once, with `extends` refused on a trait and meaning *the enum whose variants join this one's set* on an enum — fields, methods, macros (a method when parameter 0 is `self`), `extern fn` methods and functions with no body (refused in a trait or a generic type, and before anything but `fn`: `ErrorBadExtern`), an enum's variants in both spellings, tag-field synthesis and the `IsTagField` mark on an enum's discriminant; `@c` on a type is `ErrorCAttr` |
| | `parseAddVariant`, `parseVariantTagPin` | joining a variant to its enum: the closed flags, the synthesized base link, the tag value written or assigned in sequence, and its name, bound in the enum's namespace while the node joins the module's list. A variant of an enum that *extends* another keeps the unassigned sentinel unless a value was written, because that enum's numbering continues from its base's last and the base is not resolved yet |
| | `parseEnumExtensionMember` | what an enum extending another may not declare — a member of any kind, since a field or method every variant carries would have to reach its copies of the base's variants too, and a requirement declared there would need every copy to implement it; such a member belongs on the base, and comes along with the copies. Nor a discriminant: none is synthesized for such an enum either, its base's arriving with the fields name resolution splices in |
| | `parseIsTagType`, `parseTagType` | `tag` recognized where a field's type is written and nowhere else, so it is not a reserved word |
| | `parseFnSig` | parameters, `Self` inference, single or tuple return type |
| | `parseVarDcl`, `parseFieldDclBody`, `parseConstDcl`, `parsePerm` | the declaration forms; a field's or a global's trailing `use` clause goes to `parseFoldClause`, and one on a local, a parameter or a static is `ErrorBadFold`. A field's node is built while the lexer is still on its name, both so a diagnostic points there and so an enum's body can decide between a field and a bare-name variant afterwards |
| | `parseFoldClause` | `use *` with an optional `but` list, or a list of names each with an optional `as`; builds the clause on the field and an alias per listed name, bound by name resolution |
| | `parseUseSibling`, `parseModUse`, `parseUseAdmits` | a `use` standing as a statement: in a type body it folds a sibling in, and at module scope (`parseGlobalStmts`) it folds an enum's variants or a submodule's names in, held on a `ModUseNode` — which of the two is known only once the source resolves. Both name their source and then share what follows it — every member by default, `*` refused as saying nothing more, a list with `as`, a block, or `but` |
| `parser/parsefnflow.c` | `parseFn` | function/method declaration, with its `@initpure` after `fn`, before or after `@c`, recorded as `DclInitPure` and not checked, and its `@c`, refused where there is no one symbol for it to name — an anonymous, generic or `inline` fn, a trait's or a generic type's method (`ErrorCAttr`) — **despite the file name, this is where declarations and control flow are parsed, not data flow analysis** |
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
| Lookup, visibility, imports, aliases | [Names and Namespaces](../../../../doc/design/names-and-namespaces.md) |
| Node header, tags, sentinels, injection hazards | [IR Nodes](../nodes/_index.md) |
