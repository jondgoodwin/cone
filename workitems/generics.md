
- Bug: `imm mmm = Some[Option[Bool]][Some[true]]`
- Generics inference

- Switch to `()` vs. `[]`?  [[syntax-aggregating-operators|Collecting operators]]
- Generic over integers vs. types
- Type constraint arithmetic `where`

Research:
Generics (see [Go](https://go.googlesource.com/proposal/+/master/design/go2draft-generics-overview.md) and [contracts](https://go.googlesource.com/proposal/+/master/design/go2draft-contracts.md#why-not-use-interfaces-instead-of-contracts)) [Swift](https://www.reddit.com/r/swift/comments/3r4gpt/how_is_swift_generics_implemented/cwlo64w/?st=jkwrobje&sh=6741ba8b) and [.NET](https://www.microsoft.com/en-us/research/wp-content/uploads/2001/01/designandimplementationofgenerics.pdf) ([part 2](http://mattwarren.org/2018/03/02/How-generics-were-added-to-.NET/))

## The generic parameter leak, fixed in [[bugs|Bugs]]

`fnDclNameRes`, `structNameRes` and `macroNameRes` all resolved their parameter
list before `nametblHookPush()`, so a type parameter leaked into the enclosing
module namespace. Measured three ways — a valid program rejected, an invalid one
silently accepted, and a hard compiler abort on instantiation. All three now
resolve the parameter list inside the push, which is what hooks it in the right
table; the explicit re-hook loop each of them carried is gone with it.
