**Before starting: the sendability rule is already encoded, and nothing reads
it.** `permission.h` declares `RaceSafe` — "may be shared with or sent to another
thread" — and `corelib.c` sets it on `uni`, `imm` and `opaq` while withholding it
from `mut`, `ro` and `mut1`, matching `refconccomm.html`'s sendability prose
exactly. `IsLockless` is set on all six. Neither flag is read anywhere in the
tree: the only occurrences are the two definitions and the six `newPermNodeStr`
calls.

So the first send site this work item creates is one `permGetFlags` test away
from enforcing thread safety on references, against a table that has already been
derived and checked against the documentation. Verified twice by
[[Unenforced language rules]]; do not re-derive it.

`spawn` and `actor` are reserved words as of that same work item, so they may be
taken as keywords here without breaking any program that compiles today.

## OS processes & threads
- Process Spawn, kill
- Thread spawn/fork, join
- Parallelizing unique array reference across multiple threads (structured join)
## Green threads
- Spawn from nursery
- Call ==> enqueues work on channel
- promises
## Work-stealing scheduler 
- Cancellation
- Poll capability



