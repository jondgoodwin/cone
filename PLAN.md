# Cone - 0.1.0 -> 0.2.0 Project Plan (as of Jan. 7, 2017)

## Current status

The compiler successfully translates Cone programs to .obj files. 
It supports the following documented language features:

- Function declarations with typed parameters, return type and code block
- Global & local variable declaration and initialization
- Return statements (implicit and explicit)
- Assignment expressions (with permission checking)
- Function and method calls
- Control flow: if, while, break, continue and the lazy logical operators
- block and if usable as expressions
- Arithmetic, bitwise and comparison operators
- Global, local and parameter variable reference
- Number literals and types (integers, floats, Bool) of different sizes
- Type inference, type checking and automatic number casting on expressions

## Next steps

- Pointer/reference type
- Permission enforcement
- Lexical memory management: moves, drops and borrows
- type extension and methods
- structs, strings and simple arrays
- Code generation for Linux, Android and WebAssembly
