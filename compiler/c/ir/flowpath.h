/** The path walk: flow state along each path, with joins
 * @file
 *
 * The existing flow walk (flow.c) keeps one running summary per function. This
 * walk keeps state per program point and per path: it forks at each 'if' arm and
 * each 'and'/'or' right operand, joins the arms by taking the union of what each
 * changed, deposits the state at a 'break' or 'continue' with the block it names,
 * and walks a loop body again until the state at its head stops growing. It is
 * read-only: it injects nothing and changes no node, so it may walk a loop body
 * twice. It runs after blockFlow, only on a function the gate marked
 * (FlowState.gate), and only when blockFlow reported no error.
 *
 * Its one client so far is borrow freezing (flowloan.c): the facts it keeps per
 * variable are the loans the variable may hold and the pending conflicts that
 * fire if the variable is used again. compiler/c/doc/phases/flow.md, "The loan
 * walk", is the note.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef flowpath_h
#define flowpath_h

typedef struct FnDclNode FnDclNode;
typedef struct BlockNode BlockNode;

// A set of ids -- loans, or pending conflicts -- kept sorted and never changed
// once built, so that a path's state and the undo log can share one. NULL is the
// empty set; pathSetAll is every loan (a loop that would not settle, 3.3).
typedef struct PathSet {
    uint32_t cnt;
    uint32_t ids[];
} PathSet;
extern PathSet pathSetAll;

PathSet *pathSetAdd(PathSet *set, uint32_t id);
PathSet *pathSetUnion(PathSet *a, PathSet *b);
int pathSetHas(PathSet *set, uint32_t id);

// A place: somewhere a value lives. A root variable, or what the root variable
// (a borrowed reference) points at, and a path of steps from it. Two places
// overlap when they share a root and one path is a prefix of the other, step by
// step: only two different fields are disjoint.
#define PlaceMaxSteps 6
typedef struct {
    uint32_t var;       // the root variable's index in the walk
    uint8_t deref;      // 1: the root is what that variable, a borrowed reference, points at
    uint8_t nsteps;     // A path longer than PlaceMaxSteps is cut short, which only overlaps more
    uintptr_t steps[PlaceMaxSteps];
} Place;
// A step is a field's name (a Name pointer, so even), a tuple element's index
// ((n << 2) | 2), an element of an array (any index: all overlap), or a
// dereference of an owning reference
#define PlaceStepElem  ((uintptr_t)1)
#define PlaceStepDeref ((uintptr_t)3)

// What an expression does to a place
enum PathAccess {
    AccessRead,         // a copy out
    AccessBorrow,       // a read-only borrow ('&', '&imm', '&ro')
    AccessBorrowMut,    // a borrow that may write ('&mut', '&uni', '&mut1')
    AccessBorrowOpaq,   // an '&opaq' borrow: its address only
    AccessWrite,        // a store into it, or an operator that changes it in place
    AccessMove,         // a move-typed value taken to a new holder
    AccessReplace,      // the whole root stored over, its old value released or finalized
    AccessEnd,          // the root leaves its scope
};

// One variable the walk has met, and its facts on the current path
typedef struct {
    VarDclNode *var;
    PathSet *holds;     // the loans it may hold here
    PathSet *pending;   // the pending conflicts a use of it fires
    uint32_t loans;     // the first loan rooted at it (flowloan.c), 0 for none
    uint32_t xstamp;    // scratch: gathering a delta
    uint32_t jstamp;    // scratch: a join
    uint32_t jcnt;
    PathSet *jholds;
    PathSet *jpending;
    uint8_t holder;     // its type carries a borrow, so it may hold a loan
} PathVar;

// The variables the current walk has met, by index; index 0 is unused
extern PathVar *pathVars;

// Change a variable's facts on the current path, recording the old ones so a
// fork can undo them
void pathSetFacts(uint32_t var, PathSet *holds, PathSet *pending);

// Grow a buffer the walk keeps from one function to the next: twice the room,
// from the compiler's arena (a buffer freshly grown from the system costs a
// compile more, in first touches, than the walk does)
void *pathGrow(void *buf, uint32_t *cap, size_t size);

// Walk a function's body (after blockFlow), if the gate marked it
void flowPathWalk(FnDclNode *fndcl);

// Print the walk's tallies for -V 2
void flowPathPrint();

#endif
