# logsink

A console sink for the `trace` package: `logsink.install(Level.Info)` and every event of a kind no more detailed
than that is written as one text line on standard output (RFC 3339 UTC time, level, target, text,
`name=value` fields). The source's header comment (`src/logsink.cone`) is the account of the package, the line
layout and how lines stay whole; `doc/manual/lib-logsink.html` is the page for its users. It imports `trace`,
`calendar`, `time`, `collections` and `stdio`.

## Run

```
congo test        # in this folder
```

`whole` starts child processes of itself (8 threads, then 8 actors, logging at once) and checks their captured
output line by line.

## Not here

A file sink, rotation, colour, another stream than standard output.
