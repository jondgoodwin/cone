
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
## Where a destructuring's alias adjustment attaches

Returning owning references in a tuple is a measured use-after-free followed by
a double free — `fn pair() +rc-mut i32, +rc-mut i32` frees both before the
return and the caller decrements them again. **Two independent causes, and the
symptom is not closed until both are fixed.**

Cause 1 was a plain defect and is fixed by [[Bugs]]: `flowScopeDealias`'s "do
not release what is being returned" test only recognized a bare name use, so a
`VTupleTag` return matched nothing and every element was released. It now walks
a returned tuple element by element. `region-tuple-return` asserts that the
function releases nothing, so the callee half of the symptom is closed and
what remains below is the caller's.

**Cause 2 is this item's, because it needs a decision.** `assignMultRetFlow`
does no move-or-copy accounting, and cannot simply call
`flowHandleMoveOrCopy`: the other three assignment paths hold an `INode**` for
the rval and can replace it with an injected `AliasNode`, while
`assignMultRetFlow` iterates the return *types*, so there is no per-element
value node to wrap. Whatever adjustment a destructuring needs has to attach to
the call as a whole — which is the shape `AliasNode`'s currently-unused `counts`
array was evidently meant for, and which `genlExpr`'s dead `TTupleTag` arm
already anticipates.

Settle it with a function returning a tuple of two `+rc-mut` references,
destructured into two variables, counting the adjustments in `--llvmir` against
the number of live holders.
