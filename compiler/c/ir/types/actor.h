/** Actors: what an 'actor' declaration became
 *
 * An actor is no node of its own. The parser turns 'actor Name { ... }' into
 * ordinary declarations (parser/parseactor.c): the state, a struct of its
 * fields and methods; the message enum; the handle, the struct the actor's
 * name names, whose methods send; and the dispatch function. What the rest of
 * the compiler must still know of the actor as one thing is kept here: which
 * arguments cross to the thread the actor runs on, which the thread check asks
 * of once type check is done; which handle stands for which state, so that
 * a use of the state through the handle is reported as private state; and
 * what an 'await' and 'selfactor' need -- which handle method sends a message
 * awaited, which of the state's methods hold an 'await', the state's hidden
 * fields and the functions generated for its seams, and the actors package's
 * functions generation calls.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef actor_h
#define actor_h

// One message of an actor: its behaviour, and the handle's methods that send it
typedef struct ActorMessage {
    FnDclNode *method;      // The state's method declared 'async do', the behaviour
    FnDclNode *send;        // The handle's method sending it, no reply wanted
    FnDclNode *ask;         // The handle's method sending it awaited, with a reply's
                            // envelope; NULL where the method returns nothing
    FnDclNode *future;      // The handle's method sending it for a future, which it
                            // gives back; NULL where the method returns nothing
} ActorMessage;

// What the parser generated for one actor
typedef struct ActorInfo {
    StructNode *handle;     // What the actor's name names
    StructNode *state;      // Its fields and methods, reached only by the runtime
    Nodes *crossing;        // FnDclNode, VarDclNode pairs: each argument that crosses to the actor
    ActorMessage *msgs;     // Its messages, in the order written
    uint32_t nmsgs;
    Nodes *behaviours;      // The state's methods declared 'async do', refused ones too
    Nodes *awaiting;        // The state's methods whose bodies hold an 'await'
    FnDclNode *dispatch;    // The dispatch function, which alone calls a message
    FnDclNode *selffn;      // 'selfactor': the handle, from the state
    FnDclNode *replyfn;     // A request's envelope, its resume message naming no record
    FnDclNode *replyidfn;   // The same, naming the record's id
    FieldDclNode *pending;  // The state's hidden pending table, where it holds an 'await'
    FieldDclNode *answer;   // The state's hidden Answer slot, where a message returning a value holds an 'await'
} ActorInfo;

// Record an actor the parser generated, filled in by the parser. Every
// parameter whose argument crosses to the actor -- each message's and each
// initializer's, 'self' aside -- is in 'crossing', as the state's own
// VarDclNodes, where the author wrote them
void actorRegister(ActorInfo *info);

// The actors package's functions a split method's generated code calls,
// found where the parser bound the package (parseactor.c)
enum ActorRuntimeFn {
    ActorRtParkReserve,     // parkReserve(p &mut Pending, size usize) u64
    ActorRtParked,          // parked(p &mut Pending, id u64) *u8
    ActorRtUnpark,          // unpark(p &mut Pending, id u64) *u8
    ActorRtRecordFree,      // recordFree(block *u8)
    ActorRtAnswerTo,        // answerTo(slot &mut Answer) *u8
    ActorRtAnswered,        // answered(slot &mut Answer)
    ActorRtStartAwait,      // startAwait(a *u8, rp Reply): an Awaitable started at its seam
    ActorRtFutureReady,     // futureReady(fut *u8) bool: a Future has its ending
    ActorRtFutureRegister,  // futureRegister(fut *u8, rp Reply) bool: park on it, unless it has
    ActorRtFutureOpen,      // futureOpen(fut *u8, file &[]u8, line u32) *u8: where its value is
    ActorRtFutureTaken,     // futureTaken(fut *u8): a move value read out of it
    ActorRtCount
};
extern FnDclNode *actorRuntime[ActorRtCount];
extern char *actorRuntimeNames[ActorRtCount];

// The actors package's Future[T], the consumer's reference to a future: what
// a call of a behaviour returning a T gives where its value is used, and what
// an 'await' may wait for. NULL until the parser binds the package
extern StructNode *actorFuture;

// The T of an instance of actors.Future[T], what an 'await' on it gives, or
// NULL where 'type' is no such instance
INode *actorFutureResult(INode *type);

// A call whose value is used, of a handle's method sending a behaviour that
// returns a value, is made a call of the handle's method that sends it for a
// future, and its value is the future. Called once the call is checked
void actorFutureCall(TypeCheckState *pstate, FnCallNode *call);

// The operand an 'await' is checking, whose call (if it is one) is not made
// a future's: the 'await' takes the reply itself
extern INode *actorAwaitOperand;

// The actors package's Awaitable[R], the generic an 'await' on an operation
// (an I/O operation) awaits an instance of, found where the parser bound the
// package; NULL until then
extern StructNode *actorAwaitable;

// The R of an instance of actors.Awaitable[R], what an 'await' on it gives,
// or NULL where 'type' is no such instance
INode *actorAwaitableResult(INode *type);

// Refuse each crossing parameter, and each message's returned type, that is
// not Sendable. Called once, when type check has finished and every type is
// laid out
void actorCheckAll();

// Is this the state of an actor, whose methods its dispatcher runs?
int actorIsState(INode *type);

// The actor whose state 'type' is, or NULL
ActorInfo *actorOfState(INode *type);

// The actor whose handle 'fn' makes from its state ('selfactor'), or NULL
ActorInfo *actorOfSelfFn(FnDclNode *fn);

// The message a handle's method sends, and its actor, or NULL where 'send' is
// no handle's method that sends one
ActorMessage *actorMessageOfSend(FnDclNode *send, ActorInfo **info);

// Does the body of this method of an actor's state hold an 'await'?
int actorMethodAwaits(ActorInfo *info, FnDclNode *method);

// The actor whose behaviour 'fn' is, declared 'async do', or NULL
ActorInfo *actorOfBehaviour(FnDclNode *fn);

// The message of actor 'info' whose behaviour is named 'name', or NULL
ActorMessage *actorMessageNamed(ActorInfo *info, Name *name);

// The member of an actor's state that 'name' names, where 'type' is the
// actor's handle and the handle has no such member of its own, or NULL. '*state'
// is set to the state. A use of it is a use of the actor's private state
INode *actorStateMember(INode *type, Name *name, StructNode **state);

#endif
