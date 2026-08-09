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
	6. lower, make this happen after typecheck (e.g., fncall)
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

