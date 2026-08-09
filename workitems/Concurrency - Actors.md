Actors:
- Is type-like.
	- Data Structure: queue, state, GC
	- Methods encode parms as message and add to queue. 
	- Actor itself added to a thread's scheduler who dispatches (or steals)
	- Dispatch unrolls to call interior behavior with ref to self (state)
- Define the MPSC/MPMC channel to use, but auto-build the T type the queue accepts
- All behaviors turn into the definition for the T type
- Method calls add the message to the queue
- Some other way to trigger dispatch on a popped message
- Actor modularity and inheritance

