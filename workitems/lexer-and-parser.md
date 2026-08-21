Free up `:` for other uses: type spec and ?

Fix () and [] when used as postfix operators
- Switch generics and macros to ()
- Add parameters to function/index calls (!)
- Giving these distinct node shapes is half of what shrinks
  `fnCallTypeCheck`: one `FnCallNode` shape currently serves macro calls,
  `<-` on a tuple, generic substitution, overload callees, type
  constructors, initializers, the bare-name-to-`self.method` rewrite, and
  ordinary calls, so the front end re-derives at type check what the source
  already said. The other half is resolving the callee first; see
  [[tag-group-and-name-aliasing-refactor|Tag Group and Name Aliasing Refactor]].

Lexer:
- Raw string literals
- UTF8:
- UTF8 support for IsLetter. **Now with a reproducer.** `utf8IsLetter` is
  `utf8IsMultibyte(srcp) || isalpha(*srcp)` and `utf8IsMultibyte` is `*src &
  0x80`, so *every* byte at or above 0x80 is accepted as an identifier start --
  malformed ones included. A line reading `a`, an invalid lead byte, and `a` is
  lexed as one identifier: `utf8ByteSkip` believes the bad byte is four long, so
  the scan swallows the space, the letter and the newline, and line tracking
  goes with it. Nothing multi-byte therefore reaches the bad-character path at
  all, which is why the message that path prints was fixable without being
  observable from any well-formed source. Found by [[bugs|Bugs]].
- Bad token will not correctly print bad utf8 character code -- fixed; see
  [[bugs|Bugs]]
- Error message line pointer will not correctly handle multi-byte characters --
  fixed; see [[bugs|Bugs]]

Number literal inference
- Figure out the design for how it works in lexer & with method call selection
- Remove old literal conversion approach
- Fix lexer handling of number literals ‘1:i64’
- Number literal nodes can take an expected type (and default otherwise)
	- Blocked by call ordering, not by the lexer: `fnCallTypeCheck` checks
	  arguments before it resolves the callee, so no parameter type exists to
	  hand down. See [[type-inference-and-coercion|Type Inference and Coercion]] and [[tag-group-and-name-aliasing-refactor|Tag Group and Name Aliasing Refactor]].
- Move number conversion logic to “into” node and alter gen logic accordingly
- Document up number coercion/conversion in Cone doc.
- Document future number capabilities in this TODO doc
## Four tokens are lexed and never consumed

`MetaIdentToken`, `AttrIdentToken`, `QuesDotToken` and `RegionToken` are
produced by the lexer and referenced by no parser function, so they presumably
fall through to `ErrorBadTerm`/`ErrorBadGloStmt`. **Read from source, unverified.**

This is here rather than in [[bugs|Bugs]] because the repair is a decision: either
give them syntax or stop lexing them. A token that only ever produces "bad term"
is a worse diagnostic than an unknown character, so doing nothing is not
neutral. *Settle what it does today:* compile a source containing `#if`,
`@samesize`, `a?.b` and `region R`.

Three further parser observations from the same reading — unbalanced paren
counting, `parseAdd` guarding `-` against a statement break but not `+`, and a
stray `}` at global scope — were confirmed and fixed; see [[bugs|Bugs]] for what each
turned out to be. The two dead parameters (`parsePrefix`'s `noSuffix`,
`parseArrayLit`'s `typenode`) are gone with them.

## `parseCloseTok` leaks one from the open-delimiter count when it gives up

Found while fixing the paren counting. On an unterminated bracket
`parseCloseTok` reports and returns *before* `lexDecrParens`, so the count stays
one too high for the rest of the file and `lexIsStmtBreak` stops firing where it
should.

Not repaired with the counting fix, and not in [[bugs|Bugs]], because it is not clear
the recovery *should* decrement: the construct never closed, so what the count
means afterwards is a question about what recovery is trying to salvage rather
than an off-by-one. Unobservable in the corpus today — `core-parse-unclosed` and
`closure-parse-sig` both recover by consuming to end of file, so nothing follows
that the leak could affect.
