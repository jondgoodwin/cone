/** Handling for 'await' nodes
 *
 * 'await x', in an actor's method, waits for x: the method is cut there, a
 * seam, where it returns to the actor's dispatcher, and what it needs after
 * the seam waits in a continuation until x is answered. The continuation is
 * not built yet: type check places an 'await', the loan walk applies the
 * seam's rules to it (flowpath.c, pwSeam), and an 'await' every rule accepts
 * is reported unbuilt (awaitReportUnbuilt), naming what its continuation would
 * carry. compiler/c/doc/phases/flow.md, "A seam", is the note.
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
    uint8_t walked;     // The loan walk reached it on some path
} AwaitNode;

AwaitNode *newAwaitNode();

// Clone await
INode *cloneAwaitNode(CloneState *cstate, AwaitNode *node);

void awaitPrint(AwaitNode *node);

// Name resolution of await
void awaitNameRes(NameResState *pstate, AwaitNode *node);

// Type check await: it stands in an actor's method, and its value is what it awaits
void awaitTypeCheck(TypeCheckState *pstate, AwaitNode *node, INode *expectType);

// Report each seam of a function every rule accepted as not built yet
// (ErrorUnbuiltAwait), saying what its continuation would carry
void awaitReportUnbuilt(Nodes *awaits);

#endif
