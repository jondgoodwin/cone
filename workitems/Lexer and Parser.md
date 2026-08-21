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
  [[Namedef Refactor]].

Lexer:
- Raw string literals
- UTF8:
- UTF8 support for IsLetter
- Bad token will not correctly print bad utf8 character code
- Error message line pointer will not correctly handle multi-byte characters

Number literal inference
- Figure out the design for how it works in lexer & with method call selection
- Remove old literal conversion approach
- Fix lexer handling of number literals ‘1:i64’
- Number literal nodes can take an expected type (and default otherwise)
	- Blocked by call ordering, not by the lexer: `fnCallTypeCheck` checks
	  arguments before it resolves the callee, so no parameter type exists to
	  hand down. See [[Type Inference and Coercion]] and [[Namedef Refactor]].
- Move number conversion logic to “into” node and alter gen logic accordingly
- Document up number coercion/conversion in Cone doc.
- Document future number capabilities in this TODO doc
## Four tokens are lexed and never consumed

`MetaIdentToken`, `AttrIdentToken`, `QuesDotToken` and `RegionToken` are
produced by the lexer and referenced by no parser function, so they presumably
fall through to `ErrorBadTerm`/`ErrorBadGloStmt`. **Read from source, unverified.**

This is here rather than in [[Bugs]] because the repair is a decision: either
give them syntax or stop lexing them. A token that only ever produces "bad term"
is a worse diagnostic than an unknown character, so doing nothing is not
neutral. *Settle what it does today:* compile a source containing `#if`,
`@samesize`, `a?.b` and `region R`.

Three further parser observations from the same reading — unbalanced paren
counting, `parseAdd` guarding `-` against a statement break but not `+`, and a
stray `}` at global scope driving the block stack negative — are in [[Bugs]],
all unverified.

Two dead parameters (`parsePrefix`'s `noSuffix`, `parseArrayLit`'s `typenode`)
are cleanup rather than defects and are recorded in [[Bugs]] under the same
caveat.
