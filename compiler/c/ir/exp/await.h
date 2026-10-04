/** Handling for 'await' nodes
 *
 * 'await x', in an actor's method, waits for x: the method is cut there, a
 * seam, where it returns to the actor's dispatcher, and what it needs after
 * the seam waits in a continuation until x is answered. Type check places an
 * 'await', and the loan walk applies the seam's rules to it (flowpath.c,
 * pwSeam), noting on it what each variable in scope does there. A message's
 * seams every rule accepts are split (awaitSplitOrReport): generation makes
 * the method's first half and a second half for each seam, which takes the
 * seam's record (genllvm/genlawait.c). Nothing yet waits for a reply, so the
 * split is generated only under '--await-direct', which hands each record
 * straight to its second half; otherwise, and in a method that is not a
 * message, an 'await' is reported unbuilt (ErrorUnbuiltAwait), naming what
 * its continuation would carry. compiler/c/doc/phases/flow.md, "A seam", and
 * compiler/c/doc/phases/generation.md, "A split method", are the notes.
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
    uint8_t walked;     // The loan walk reached it on some path
} AwaitNode;

// Test only ('--await-direct'): a split message's seam hands its record
// straight to its second half, with the value it awaited as the result,
// rather than waiting for a reply, which nothing builds yet. Without it no
// method is split, and every 'await' is reported unbuilt
extern int awaitDirect;

AwaitNode *newAwaitNode();

// Clone await
INode *cloneAwaitNode(CloneState *cstate, AwaitNode *node);

void awaitPrint(AwaitNode *node);

// Name resolution of await
void awaitNameRes(NameResState *pstate, AwaitNode *node);

// Type check await: it stands in an actor's method, and its value is what it awaits
void awaitTypeCheck(TypeCheckState *pstate, AwaitNode *node, INode *expectType);

// Each seam of a function every rule accepted: split, where the function is a
// message of an actor and '--await-direct' is given and no seam stands where
// splitting is not built; otherwise each is reported not built yet
// (ErrorUnbuiltAwait), the message saying what its continuation would carry
void awaitSplitOrReport(FnDclNode *fn, Nodes *awaits);

// The seams of a split function, in the order written, or NULL where it is not split
Nodes *awaitSplitOf(FnDclNode *fn);

// Whether an 'await' stands anywhere inside 'node' (not inside a function it declares)
int awaitWithin(INode *node);

// Whether a type is a lock's guard's: a reference whose permission says it
// holds its lock (permHeldKind), which a seam gives back
int awaitIsGuardType(INode *type);

#endif
