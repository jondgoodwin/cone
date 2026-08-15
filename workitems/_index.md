
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
- [[Names and Namespaces]], including name paths and name folding
- [[Overload Refactor]]
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
	- [[Analysis re-factor]]
	- Generation: [[LLVM Generation]] and [[C-ABI Generation]]
	- [[Test Suite]]
- Build System:
	- Import vs. Include (allow include inside a type?)
	- [[Packages and Separate Compilation]]
- [Language server protocol](https://github.com/Microsoft/language-server-protocol)
- [[Playground]]
- Code examples
	- [[Code examples]]
	- Rosetta