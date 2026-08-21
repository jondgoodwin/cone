
**Modules**
- [[packages-and-separate-compilation|Packages and Separate Compilation]]
- [[using-and-module-name-folding|Using and Module Name-folding]]
- [[module-generics-and-traits|Module generics and traits]]
- [[intrinsics|Intrinsics]]

- Dynamic, interwoven name resolution and type checking, with recursion detection

**Metaprogramming**
- [[macro-and-inline|Macro and Inline]]
- [[generics|Generics]]
- [[metaprogramming|Metaprogramming]]

**Terms, Expressions, and Control Flow
-  Terms
	- [[constant-literals|Constant literals]]
	- [[copy-and-alias-vs-move-semantics|Copy & Alias vs. Move Semantics]]
- Expressions
	- [[type-inference-and-coercion|Type Inference and Coercion]]
	- [[conditional-expressions|Conditional Expressions]]
	- [[operator-methods|Operator Methods]]: `and` `or` `==` `*`
	- [Array expressivity](https://cone.jondgoodwin.com/play/index.html?gist=c51d5cf9bcb9e25b9c653e77211c378a](https://cone.jondgoodwin.com/play/index.html?gist=c51d5cf9bcb9e25b9c653e77211c378a)
- Control Flow:
	- [[each-match-with-builder-blocks|Each, Match, With, Builder blocks]]
	- [[error-handling|Error Handling]]
	- [[use-escape-analysis-and-de-aliasing|Use, escape analysis and de-aliasing]]
	- [[flow-typing-and-refinements|Flow typing and refinements]]

**Types**
- [[names-and-namespaces|Names and Namespaces]], including name paths and name folding
- [[init-and-final|Init and Final]]
- Built-in Types:
	- [[types-function-and-closure|Types. Function and Closure]]
	- [[types-number-and-enum|Types. Number and Enum]]
	- [[types-struct-and-union|Types. Struct and Union]]
	- [[types-pointers-and-borrowed-references|Types. Pointers and Borrowed References]]
	- [[permissions|Permissions]]
	- [[types-array|Types. Array]]
- Library Types
	- [[option|Option]]
	- [[result|Result]]
	- [[virtual-references|Virtual References]]
	- [[regions|Regions]]
	- [[managed-reference-metadata-access-prototype|Managed Reference Metadata Access Prototype]]
	- [[collection-types|Collection Types]] and [[collection-streams-and-iteration|Collection - Streams & Iteration]]
	- [[graph-types|Graph Types]]
## Concurrency
- [[concurrency-primitive-types|Concurrency Primitive Types]]
- [[concurrency-threads|Concurrency Threads]]
- [[concurrency-actors|Concurrency - Actors]]
- [[concurrency-async-await|Concurrency - Async Await]]

**Ecosystem**
- [[compiler|Compiler]] Components
	- [[lexer-and-parser|Lexer and Parser]] [[syntax-aggregating-operators|Syntax - Aggregating operators]]
	- Analysis: `design/phases/type-check.md` describes how type check is
	  scheduled. [[analysis-re-factor|Analysis re-factor]] — done; see `workitems/done/`
	- [[design-notes-follow-on|Design notes follow-on]] — what `design/` still owes.
	  [[design-documentation|Design Documentation]] built it out; done, see `workitems/done/`
	- Generation: [[llvm-generation|LLVM Generation]] and [[c-abi-generation|C-ABI Generation]]
	- [[vault-and-repo-sync|Vault and repo sync]] — the repo is now the only copy of these notes.
	  Done; see `workitems/done/`
	- [[bugs|Bugs]] — defects with a known fix, needing no design decision and no
	  major refactor. Every entry it recorded is fixed, or closed by a decision
	  that it records. Done; see `workitems/done/`. Where an entry was part bug
	  and part open question, the bug half was fixed there and the item holding
	  the rest is named in it.
	- Defects the test suite found, by the decision each needs:
		- [[ownership-memory-safety|Ownership memory safety]] — done; see `workitems/done/`
		- [[diagnose-instead-of-crash|Diagnose instead of crash]] — done; see `workitems/done/`
		- [[unenforced-language-rules|Unenforced language rules]] — every check that could be written
		  has been; the remainder is feature work, carried now by
		  [[types-pointers-and-borrowed-references|Types. Pointers and Borrowed References]],
		  [[concurrency-threads|Concurrency Threads]], [[copy-and-alias-vs-move-semantics|Copy & Alias vs. Move Semantics]] and
		  [[analysis-re-factor|Analysis re-factor]]. Done; see `workitems/done/`
		- [[compiler-defect-backlog|Compiler defect backlog]] — every defect it recorded is fixed; the
		  remainder is design work, carried now by [[compiler|Compiler]],
		  [[permissions|Permissions]], [[type-inference-and-coercion|Type Inference and Coercion]], [[regions|Regions]],
		  [[types-struct-and-union|Types. Struct and Union]] and [[types-function-and-closure|Types. Function and Closure]]
- Build System:
	- Import vs. Include (allow include inside a type?)
	- [[packages-and-separate-compilation|Packages and Separate Compilation]]
- [Language server protocol](https://github.com/Microsoft/language-server-protocol)
- [[playground|Playground]]
- Code examples
	- [[code-examples|Code examples]]
	- Rosetta