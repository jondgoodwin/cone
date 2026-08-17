
- ‘Copy’ method: use w/ allocated references, assignment, fn call, etc.
	- **`WarnCopy` (3003) is already declared for this and is raised nowhere**,
	  deliberately kept by [[Unenforced language rules]] so the intent lives where
	  an implementer will look — which is here. It names an unsafe copy of a
	  `CopyMethod` or `CopyMove` typed value, and **neither name exists anywhere in
	  the tree**: the only occurrence of either is the comment beside the code in
	  `shared/error.h`. So this is not a missing check against existing machinery —
	  the machinery is absent too, and building it is this item. `python
	  test/run.py --coverage` lists the code under "raised by nothing" and is right
	  to; it is the only entry there, so anything joining it is a red flag rather
	  than noise.
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