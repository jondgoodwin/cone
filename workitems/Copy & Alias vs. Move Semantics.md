
- ‘Copy’ method: use w/ allocated references, assignment, fn call, etc.
- .clone for custom copying/move

#### Move semantics

- Auto-drop/de-alias on moved variable:

- Returned move values should be considered moved (don’t de-alias)
- Don’t block-dealias a moved variable
- Drop: _ = Add support for ‘_’ as a valid lval (test that move fails on lex). It may not be used as an rval nor as a declared variable (name resolution step).

- Conditional moves (when moves are inside conditional branch):

- Enrich stack, set up multiple flags that are pushed/popped
- If conditional, generate runtime flag and conditional drop logic
- Conditional move of stack values?
- break/continue

- Review move page in documentation