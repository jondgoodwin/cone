**Name generation**
- Fix unique name generation algorithm (and document in cabi page)
- Research Rust/C++/D mangling approach
- Fix gen name/prefix for modules (mod keyword so that only pgm has no prefix)
- Type-based name generation, including generic type names(type parameters)
- Function mangling for overloads, anonymous functions (private)
	- Methods with a non-const permission won’t correctly mangle the name

