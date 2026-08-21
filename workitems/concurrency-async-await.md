## `async`

- async splits the block at this keyword's statement right where it is. 
- The right-hand side completes the upper portion by sending a message to another actor
- The left-hand side and below constitute an anonymous behavior (+ closure capturing any state) that unwraps the result and processes it.
- The original behavior call implicitly is auto-populated with actor/behavior to return results to

## Simple syntactic sugar
Consider this, where A,B held in closure state (not local stack)
```
imm A = await x.f (req)
imm B = await y.n (A.req)
```
turns into
```
x.f(req)
TBD
```

- Creates anonymous async methods (behaviors) at `await` boundary
- A function/behavior can name its return values
- A behavior can specify a promise as return value (return `x.f`)
- A behavior can transfer who to return to (its caller vs. itself)
- Returning behaviors bake into the passing of who to return to: delegate of (actor/method)
- Complex awaiting of anonymous behavior until all parameter values have been received, spanning many, handling each as returns
- Logging & return diagnostics
## Auto-detection of workflow dependencies

```
FA = await x.f(req)  // x.f and y.n executed in parallel
FB = await y.n (req) //   since no dependency
C = await z.l(req, A, B)   // z.l fires only when both FA/FB ready
```

## if handling
```
D=if await N.cmd(req) 
   {await O.x(req)}
else
   {P.y(req)}
```

## loop handling
spanning many, handling each
```
each resp = reqs while !timeout
    B.merge(resp)
```