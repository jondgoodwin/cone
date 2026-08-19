Design: `design/Analysis.md` builds this design one example at a time, deriving
each mechanism from the program that forces it, and measuring current behaviour
rather than reading it. Its sections 1-9 are the design in order; 10 is
resolution order per declaration kind; 12 is what changes per node; 13 is
reference, including the five defects it closes (13.4) and the questions that
were settled deliberately (13.5). Read it before starting.

Plan: `workitems/Analysis re-factor plan.md` sequences the work into eight
commits, each green and revertable on its own, with what each changes, why it
is safe at that point, and which test expectations it moves.

1. Create AnalysisState from folding TypeCheckState into NameResState
	1. Rename NameResState to AnalysisState
	2. Rename nState to aState
	3. Add `FnDclNode *fn` to AnalysisState
	4. Rename TypeCheckState to AnalysisState
	5. Rename tState to aState
2. Create analyze method for nodes, beginning with pgm, that combines nameres and typecheck. 
	1. Be sure to confirm nameres did not error before running typecheck.
	2. Check for recursion and place limits on infinite recursion
	3. nameres: dictionary is complete and all names are resolved
	4. typecheck: conforms when constraint exists, inferred when it does not
	5. flow for escape, permission, lifetime, read-only-when-populated analysis
	6. No lowering during name resolution: move the '<-' value-tuple
	   expansion in fnCallNameRes, and the bare-name-to-self.member rewrite
	   in nameUseNameRes, into type check where the types they need exist.
	   (Lowering cannot move any later than that -- it is what establishes
	   the node's type. See design/Analysis.md section 10.)
3. Switch to just-in-time name resolution and type check to fix ordering bugs. For pgm, mod, struct, union, fnsig nodes
	2. Visit all imports first, but not includes! (module-only)
	3. Build this level's dictionary of all declared names (do this in parser), including fields/var, methods/fn/macros/behaviors, typedcls, constants, etc.
	4. Analyze base type of a type, and perform folding of fields/names
	5. Fully analyze all isolated named-type / constant (non value nodes) in order
	6. Fully analyze all fields/vars, and do folding as needed
		1. (Handle recursion and ensure if 'len' is unknown because of unfinished analysis, this is an error!)
	7. Fully analyze all functions/methods/macros/behaviors
	8. Fully analyze all variants (and do folding as part of that)

Verification:
- Fix type checking to do vars before function bodies
- Bug when function is declared before global variable it accesses… type is unknown. Probably means we should type check initialized global vars before defs for functions

