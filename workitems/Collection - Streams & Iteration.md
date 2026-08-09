
## Stream iterators

These are more performant if they are not closures that return a nullable value, but instead are new state structs with two methods:
- isEnd
- () which retrieves a value
Test to see if this is actually faster, despite the extra call (or is there one if it is a macro)

We could use `yield` in a similar way to `async` to create stream generators.  Probably better to use a iterator.

## `lazy`

See its use in `[lazy exp]` to initialize or copy values

Is there any value in having while or each generating multiple values, one at a tie, much like a `yield`