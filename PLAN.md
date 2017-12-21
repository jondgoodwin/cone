# Cone - 0.1.0 Project Plan (as of Dec. 22, 2017)

The current development objective is for the compiler to be able to load,
scan, parse, generate (via LLVM), link, and run Cone programs that use numeric expressions, 
local variables, if/while blocks, and function definition/application
(e.g., roughly equivalent to capability in LLVM's sample language Kaleidoscope).

Completed so far: A simple end-to-end compiler that turns source into tokens, tokens into AST,
and AST (via LLVM) into linkable .obj files. It supports the following features:

- Function declarations with typed parameters, return type and code block
- Global & local variable declaration and initialization
- Return statements (implicit and explicit)
- Expression statements
- Assignment expressions  (with permission checking)
- Function calls
- Global, local and parameter variable reference
- Number literals and types (integers, floats) of different sizes
- Type inference, type checking and automatic number casting on expressions

Next steps:

- Arithmetic expressions
- If and while control structures, numeric comparison operators

- Tested code examples that run on Windows and Linux
- Attempted port to WebAssembly and Android
