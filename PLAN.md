# Cone - 0.0.1 Project Plan (as of Dec. 4, 2017)

The current development objective is for the compiler to be able to load,
scan, parse, generate (via LLVM), link, and run Cone programs that use numeric expressions, 
local variables, if/while blocks, and function definition/application
(e.g., roughly equivalent to capability in LLVM's sample language Kaleidoscope).

Completed so far: A simple end-to-end compiler that turns source into tokens, tokens into AST,
and AST (via LLVM) into linkable .obj files. It supports the following features:

- Functions and returns
- Primitive numeric types (integers, floats)

Next steps:

- Arithmetic expressions
- Local, mutable variables, and assignment
- If and while control structures, numeric comparison operators
- Tested code examples that run on Windows and Linux
- Attempted port to WebAssembly and Android
