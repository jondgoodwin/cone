
# Parenthesis: ()
## (,)  Tuple or Parenthesis
An anonymous, ordered list of heterogeneous elements (sugar for anonymous struct!)
- `()`, the empty tuple, is equivalent to the value of `null` (of type `void`)
- Single element is not treated as a "tuple", but as dropped parenthesis for precedence clarity.
- Multiple elements (comma-separated) can be all values or all types: 
	- Neither tuple nor elements are given names (use `{}` struct/constructor if names needed)
		- An element inside a tuple can be indexed by `.1`, a constant integer
	- Tuple types are matched structurally by arity, and in element order by type 
	- A value tuple is allocated in registers/stack by default

## x(,)  Function/generic definition or application
An ordered list of parametric values to be defined applied to a function or a generic entity:
- Definition: comma-separated parameters:  `name: type [=value]` 
- Application: comma-separated arguments: `[name:] value` to a type with `()` method
Generic or macro parameters can be type, constant, or various other kinds. Also `lazy`
# Square brackets: []
## [,] Array
**Value**: An ordered list of same-typed value elements, in one of these formats
- `[]` is the empty list
- **Comma-separated elements**
- `[lazy exp]` uses an iterator or closure to lazy populate the list. (or `from` or `in`)
**Type**: Not allowed. See below for array declaration.
## x[] Indexed definition or application
**Value**: *just like x(,) above, except apply it to `[]` method, typically on a collection*
**Type**: syntactic sugar to define Array generic type:  `i32[4]` is same as `Array(i32,4)`

# Curly braces: {}
## {;} Object
The syntax/semantics varies significantly depending on the kind of object:
- Value block wherever expressions are allowed inside function declaration or value initializer:
	- Semi-colon-separated statements/expressions.
	- Only last statement/exp calculates the block's 'returned' value. Return value for others is ignored
- Struct, etc: It constructs multiple namespace dictionaries and lists, semantically enriched
	- "struct" record layout 
	- methods
	- auxiliary names in namespace: types etc.
	- TBD
## x{} Constructor
- Named or ordered sequence of values: `[name:] value`
	- *default*: Ties to field names of type for initialization
	- init-based:  Ties to parameters of one of several `init` methods for the type
	- For a collection, including a `[]` can be used to initialize the collection!

# Quotation marks
## $"" Interpolation
TBD.