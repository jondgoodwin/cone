
1. Install LLVM and ensure compiler compiles
2. [[IR refactor]]
3. [[Analysis re-factor]] - jit, with recursion checking
4. [[Packages and Separate Compilation]]
5. [[Using and Module Name-folding]] w/ [[Names and Namespaces]]
6. [[Intrinsics]]
7. [[Macro and Inline]]
8. Generics on numbers
9. C-competitive and -compatible
	- [[Types. Pointers and Borrowed References]]

Add test suite is done; see `workitems/done/`. It left four unplaced follow-ups,
listed under Compiler Components in [[_index]] and not slotted here yet.
[[Ownership memory safety]] is now done as well, and what it did not settle went
to the items that own those decisions: fallible allocation to [[Regions]], array
fill literals to [[Types. Array]], and two loose defects to
[[Compiler defect backlog]]. Still unplaced: [[Diagnose instead of crash]],
[[Unenforced language rules]], [[Compiler defect backlog]].

