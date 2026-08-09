
1. Create namedef node (name, type/constraint, value, generic?, owner?) and convert compliant nodes accordingly
	1. Switch exp/type detection algorithm to not use nodetype as signal, thereby fixing nameuse variants etc.
	2. See [[Namedef Refactor]]
2. Create genericdef node (genname, parmdefs, body)
	1. Handle genname correctly regardless of name aliasing and cross-module references and "ownership"
3. Add a "header" to type nodes that includes:
	1. len/alignment
	1. Special constraints and flags (abstract/concrete, and ?)
	1. Constraint expressions, addressing `where` clauses, lazy and other generic constraints, lifetime constraints, permissions, etc.
4. Converge struct/mod/union node structure, with ways to distinguish. Does this work for unions?
	1. imports and other first-processed nodes
	2. Compile-time name "type"/mod nodes
	3. named vars/fields (with folding?)
	4. named fn/macros/etc.
	5. Variants

