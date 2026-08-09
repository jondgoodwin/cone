
Error handling and panic
- Rethink catch design. See: [https://www.microsoft.com/en-us/research/publication/exceptional-syntax/](https://www.microsoft.com/en-us/research/publication/exceptional-syntax/)
- try { a? + b? } is my favorite example -- it's like match (a, b) { (Some(a), Some(b)) => Some(a+b), _ => None }
- And [https://doc.rust-lang.org/nightly/unstable-book/language-features/try-blocks.html](https://doc.rust-lang.org/nightly/unstable-book/language-features/try-blocks.html)
- Numeric overflow protection (swift)
- Each x in 0 <= 0x7fffffff will loop forever because of LLVM optimization to true
- Boundary checking
- Assert

## Resources

Exception Handling
- [C++ proposal](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0709r0.pdf) 
- Reddit conversion. [Rust](https://www.reddit.com/r/rust/comments/8iwm5x/c_throwing_values_proposal_resultlike_error/?st=jh5eir8e&sh=1e59fac8)   [C++](https://www.reddit.com/r/cpp/comments/8iw72i/p0709_r0_zerooverhead_deterministic_exceptions/?st=jh5ejpsh&sh=da313c9e)   [ProgrammingLanguages](https://www.reddit.com/r/ProgrammingLanguages/comments/8ivehl/p0709r0_zero_overhead_deterministic_exceptions/?st=jh5dj7iv&sh=ae98a3c5)
- Delimited continuations: [resource](https://gist.github.com/sebfisch/2235780)

[Duffy](http://joeduffyblog.com/2016/02/07/the-error-model/):
- Return code errors are a performance hit on both sides
- Errors never silently ignored:  ignore xxx, notnull(exp)
- if err := foo(); err != nil {
- Error handling logic (and RAII cleanup) - Go handle
- let value = try!(foo);  [try! macro](https://doc.rust-lang.org/std/macro.try!.html) 

[Sutter](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0709r0.pdf)
- Installable panic handler?

Abandonment (panic):
- An incorrect cast.
- An attempt to dereference a null pointer.
- An attempt to access an array outside of its bounds.
- Divide-by-zero.
- An unintended mathematical over/underflow.
- Out-of-memory.
- Stack overflow.
- Explicit abandonment.
- Contract failures on fns: ensures xxx, and requires xxx
- Assertion failures. debug.assert/release.assert

Catch on every block (no need for try)
- Happy path starts every block. 
- Multiple catches can be written for a different error types. It can return, throw or panic
- If catches provided (and if not?): Result returns are implicitly checked for success and type unwrapped automatically on happy path. Otherwise jump to appropriate catch where result is auto-unwrapped and handled.
- One catch for any error, also. Compiler can error if a result is not caught.
- Can we avoid need/use of explicit pattern match/unwrapping short-circuit operators, by allowing an error to be uncaught if the function throws an error of that type? Thus, auto-throw for an error of compatible type. If not caught and not compatible, it panics.
