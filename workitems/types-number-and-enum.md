
Fix syntax on typed number literals:  10:int-32

Hexadecimal floating point literals (including ‘p’ exponent)

Structural number types (e.g., int-128 or float-32)
- Type specification & correct parsing
- Type match and subtype match
- Coercions from structural and struct-based number types

Enum
- ir node that is a namespace
- Parse to create a set of const integer names w/ type of enum
- Gen to ignore enum node    
- Methods?

Atomic number types

Rebuild nominal number types using struct, generics (w/ const integer), intrinsic functions
- Change type literal constructor to work with any arbitrary type
- Support them w/ generics
- Support intrinsics for the methods
- Core library