
**Methods**: Further generalize code for methodtypes
- Change 1st parm type on AddFn and AddField to MethAstNode *
- Move code out from struct.c, generalize to other types (e.g., numbers, arrays)

**Anonymous functions**

**Closures:**
- Design and document: Parse, semantic lowering, traits

`fn {q,inc=3}(v i32) {inc-- > 0? v*q : null}`

The first set of curly braces is the closure's internal state as captured in an anonymous struct:
- `q` means we are copying and capturing `q` from the outer state
- `inc=3` means we are capturing a calculated/constant value for the state
If it is missing, it will be implied by noticing any outer variable used inside the closure
We want it whenever we want captured state to not just be a copy of some outer state variable.

Notice: return type need not be defined.

This allocates the state struct and attaches to it a `()` method for the function.
It can be allocated as part of a `new` heap allocated object.
Its lifetime restrictions are limited by whether it captures any lifetime-restricted value


**Calculated Properties:**
- Implement `var.prop = 12` syntactic sugar

**Pure functions:**
- Trust and pure flags: fold up to functions/methods

