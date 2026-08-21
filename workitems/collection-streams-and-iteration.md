
## `<-` gets its own test group, once there are collections

The append operator is implemented and reached today only through stdio's
`IOStream`: it lexes as `LessDashToken`, interns as `lessDashName`, and
`fncall.c:100` lowers it on a vtuple to a block appending each element
separately. It is an append operator, not a channel send.

It is intended for collections, and there are none — `corelib/` defines
`Option`, `Result`, `so` and `rc` and no container types at all, which the
`collection` coverage group established while being written. So `<-` is the one
construct [[add-test-suite|Add test suite]] could not place: no group's subject covers it, and
covering it through stdio alone would pin the operator to the one consumer that
is not what it is for.

**It gets its own coverage group when collections arrive**, alongside whatever
manual chapter they bring. `design/diagnostics/test-suite.md` carries the reserved row.

## Stream iterators

These are more performant if they are not closures that return a nullable value, but instead are new state structs with two methods:
- isEnd
- () which retrieves a value
Test to see if this is actually faster, despite the extra call (or is there one if it is a macro)

We could use `yield` in a similar way to `async` to create stream generators.  Probably better to use a iterator.

## `lazy`

See its use in `[lazy exp]` to initialize or copy values

Is there any value in having while or each generating multiple values, one at a tie, much like a `yield`