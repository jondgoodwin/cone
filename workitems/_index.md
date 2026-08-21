
**Modules**
- [[Packages and Separate Compilation]]
- [[Using and Module Name-folding]]
- [[Module generics and traits]]
- [[Intrinsics]]

- Dynamic, interwoven name resolution and type checking, with recursion detection

**Metaprogramming**
- [[Macro and Inline]]
- [[Generics]]
- [[Metaprogramming]]

**Terms, Expressions, and Control Flow
-  Terms
	- [[Constant literals]]
	- [[Copy & Alias vs. Move Semantics]]
- Expressions
	- [[Type Inference and Coercion]]
	- [[Conditional Expressions]]
	- [[Operator Methods]]: `and` `or` `==` `*`
	- [Array expressivity](https://cone.jondgoodwin.com/play/index.html?gist=c51d5cf9bcb9e25b9c653e77211c378a](https://cone.jondgoodwin.com/play/index.html?gist=c51d5cf9bcb9e25b9c653e77211c378a)
- Control Flow:
	- [[Each, Match, With, Builder blocks]]
	- [[Error Handling]]
	- [[Use, escape analysis and de-aliasing]]
	- [[Flow typing and refinements]]

**Types**
- [[names-and-namespaces|Names and Namespaces]], including name paths and name folding
- [[Init and Final]]
- Built-in Types:
	- [[Types. Function and Closure]]
	- [[Types. Number and Enum]]
	- [[Types. Struct and Union]]
	- [[Types. Pointers and Borrowed References]]
	- [[Permissions]]
	- [[Types. Array]]
- Library Types
	- [[Option]]
	- [[Result]]
	- [[Virtual References]]
	- [[Regions]]
	- [[Managed Reference Metadata Access Prototype]]
	- [[Collection Types]] and [[Collection - Streams & Iteration]]
	- [[Graph Types]]
## Concurrency
- [[Concurrency Primitive Types]]
- [[Concurrency Threads]]
- [[Concurrency - Actors]]
- [[Concurrency - Async Await]]

**Ecosystem**
- [[Compiler]] Components
	- [[Lexer and Parser]] [[Syntax - Aggregating operators]]
	- Analysis: `design/phases/type-check.md` describes how type check is
	  scheduled. [[Analysis re-factor]] — done; see `workitems/done/`
	- [[Design notes follow-on]] — what `design/` still owes.
	  [[Design Documentation]] built it out; done, see `workitems/done/`
	- Generation: [[LLVM Generation]] and [[C-ABI Generation]]
	- [[Vault and repo sync]] — the repo is now the only copy of these notes.
	  Done; see `workitems/done/`
	- [[Bugs]] — defects with a known fix, needing no design decision and no
	  major refactor. Every entry it recorded is fixed, or closed by a decision
	  that it records. Done; see `workitems/done/`. Where an entry was part bug
	  and part open question, the bug half was fixed there and the item holding
	  the rest is named in it.
	- Defects the test suite found, by the decision each needs:
		- [[Ownership memory safety]] — done; see `workitems/done/`
		- [[Diagnose instead of crash]] — done; see `workitems/done/`
		- [[Unenforced language rules]] — every check that could be written
		  has been; the remainder is feature work, carried now by
		  [[Types. Pointers and Borrowed References]],
		  [[Concurrency Threads]], [[Copy & Alias vs. Move Semantics]] and
		  [[Analysis re-factor]]. Done; see `workitems/done/`
		- [[Compiler defect backlog]] — every defect it recorded is fixed; the
		  remainder is design work, carried now by [[Compiler]],
		  [[Permissions]], [[Type Inference and Coercion]], [[Regions]],
		  [[Types. Struct and Union]] and [[Types. Function and Closure]]
- Build System:
	- Import vs. Include (allow include inside a type?)
	- [[Packages and Separate Compilation]]
- [Language server protocol](https://github.com/Microsoft/language-server-protocol)
- [[Playground]]
- Code examples
	- [[Code examples]]
	- Rosetta