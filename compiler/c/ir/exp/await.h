/** Handling for 'await' nodes
 *
 * 'await x', in an actor's behaviour (a method declared 'async do'), waits
 * for x: the behaviour is cut there, a seam, where it returns to the actor's
 * dispatcher, and what it needs after the seam waits in a continuation until
 * x is answered. Type check places an 'await' -- in a behaviour, never in a
 * synchronous 'fn', which no dispatcher runs as a message -- and the loan walk
 * applies the seam's rules to it (flowpath.c, pwSeam), noting on it what each
 * variable in scope does there. A behaviour's seams every rule accepts are
 * split (awaitSplitOrReport): generation makes the behaviour's first half and
 * a second half for each seam, which takes the seam's record
 * (genllvm/genlawait.c).
 *
 * What is awaited is a behaviour of an actor -- one of its handle's methods
 * sending one that returns a value. Type check sends it awaited instead: the
 * handle's second method for it, which carries a reply's envelope
 * (AwaitReplyNode) in the message. The seam parks its record in the actor's
 * pending table, and the reply's dispatch calls the second half with it and
 * the value returned. A behaviour that returns nothing sends no reply, so an
 * 'await' on one is refused (ErrorAwaitVoid). What is awaited may instead be
 * an operation, a value of the actors package's Awaitable[R] (an I/O
 * operation of the aio package's): the seam starts it, handing it a reply's
 * envelope as a request carries one, and its answer, an R, is the 'await''s
 * value; an 'await' on one whose value is unwanted is warned
 * (WarnAwaitUnused). Or what is awaited is a future, a value of the actors
 * package's Future[T], which a call of a behaviour returning a T gives where
 * its value is used (actorFutureCall): the seam parks on it unless it has
 * its ending already, when the behaviour goes on at once, and the 'await''s
 * value is the T opened out of it. An 'await' on anything else waits for no
 * answer, and is refused (ErrorAwaitNotFuture), naming what its continuation
 * would carry, unless '--await-direct', for tests, hands its record straight
 * to its second half. compiler/c/doc/phases/flow.md, "A seam", and
 * compiler/c/doc/phases/generation.md, "A split method" and "A message's
 * reply", are the notes.
 *
 * 'selfactor', in an actor's method, is the actor's own handle: type check
 * lowers it to a call of the function the actor's declaration generated for
 * it (SelfActorNode). 'self.m()', where m is one of the actor's behaviours, is
 * a send through that handle, as 'selfactor.m()' is (selfActorSend).
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef await_h
#define await_h

// What the loan walk found of one variable in scope at a seam, on any path
// reaching it (several walks of a loop's body gather into one)
enum SeamFlags {
    SeamOpen = 0x01,    // it holds its value (a variable never given one, or moved out, does not)
    SeamLive = 0x02,    // it is used after the seam before it is stored over whole
    SeamDies = 0x04,    // its death does something (itypeNeedsFinal): it is droppable
    SeamGuard = 0x08,   // a lock's guard, the lock a borrow through a lock permission holds
    SeamEnds = 0x10,    // it holds a borrow that is not global, which ends at the seam
    SeamParm = 0x20,    // a parameter: it dies as the function ends
    SeamTemp = 0x40,    // a temporary of the statement the seam is in
};

typedef struct SeamVar {
    VarDclNode *var;
    uint8_t flags;      // SeamFlags
} SeamVar;

// 'await exp'
typedef struct AwaitNode {
    IExpNodeHdr;
    INode *exp;         // What is awaited: evaluated before the seam
    SeamVar *seamvars;  // The variables in scope at the seam, in the order they were declared:
                        // parameters, then locals, then the statement's temporaries
    uint32_t nseamvars;
    uint32_t seamcap;
    uint32_t seamno;    // A split method's seams are numbered from 1 in the order written, which names each second half
    struct GenSeam *genseam;    // Generation: its record and its second half (genlawait.c), made by the first half to reach it
    FnDclNode *message; // What is awaited is this behaviour of an actor, sent awaited; NULL for anything else
    INode *awaitable;   // Or what is awaited is an operation, of this instance of actors.Awaitable[R],
                        // started at the seam; NULL for anything else
    INode *future;      // Or what is awaited is a future, of this instance of actors.Future[T]:
                        // parked on unless it has its ending already; NULL for anything else
    uint8_t par;        // Or it is the end of a 'parallel each' written in a behaviour (pareach.c):
                        // what it waits for is the loop's last piece, and it follows the loop
    uint8_t walked;     // The loan walk reached it on some path
    struct GenParSeam *genpar;  // Generation: the loop's range, piece and copies, made where the loop is generated (genlpar.c)
} AwaitNode;

// Does this seam wait for an answer -- a message's reply, an operation's, a
// future's ending, a parallel loop's last piece -- so that its record parks in
// the actor's pending table?
#define awaitParks(node) ((node)->message || (node)->awaitable || (node)->future || (node)->par)

// The envelope of the request a message 'await' sends: the last argument of
// the handle's method that sends it awaited. Generation makes it where the
// seam's record is laid out and given its id (genlAwaitReply)
typedef struct AwaitReplyNode {
    IExpNodeHdr;
    AwaitNode *await;   // The seam whose reply it is
} AwaitReplyNode;

// 'selfactor', before type check lowers it
typedef struct SelfActorNode {
    IExpNodeHdr;
} SelfActorNode;

// Test only ('--await-direct'): a split message's seam that awaits anything
// but a message hands its record straight to its second half, with the value
// it awaited as the result. Without it such a seam is reported unbuilt
extern int awaitDirect;

AwaitNode *newAwaitNode();
SelfActorNode *newSelfActorNode();

// The seam at the end of a 'parallel each' written in a behaviour: a statement
// following the loop, which cuts the behaviour there (pareach.c builds it)
AwaitNode *awaitParNew(TypeCheckState *pstate, INode *lexnode);

// Clone await
INode *cloneAwaitNode(CloneState *cstate, AwaitNode *node);

void awaitPrint(AwaitNode *node);

// Name resolution of await
void awaitNameRes(NameResState *pstate, AwaitNode *node);

// Type check await: it stands in an actor's behaviour, and its value is what
// it awaits. A behaviour it awaits is sent awaited, and the 'await''s value is
// what the behaviour returns
void awaitTypeCheck(TypeCheckState *pstate, AwaitNode *node, INode *expectType);

// Type check 'selfactor': it stands in an actor's method, and becomes the
// call that makes the actor's handle from its state
void selfActorTypeCheck(TypeCheckState *pstate, INode **nodep);

// A method call on an actor's state, 'objdereftype', whose receiver is the
// call's objfn: where the method named is one of the actor's behaviours and
// the caller is not its dispatcher, the call is a send, and its receiver
// becomes the actor's handle, made from the state as 'selfactor' is.
// Answers whether the receiver was replaced
int selfActorSend(TypeCheckState *pstate, FnCallNode *call, INode *objdereftype);

// Each seam of a function every rule accepted: split, where the function is a
// behaviour of an actor, each of its seams awaits one (or '--await-direct'
// is given), and no seam stands where splitting is not built; otherwise each
// is reported not built yet (ErrorUnbuiltAwait), the message saying what its
// continuation would carry
void awaitSplitOrReport(FnDclNode *fn, Nodes *awaits);

// The seams of a split function, in the order written, or NULL where it is not split
Nodes *awaitSplitOf(FnDclNode *fn);

// Whether an 'await' stands anywhere inside 'node' (not inside a function it declares)
int awaitWithin(INode *node);

// Where a seam cuts inside a statement, the rest of the statement runs after
// it: 'await e' is 't = e; await t', and what is still to be done once t is
// made is the second half's first statement. A value computed to the left of
// the 'await' in the order of evaluation is made before the seam and travels
// in the record ('two(x, await f())'). A place, or a borrow of one, written to
// its left is reached again after the seam instead -- 'self.store(await f())',
// 'items[await i] = 7', 'take(&x, await y)' -- where it is a plain path: a
// variable ('self' among them), a field, a dereference or an index of a plain
// path, the index itself a plain path or a literal; or the actor's own handle
// made from one, as 'self.m()' sends m through ('Page.self'(self)'). Neither
// reaches anything a call makes, so reaching it after the seam does nothing a
// reader could see but find what is there then. A borrow a call made, or a
// place reached through one, cannot be made again, and is refused
// (ErrorAwaitLeftCall): flowloan.c, loanSeamFlight; awaitWalk here.
//
// Is 'node' a plain path?
int awaitIsPath(INode *node);

// Is 'node', an operand standing before a seam held by a later operand of the
// same list, made only after that seam: a plain path whose value is a borrowed
// reference -- a receiver, or a borrow written to the left of the 'await'?
int awaitReReached(INode *node);

// The order a list of operands is evaluated in, as indexes into 'nodes',
// written into 'order', which holds nodes->used of them: the order written,
// but that each operand re-reached after the last of them holding a seam
// (awaitReReached) comes just after it. Answers how many operands lead the
// order up to and including that last one: each before it is made before its
// seam and is in flight across it. 0 where no operand holds a seam. Flow and
// generation both take an operand list in this order
uint32_t awaitOrder(Nodes *nodes, uint32_t *order);

// A place indexed more than once -- 'a[i][j]', 'a[i].f[j]', a slice 'a[i][lo..<hi]'
// -- reaches the array each index names after every seam in the indexes, which
// are made first, in the order written, each kept in flight across the seams
// that follow it. The chain of a place is the indexes it passes through
// (ArrIndex), by way of fields, dereferences, casts and borrows, down to its
// root. Answers the indexes, innermost first, from the innermost one whose
// arguments hold a seam outward (the ones inside it are plain paths, reached
// after the seam as any are), or NULL where no index in the chain holds one;
// '*rootseam' says whether the chain's root holds a seam of its own, which
// cannot be made in that order
Nodes *awaitChainLevels(INode *place, int *rootseam);

// Refuse 'left', written to the left of a seam in its statement and used
// after it, which a call or a temporary made (ErrorAwaitLeftCall). 'what'
// names it, capitalized: "This borrow", "This place's base"
void awaitLeftCallMsg(INode *left, char *what);

// Whether a type is a lock's guard's: a reference whose permission says it
// holds its lock (permHeldKind), which a seam gives back
int awaitIsGuardType(INode *type);

#endif
