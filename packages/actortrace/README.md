# actortrace

Tracing the actors runtime, round 1: the actor level only, four ways of
recording the same events, measured against one benchmark. A first round, to
learn from; names and shape are expected to change. The source's header comment
(`src/actortrace.cone`) is the account of the record, the file and the four
ways; this file is what to run.

## What it records

The `actors` package calls three hooks, each behind a layer's bit in a mask
(one relaxed load and a bit test when off):

| Event | When | Fields |
| --- | --- | --- |
| send, reply | a message (or an awaited reply) is queued | id = a fresh id, which the message carries; argument = the id of the message the sender was running, 0 outside |
| run | a message's behaviour begins | id = the message's id; argument = behaviour (message tag, actor type) |
| end | it returns | the same |

A send's record and the run record with the same id give the message's time in
the mailbox; the sends a behaviour makes carry its id as their cause. A
continuation after an `await` runs under its reply's id, whose send record
names the answering message.

The record is 32 bytes: `u64 time, u16 kind, u16 worker, u32 actor, u64 id,
u64 argument`. A file is a self-describing header (clock frequency, kinds,
layers), then records.

## The four ways (`start(method, ...)`)

- `methodLogger` (A): a logger actor, one message per event.
- `methodBuffers` (B): a buffer for each worker, a full one moved whole to the
  logger as one message; the pool of spare buffers is finite (drop and count)
  unless `Options.grow`.
- `methodEtw` (C): Event Tracing for Windows, self-describing events written
  with advapi32 directly (`src/etw.cone`). Provider `Cone.ActorTrace`,
  `{5b8e3b2c-7f1d-4a65-9c2e-0d6a41f3b7e9}`. Recording a session needs no
  elevation for a member of Performance Log Users:
  `tracelog -start Name -f out.etl -guid #5b8e3b2c-7f1d-4a65-9c2e-0d6a41f3b7e9`, run the
  program, `tracelog -stop Name`, then `tracerpt out.etl -of CSV -o out.csv` or open the
  file in Windows Performance Analyzer.
- `methodText` (D): a line of text for each event, formatted by the worker into
  its own buffer, written when full.
- `methodNull` and `methodCount`: the hook and sink with nowhere to put the record,
  for the cost of the hook alone.

## Run

```
congo test                                                  # in this folder
congo run --release examples/bench.cone -- logger high 8 100000 5000 trace.bin
congo run --release examples/microbench.cone -- buffers enabled 1000000 trace.bin
congo run --release examples/tracestat.cone -- trace.bin    # the reader
```

`bench` prints `key value` lines: the time the workload took to complete, the
time to finish writing, what the recording did. Timings on a machine other
programs share are only good taken one at a time.

## Limits of round 1

- The clock is QueryPerformanceCounter, 100 ns a tick on the machine measured:
  a message's run is often under a tick. A finer clock (the time stamp counter)
  is a next step.
- Behaviours and actor types have numbers, not names: the compiler gives the
  runtime no type name.
- Hooks for steal, idle, await and resume, switching a layer's level from
  outside the process, the flight recorder, and HTTP-layer hooks come later.
