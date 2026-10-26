# trace

The facade every package logs and traces through: kinds of event declared
once, emitted through macros that cost a relaxed load and a compare while
nothing wants them (their arguments then never evaluated), one sink or two
installed by the program, and levels by target from code or `CONE_TRACE`. The
source's header comment (`src/trace.cone`) is the account of the package;
`doc/manual/lib-trace.html` is the page for its users. It imports only `core`.

## Run

```
congo test                                                    # in this folder
congo run --release examples/disabledcost.cone -- 200000000   # a disabled emit's cost
```

`awaits` runs the actors runtime: run the package's tests under the memory cap
the actors tests use. `disabledcost` prints `key value` lines: the best of five
rounds of a hot loop with and without a disabled `emit`, in picoseconds a pass.
Timings on a machine other programs share are only good taken one at a time.

## Not here yet

- Sinks (a buffered binary file, ETW, console text), the packages' own
  instrumentation, metrics, and tools: each a package of its own, later.
- A fan-out as a sink of its own, `Fanout is Sink { first: Uni[Sink]; second:
  Uni[Sink]; }` made by `fanout(a, b) Uni[Sink]`, nestable for more than two.
  Today's compiler sizes such a value wrongly when it is held as a `Uni[Sink]`
  (one byte is allocated for it), so the fan-out is `installPair`, two slots in
  the facade, until that is fixed.
- More than four fields to a kind: each number of fields is a type of its own,
  `Kind0` to `Kind4`, since a type takes no variable number of type arguments.
