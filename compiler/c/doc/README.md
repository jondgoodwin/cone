# compiler/c/doc

Notes on the internals of the C compiler, `conec`. They describe this compiler and retire with it;
the language design notes that would survive a rewrite are in `doc/design/` (`doc/design/_index.md`
is the entry point for both kinds, and maps each compiler phase to its note).

- `phases/`: one note per compiler phase (parse, name resolution, type check, flow, generation).
  Each carries the phase's principles, what it deliberately does not do, its hazards, and a
  file-and-function map into the code.
- `nodes/`: what is true of every IR node (`nodes/_index.md`), plus a note per node family. Includes
  how modules and the packages folder work (`nodes/module.md`).
- `compiler/`: how `conec` itself is built and stays fast. `compiler/architecture.md` is the source
  layout (`parser/`, `ir/` with `exp/`, `stmt/`, `types/`, `meta/`, `corelib/`, `genllvm/`,
  `shared/`) and the rules for adding to it; `compiler/performance.md` holds the baseline for
  `tools/flowbench/` ("Measuring it").
- `diagnostics/`: measuring, error codes, and the test-suite authoring guide (`test-suite.md`).
