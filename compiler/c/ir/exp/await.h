/** Handling for 'await' nodes
 *
 * 'await x', in an actor's method, waits for x: the method is cut there, a
 * seam, where it returns to the actor's dispatcher, and what it needs after
 * the seam waits in a continuation until x is answered. Type check places an
 * 'await', and the loan walk applies the seam's rules to it (flowpath.c,
 * pwSeam), noting on it what each variable in scope does there. A message's
 * seams every rule accepts are split (awaitSplitOrReport): generation makes
 * the method's first half and a second half for each seam, which takes the
 * seam's record (genllvm/genlawait.c).
 *
 * What is awaited is a message to an actor -- one of its handle's methods
 * sending a message that returns a value. Type check sends it awaited
 * instead: the handle's second method for it, which carries a reply's
 * envelope (AwaitReplyNode) in the message. The seam parks its record in
 * the actor's pending table, and the reply's dispatch calls the second half
 * with it and the value returned. Any other 'await' is reported unbuilt
 * (ErrorUnbuiltAwait), naming what its continuation would carry, unless
 * '--await-direct', for tests, hands its record straight to its second half.
 * In a method that is not a message, every 'await' is reported unbuilt.
 * compiler/c/doc/phases/flow.md, "A seam", and
 * compiler/c/doc/phases/generation.md, "A split method" and "A message's
 * reply", are the notes.
 *
 * 'selfactor', in an actor's method, is the actor's own handle: type check
 * lowers it to a call of the function the actor's declaration generated for
 * it (SelfActorNode).
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
    FnDclNode *message; // What is awaited is this message of an actor, sent awaited; NULL for anything else
    uint8_t voidmessage;    // What is awaited is a message that returns nothing, which is not built
    uint8_t walked;     // The loan walk reached it on some path
} AwaitNode;

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

// Clone await
INode *cloneAwaitNode(CloneState *cstate, AwaitNode *node);

void awaitPrint(AwaitNode *node);

// Name resolution of await
void awaitNameRes(NameResState *pstate, AwaitNode *node);

// Type check await: it stands in an actor's method, and its value is what it
// awaits. A message it awaits is sent awaited, and the 'await''s value is
// what the message returns
void awaitTypeCheck(TypeCheckState *pstate, AwaitNode *node, INode *expectType);

// Type check 'selfactor': it stands in an actor's method, and becomes the
// call that makes the actor's handle from its state
void selfActorTypeCheck(TypeCheckState *pstate, INode **nodep);

// A call of 'callee', a method selected for it: refused where the callee is a
// method of an actor holding an 'await' and the caller is not its dispatcher,
// since the callee's seams would cut the caller, where nothing shows it
void awaitCallCheck(TypeCheckState *pstate, INode *call, FnDclNode *callee);

// Each seam of a function every rule accepted: split, where the function is a
// message of an actor, each of its seams awaits a message (or '--await-direct'
// is given), and no seam stands where splitting is not built; otherwise each
// is reported not built yet (ErrorUnbuiltAwait), the message saying what its
// continuation would carry
void awaitSplitOrReport(FnDclNode *fn, Nodes *awaits);

// The seams of a split function, in the order written, or NULL where it is not split
Nodes *awaitSplitOf(FnDclNode *fn);

// Whether an 'await' stands anywhere inside 'node' (not inside a function it declares)
int awaitWithin(INode *node);

// Whether a type is a lock's guard's: a reference whose permission says it
// holds its lock (permHeldKind), which a seam gives back
int awaitIsGuardType(INode *type);

#endif
