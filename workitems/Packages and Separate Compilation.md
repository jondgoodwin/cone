
**Design and Document:**
- Package fundamentals
	- Package build files
	- Congo - build and use libraries
	- Compiler options
- Language syntax & nodes
	- `mod` (including future generics). Distinct from structs:
		- A package can only be a module, not a type
		- Module cannot be imported into a type, but types can namespace other types, macros, etc.
		- Module cannot have fields/methods, only vars/functions
		- Global variables do not support delegated inheritance, unlike fields
	- `extern` - get rid of this, because an extern is a name in an imported package!
	- `import`
	- `include` (and role of separately compiled files)
- LLVM supports reusable "precompiled headers"?

**Implementation:**
1. Add `mod` node and parse it in source files
2. Refactor existing source file handling logic:
	1. Separate import file handling from module. 
	2. File handling is a dictionary. 
	3. Module names come from parsing mod in source file. 
	4. Mark fn and var on whether they are part of current source file or not, helping to decide what to generate in object file.
		- FlagExtern should instead be a collection globals in an “extern” module

3. Add Congo ability to build and use libraries
4. Refactor Cone handling of core, stdio library in Conehome
	1. Make them external Cone packages & source code files
## Refinements

- Overloaded function names, specifying real name
- Module name concatenation  _ vs : for module namespace separator (generation)
- Ensure that linkonce works correctly with generic functions
## Auto-generate library header file