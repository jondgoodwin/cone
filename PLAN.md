# Cone - 0.1.0 -> 0.2.0 Project Plan (as of Jan. 7, 2017)

## Current status

The compiler successfully translates Cone programs to .obj or .o files on Windows, Linux and Mac OS. 
It supports the following documented language features:

- Function declarations with typed parameters, return type and code block
- Global & local variable declaration and initialization
- Return statements (implicit and explicit)
- Assignment expressions
- Function and method calls
- Control flow: if, while, break, continue and the lazy logical operators
- block and if usable as expressions
- Arithmetic, bitwise and comparison operators
- Global, local and parameter variable reference
- Number literals and types (integers, floats, Bool) of different sizes
- Type inference, type checking and automatic number casting for:
  -- Number types (e.g., i32, u32, f32)
  -- References (incl. & and * operators)
- Permission specification, coercion and enforcement

## Next steps

- structs
- method definition
- Code generation for Android and WebAssembly
