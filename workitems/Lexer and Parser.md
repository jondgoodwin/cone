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