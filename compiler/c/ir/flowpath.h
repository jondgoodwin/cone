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
// Does a loan set hold the loan 'loan', near or far, with any tag (flowloan.h)?
int pathSetHasLoan(PathSet *set, uint32_t loan);

// A place: somewhere a value lives. A root variable, or what the root variable
// (a borrowed reference) points at, and a path of steps from it. Two places
// overlap when they share a root and one path is a prefix of the other, step by
// step: only two different fields are disjoint.
//
// A place is reached through a shared path when a reference on the way to it
// -- the borrowed reference its root is read through, or an owning reference
// a step dereferences -- may alias ('mut', 'ro', 'imm', 'opaq', 'mut1'), so
// that other references may reach it too. A place reached through none is
// reached as 'uni': a local, or through 'uni' references only.
#define PlaceMaxSteps 6
typedef struct {
    uint32_t var;       // the root variable's index in the walk
    uint8_t deref;      // 1: the root is what that variable, a borrowed reference, points at
    uint8_t nsteps;     // A path longer than PlaceMaxSteps is cut short, which only overlaps more
    uint8_t shared;     // 1: reached through a shared path
    uint8_t sharedlen;  // then, how many steps lead to the first reference that may alias (0: the root's)
    uint8_t shwrite;    // 1: some reference on the way, other than 'imm', may alias, so that another
                        // holder may change what the place is in (a container's block may move)
    uint8_t owned;      // 1: a step dereferences an owning reference others may own too ('Rc'), so
                        // the place may outlive its root variable
    uint8_t far;        // deref: the reference was itself read through a borrowed one ('**pp',
                        // '*r.g'), so the root stands for anything a borrow or more past where
                        // the variable points (flowloan.h, LoanFar)
    uintptr_t steps[PlaceMaxSteps];
    INode *use;         // the name use of the root variable, where a use of it is reported
    INode *referent;    // deref: the type the root variable, read itself as the reference, points
                        // at; NULL when the reference was read from a part of it
    StructNode *slotted; // A struct declaring lifetimes whose field the path's first step is, where
                        // the root variable's loans are tagged by its slots (flowloan.h); else NULL
    uint32_t slots;     // then, the slots of that field (lifeFieldSlots)
} Place;
// A step is a field's name (a Name pointer, so even), a tuple element's index
// ((n << 2) | 2), an element of an array (any index: all overlap), or a
// dereference of an owning reference
#define PlaceStepElem  ((uintptr_t)1)
#define PlaceStepDeref ((uintptr_t)3)

// What an expression does to a place
enum PathAccess {
    AccessRead,         // a copy out
    AccessBorrow,       // a read-only borrow others may change under ('&', '&ro')
    AccessBorrowImm,    // a borrow promising the value never changes ('&imm')
    AccessBorrowMut,    // a borrow that may write, and may alias ('&mut', '&mut1')
    AccessBorrowUni,    // a borrow that may write, the only one ('&uni')
    AccessBorrowOpaq,   // an '&opaq' borrow: its address only
    AccessWrite,        // a store into it, or an operator that changes it in place
    AccessMove,         // a move-typed value taken to a new holder
    AccessReplace,      // the whole root stored over, its old value released or finalized
    AccessEnd,          // the root leaves its scope
    AccessSeam,         // a seam ('await'): every borrow that is not global ends there
};

// What a variable the drop-flag client tracks may hold on the paths reaching a
// point: a set of these bits (flowdrop.c). 0 is a variable not in scope.
enum DropState {
    DropWhole = 0x1,    // its whole value
    DropHollow = 0x2,   // a sole owner some of whose referent moved out
    DropUninit = 0x4,   // nothing: never given a value
    DropMoved = 0x8,    // nothing: moved out
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
    PathSet *jfirst;    // scratch: a join on a GPU target, what the first path gives it
    uint32_t jla;       // scratch: a join on a GPU target, a loan of each of two paths that differ
    uint32_t jlb;
    uint8_t japart;     // scratch: a join on a GPU target, two paths gave it different places
    uint8_t holder;    // its type carries a borrow, so it may hold a loan
    uint8_t temp;       // a temporary's stand-in (TempNode.walkvar), ending with its statement
    uint8_t state;     // drop-flag client: what it may hold here (DropState bits)
    uint8_t jstate;     // scratch: a join
    uint8_t tracked;    // drop-flag client: its state is followed (flowDropTracked)
    uint8_t dies;       // drop-flag client: it has something to do as it dies (itypeNeedsFinal)
    uint8_t flagged;    // drop-flag client: its state differs by path at a release
    uint8_t initing;    // its initializer is being walked: it holds no value yet
} PathVar;

// The variables the current walk has met, by index; index 0 is unused
extern PathVar *pathVars;

// Change a variable's facts on the current path, recording the old ones so a
// fork can undo them
void pathSetFacts(uint32_t var, PathSet *holds, PathSet *pending);
// The same for its drop state
void pathSetState(uint32_t var, uint8_t state);

// Grow a buffer the walk keeps from one function to the next: twice the room,
// from the compiler's arena (a buffer freshly grown from the system costs a
// compile more, in first touches, than the walk does)
void *pathGrow(void *buf, uint32_t *cap, size_t size);

// Walk a function's body (after blockFlow), if the gate marked it: with borrow
// freezing ('loans') for the loan gate, with drop flags ('drops') for the drop
// gate (FlowState.dropgate), or both in one walk. 'seams' says it holds an
// 'await', so its operands are walked in the order a seam gives them
// (awaitOrder)
void flowPathWalk(FnDclNode *fndcl, int loans, int drops, int seams);

// Print the walk's tallies for -V 2
void flowPathPrint();

// A variable in scope at the seam 'seam' is used after it, before it is
// stored over whole: what it holds there is needed past the seam (flowloan.c,
// a seam's live mark fired)
void pathSeamLive(INode *seam, uint32_t var);

#endif
