# Cone - 0.0.1 Project Plan (as of Sept. 28, 2017)

Compiler helpers have been built: file loading, error output,
arena memory allocation and symbol table.

The next step is 3 weeks of holidays. Compiler work will lag accordingly.

Starting October 23, the objective is to build a simple end-to-end compiler
that uses LLVM to generate code. The implemented language will mirror
the LLVM sample language Kaleidoscope, but will use Cone's syntax.

At the end of this phase, the compiler will be able to load,
compile, link, and run Cone programs that use numeric expressions, 
if/while blocks, and function definition/application.

Key development milestones:

- Lexer
- AST structures
- Primitive types (integers, floats, bool, null)
- Parsing and LLVM code generation for:
 - "print" capability for numbers and string literals
 - local, mutable variables, number literals and assignment
 - arithmetic expressions
 - functions and implicit returns
 - if and while control structures, numeric comparison operators
- Reference documentation for this subset of the language
- Tested code examples that run on Windows and Linux
- Attempted port to WebAssembly and Android
