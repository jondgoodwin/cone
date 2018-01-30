# Cone - 0.1.0 -> 0.2.0 Project Plan (as of Jan. 31, 2017)

## Current status

The compiler translates Cone programs to C-ABI compatible object files on Windows, Linux and Mac OS.
As demonstrated by the Cone playground, it supports the following documented language features:

- Function declarations with typed parameters, return type and code block
- Global & local variable declaration and initialization
- Return statements (implicit and explicit)
- Assignment expressions
- Function and method calls
- Control flow: if, while, break, continue and the lazy logical operators
- block and if usable as expressions
- Arithmetic, bitwise and comparison operators
- Reference ('&' / '*') and struct ('.') operators
- Global, local and parameter variable use
- Number, string and Bool literals
- Type inference, type checking/coercion, and number casting for:
  - Integers (e.g., i32, u32) and Bool
  - Floats (f32 and f64)
  - References
  - Structs
- Permission specification, coercion and mutation enforcement

## Next steps

- Struct literals
- Method definition for types
- Constructors and finalizers
- Simple macro/inline
- lex reference allocator + methods
- Allocator events: drop, alias, de-alias
- 'uni'/reference constraints: move semantics, var shadows
- Modules and packaging
