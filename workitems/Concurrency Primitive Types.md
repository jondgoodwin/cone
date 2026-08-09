Language built-in support:
- Type attribute marker that indicates copied data (e.g., ref alias) is leaving one thread for another, so static errors can be raised based on permissions
## Atomics and memory model
`conc` permission?
## Locks and Semaphores
Concurrency permissions and data structures
- Lock permissions:  mutex (single and multi-thread), rwlock
- Acquire and release lock
- Borrow
## Channels (concurrent FIFO ring)
- Channels are generics, polymorphic on the type of the value.
- Variations: Multiple tx, single receiver, a single double-ended channel (Go)
- Append to tx
- Iterable rx
- Channel can be buffered (sized)
- Channel closing and tested for closure
Simple [queue algorithm](http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue)
## Concurrent, lockless data structures

## Asynchronous I/O (as actors?)
- Transport/Sockets/Http
- File
## Timers
  
