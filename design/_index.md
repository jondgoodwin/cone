# Cone design notes

These notes provide design context for compiler and language work. The source
code remains the truth for current behavior; notes may also describe intended,
incomplete, or exploratory behavior. Page in the relevant note when its topic
is needed rather than loading the whole folder.

| Document | Contents |
| --- | --- |
| [Type Check Phase](type-check-phase.md) | How the type check phase is scheduled: what demand means, what it may assume from name resolution, the two marks a declaration carries, what a declaration under type check can answer about its type and its size, and the resolution order within each declaration kind. Covers when declarations are checked, not what the checks decide -- see [[Design Documentation]] for that gap. |
| [IR Nodes](ir-nodes.md) | Overview of compiler IR node groups, the major node tags used across semantic passes, the common source-location and node metadata fields, the three type sentinels and what `--checktree` verifies. |
| [Names and Namespaces](names-and-namespaces.md) | Current and intended rules for names, lexical scopes, module and type namespaces, lookup, visibility, imports, aliases, overloading, and a source-code manifest for the implementation. |
| [Return](return.md) | Focused notes for the `return` statement IR node, including its expression payload and placement restriction. |
| [Test Suite](test-suite.md) | Operational guide for adding or updating test coverage: the 17 test groups and what each owns, choosing the scenario to change, writing test sources, what to assert, and updating expectations. |
