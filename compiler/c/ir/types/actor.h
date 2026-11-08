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
    FnDclNode *method;      // The state's method declared 'async fn', the behaviour
    FnDclNode *send;        // The handle's method sending it, no reply wanted
    FnDclNode *ask;         // The handle's method sending it awaited, with a reply's
                            // envelope; NULL where the method returns nothing
    FnDclNode *future;      // The handle's method sending it for a future, which it
                            // gives back; NULL where the method returns nothing
    int generic;            // Set for a message of an instance of a generic actor trait's handle:
                            // 'method' is the template's requirement, in its type parameters, so
                            // what the behaviour returns is what 'future' holds (actorMessageReturns)
} ActorMessage;

// What the parser generated for one actor. A generic actor's is the template's:
// its nodes are in the type parameters. Each instance of its handle has an info
// of its own (actorOfHandle, actorOfState), made when it is first asked for, with
// the instance's counterpart of every node here
typedef struct ActorInfo {
    struct ActorInfo *tmpl; // For an instance of a generic actor: the template's; NULL otherwise
    StructNode *handle;     // What the actor's name names
    StructNode *state;      // Its fields and methods, reached only by the runtime
    StructNode *msg;        // Its message enum (a generic actor's: the template's, in the type parameters)
    Nodes *crossing;        // FnDclNode, VarDclNode pairs: each argument that crosses to the actor
    ActorMessage *msgs;     // Its messages, in the order written
    uint32_t nmsgs;
    Nodes *behaviours;      // The state's methods declared 'async fn', refused ones too
    Nodes *awaiting;        // The state's methods whose bodies hold an 'await'
    FnDclNode *dispatch;    // The dispatch function, which alone calls a message
    FnDclNode *selffn;      // 'selfactor': the handle, from the state
    FnDclNode *isfn;        // Its 'is' list: a function of one parameter, whose type is the list; NULL where it has none
    FnDclNode *replyfn;     // A request's envelope, its resume message naming no record
    FnDclNode *replyidfn;   // The same, naming the record's id
    FieldDclNode *pending;  // The state's hidden pending table, where it holds an 'await'
    FieldDclNode *answer;   // The state's hidden Answer slot, where a message returning a value holds an 'await'
} ActorInfo;

// What the parser generated for one 'actor trait': the abstraction of an actor,
// behaviours only. Its name, used as a type, names a handle of two words, the
// mailbox of whichever actor it holds (counted, as an actor's handle's is) and
// the vtable of that actor's handle as the virtual trait 'vt' sees it, so that a
// call through it is one indirect call of the actor's own sending method
typedef struct ActorTraitInfo {
    StructNode *handle;     // What the trait's name names: the two-word handle
    StructNode *body;       // The trait as written, its behaviours as requirements
    StructNode *vt;         // The ordinary trait whose methods are the handle's: send, ask and future
    FnDclNode *wrap;        // Makes a handle from a virtual reference to an actor's handle
    Nodes *crossing;        // FnDclNode, VarDclNode pairs: each parameter that crosses to the actor
    ActorMessage *msgs;     // Its behaviours, in the order written; 'method' is the requirement in 'body'
    uint32_t nmsgs;
} ActorTraitInfo;

// Record an actor trait the parser generated
void actorTraitRegister(ActorTraitInfo *info);

// The actor trait whose handle 'type' is, or NULL
ActorTraitInfo *actorTraitOfHandle(INode *type);

// Is a value of type 'from' (a type declaration) an actor's handle, wanted
// where 'to' (a type declaration) is the handle of an actor trait? Then the
// conversion is the language's own (actorTraitCoerce), whether or not the actor
// conforms: it is judged there, where it can be said which behaviour is missing
int actorTraitWanted(INode *from, INode *to);

// Does this type meet the actor trait whose handle is 'traitHandle' (a type
// declaration, the handle or an instance of a generic one)? It does where it is an
// actor's handle that conforms, which is what a bound on a type parameter,
// '[S: ChunkSink]', asks; the generic body then calls the actor's own methods, and
// no table is made. Reports nothing
int actorTraitMeets(INode *type, INode *traitHandle);

// What the behaviour sent by the method 'fn' returns: the method's return, but for
// the future-giving method of an instance of a generic trait's handle (the
// message's 'method' is then the template's requirement, in its type parameters),
// where it is what the future holds
INode *actorMessageReturns(FnDclNode *fn);

// Convert the actor handle '*nodep' to the actor trait handle 'totype': the
// actor's conformance is checked, naming each behaviour it lacks, and the value
// becomes a call of the trait's 'wrap' on a borrow of the handle, which makes
// the trait's handle, a counted copy of the actor's mailbox with the table of
// the actor's sending methods that the virtual reference carries. 'pstate' is
// the type check state, NULL where a coercion has none; a value that is no
// place is lent as a temporary only with one. Answers whether the result has
// the type wanted
int actorTraitCoerce(TypeCheckState *pstate, INode **nodep, INode *totype);

// Record an actor the parser generated, filled in by the parser. Every
// parameter whose argument crosses to the actor -- each message's and each
// initializer's, 'self' aside -- is in 'crossing', as the state's own
// VarDclNodes, where the author wrote them
void actorRegister(ActorInfo *info);

// The actors package's functions a split method's generated code calls,
// found where the parser bound the package (parseactor.c)
enum ActorRuntimeFn {
    ActorRtParkReserve,     // parkReserve(p: &mut Pending, size: usize) u64
    ActorRtParked,          // parked(p: &mut Pending, id: u64) *u8
    ActorRtUnpark,          // unpark(p: &mut Pending, id: u64) *u8
    ActorRtRecordFree,      // recordFree(block: *u8)
    ActorRtAnswerTo,        // answerTo(slot: &mut Answer) *u8
    ActorRtAnswered,        // answered(slot: &mut Answer)
    ActorRtStartAwait,      // startAwait(a: *u8, rp: Reply): an Awaitable started at its seam
    ActorRtFutureReady,     // futureReady(fut: *u8) bool: a Future has its ending
    ActorRtFutureRegister,  // futureRegister(fut: *u8, rp: Reply) bool: park on it, unless it has
    ActorRtFutureOpen,      // futureOpen(fut: *u8, file: &Array[u8], line: u32) *u8: where its value is
    ActorRtFutureTaken,     // futureTaken(fut: *u8): a move value read out of it
    ActorRtParBlock,        // parBlock(size: usize) *u8: room for the copies a parallel each's pieces read
    ActorRtParSeam,         // parSeam(lo, hi, body, caps, resume, id, hasId): a behaviour's parallel each, cut at its end
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

// Is this the message enum of an actor, or an instance of a generic actor's? It
// declares Sendable on the strength of actorCheckAll, which judges each
// instance's parameters, so the thread check does not look at its type arguments
int actorIsMessage(INode *type);

// The actor whose state 'type' is, or NULL
ActorInfo *actorOfState(INode *type);

// The actor whose handle 'type' is, or NULL
ActorInfo *actorOfHandle(INode *type);

// Is this function the 'allocRegion' the compiler generated in an actor's state,
// which 'new self[T](...)' is a call of: the way to the actor's own region?
int actorIsAllocator(FnDclNode *fn);

// The actor whose handle 'fn' makes from its state ('selfactor'), or NULL
ActorInfo *actorOfSelfFn(FnDclNode *fn);

// The message a handle's method sends, and its actor, or NULL where 'send' is
// no handle's method that sends one
ActorMessage *actorMessageOfSend(FnDclNode *send, ActorInfo **info);

// Does the body of this method of an actor's state hold an 'await'?
int actorMethodAwaits(ActorInfo *info, FnDclNode *method);

// The actor whose behaviour 'fn' is, declared 'async fn', or NULL
ActorInfo *actorOfBehaviour(FnDclNode *fn);

// The message of actor 'info' whose behaviour is named 'name', or NULL
ActorMessage *actorMessageNamed(ActorInfo *info, Name *name);

// The member of an actor's state that 'name' names, where 'type' is the
// actor's handle and the handle has no such member of its own, or NULL. '*state'
// is set to the state. A use of it is a use of the actor's private state
INode *actorStateMember(INode *type, Name *name, StructNode **state);

#endif
