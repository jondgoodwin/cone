# Cone - 0.0.1 Project Plan (as of Nov. 11, 2017)

The current development objective is for the compiler to be able to load,
scan, parse, generate (via LLVM), link, and run Cone programs that use numeric expressions, 
local variables, if/while blocks, and function definition/application
(e.g., roughly equivalent to capability in LLVM's sample language Kaleidoscope).

Completed so far: 

- Shared helpers for file loading, error output,
arena memory allocation, symbol table and AST handling.
- Lexer and parser can transform source program's numeric literals into an AST.

Next steps:

- A simple LLVM backend that generates a native-code program.
- Primitive numeric types (integers, floats)
- Arithmetic expressions
- Local, mutable variables, and assignment
- Functions and implicit returns
- If and while control structures, numeric comparison operators
- Reference documentation for this subset of the language
- Tested code examples that run on Windows and Linux
- Attempted port to WebAssembly and Android
