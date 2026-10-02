/** The Data Flow analysis pass whose purpose is to:

 * - Drop and free (or de-alias) variables at the end of their declared scope.
 * - Allow unique references to (conditionally) "escape" their current scope,
     thereby delaying when to drop and free/de-alias them.
 * - Track when copies (aliases) are made of a reference
 * - Ensure that lifetime-constrained borrowed references always outlive their containers.
 * - Deactivate variable bindings as a result of "move" semantics or
 *   for the lifetime of their borrowed references.
 * - Enforce reference (and variable) mutability and aliasing permissions
 * - Track whether every variable has been initialized and used
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef flow_h
#define flow_h

typedef struct VarDclNode VarDclNode;
typedef struct FnSigNode FnSigNode;

// Why a function would need a walk that follows borrows along each path: the
// gate, set as the walk meets each trigger; fnDclTypeCheck reads it, -V 2 counts it.
enum FlowGate {
    FlowGateHolder = 0x1,   // a local declared, assigned or swapped whose type carries a borrow
    FlowGateResult = 0x2,   // a value a scope hands out carrying a borrow, a bare borrowed reference too
    FlowGateStore  = 0x4,   // a call with a '&mut X' argument, X carrying a borrow, beside another argument carrying one
    FlowGateInCall = 0x8,   // a variable named while an operand's borrow of it waits for its call or literal
};

// How many operands' borrows the gate remembers waiting at once; past that,
// the function is gated
#define FlowInflightMax 8

// Context used across the data flow pass for a specific function/method
typedef struct FlowState {
    FnSigNode *fnsig;    // The type signature of the function we are within
    int16_t scope;      // Current block scope (2 = main block)
    uint16_t gate;      // FlowGate bits found so far
    uint16_t inflightcnt;   // How many of 'inflight' are in use
    uint8_t dropgate;   // 1: some variable's state may differ by path, so the path walk decides its drops
    uint8_t jumped;     // Set by blockFlow and ifFlow: every path through the block or 'if' jumped away
    VarDclNode *inflight[FlowInflightMax];  // The variable each waiting operand's borrow is of
} FlowState;

// The walk's conditional depth: how many 'if' arms, later 'elif' conditions,
// right operands of 'and' or 'or', and loop bodies enclose the node being
// walked. A variable records it as it is declared (VarDclNode.flowdepth); a
// tracked variable changed deeper than that may differ by path (flowDropNote).
extern uint16_t flowDepth;

// A function's walk begins: its depth, its state log
void flowFnBegin(FlowState *fstate);

// The main walk's variable flags change only through this, which logs the old
// ones so that an 'if' can walk each arm from the same state and join them
void flowVarSetFlags(VarDclNode *var, uint16_t flags, Nodes *hollowed);
uint32_t flowVarLogMark();
uint32_t flowVarLogPos();
// What one path changed since 'mark', and undoing it
typedef struct FlowVarPath FlowVarPath;
FlowVarPath *flowVarPathTake(uint32_t mark, FlowVarPath *next);
void flowVarRollback(uint32_t mark);
// Join the paths that went on after the fork -- 'npaths' of them, those that
// changed nothing left out of 'paths' -- into the state at the fork
void flowVarJoin(FlowVarPath *paths, uint32_t npaths);

// Is this variable one whose state the path walk follows for drop flags: a local
// or a parameter whose value moves or has something to do as it dies?
int flowDropTracked(VarDclNode *var);
// Is this type certainly one whose value has nothing to do as it dies, by a
// look at what it names: a number or void? Anything else is asked.
int flowNoDeath(INode *type);
// A tracked variable's state changed here: if deeper than its declaration, the
// function's drops are the path walk's to decide
void flowDropNote(FlowState *fstate, VarDclNode *var);
extern FlowState *flowCurrent;

// The variable at the root of an assignment target that is part of a local's
// own value -- a field, a tuple element, an array element, however deep -- or
// NULL when the target is reached through a reference or a pointer, or is the
// variable itself
VarDclNode *flowLvalRootVar(INode *lval);

// The variable that owns a variable's value: itself, or for a match's binding,
// the matched value's variable
VarDclNode *flowDropOwner(VarDclNode *var);

// Append to 'varlist' what the death of 'var' at a scope's end does, when it
// holds its whole value ('whole'), or is hollowed ('hollow', listing the moves
// that hollowed it beside 'extra'). 'dropat' positions a drop call. 'test' wraps
// each release in a DropFlagNode, so that it runs only when the variable's drop
// flag says it holds that.
void flowVarRelease(VarDclNode *var, INode *dropat, int whole, int hollow, Nodes *hollowed, Nodes *extra,
    int test, Nodes **varlist);

// Is this variable's value the one a scope hands back (exempting it from the
// scope's release)? 'hollow' gathers a part handed back out of what it owns.
int flowIsScopeResultOf(INode *retexp, VarDclNode *varnode, Nodes **hollow);

// Start the flow state for a function with this signature
void flowStateInit(FlowState *fstate, FnSigNode *fnsig);

// The gate's triggers are inline tests in flowgate.h, which read node types
// and so follow every node header; these are the questions they ask out of
// line, once the quick test says the answer may matter

// Set for -V 2, to ask every trigger and count each; otherwise the first one
// found settles the gate
extern int flowGateCountAll;

// A value a return, break or block end hands out has this type
void flowGateResultAsk(FlowState *fstate, INode *type);
// A call of two or more arguments has one that is a borrowed reference
void flowGateCallAsk(FlowState *fstate, Nodes *args);
// An operand just walked may be a borrow
void flowGateOperandAsk(FlowState *fstate, INode *operand);
#define flowGateOperandsEnd(fstate, mark) ((fstate)->inflightcnt = (mark))

// A variable is named while an operand's borrow waits: gate trigger when it is
// the one borrowed. Called only when 'inflightcnt' is not zero.
void flowGateUse(FlowState *fstate, VarDclNode *var);

// Tally a function's gate once its walk is done, and print the tallies (-V 2)
void flowGateCount(FlowState *fstate);
void flowGatePrint();

// Perform data flow analysis on a node whose value we intend to load
// At minimum, we check that it is a valid, readable value
// copyflag indicates whether value is to be copied or moved
// If copied, we may need to alias it. If moved, we may have to deactivate its source.
void flowLoadValue(FlowState *fstate, INode **nodep);

// Load a reference that a value is about to be read through, and refuse the
// read when the reference's permission grants none
void flowLoadThroughRef(FlowState *fstate, INode **refp);

// An initializer's 'self &new' (doc/reference/refinitdrop.html). It is reached
// only through itself -- '*self', a field, a method called on it -- and only
// once '*self = value' has filled it on every path; the init returns only once
// it is filled. 'flowThroughSelf' is set while a use through it is walked, so
// that nameuseFlow refuses every other use, which would let it escape.
// The variable an expression names when it is an init's 'self', or NULL
VarDclNode *flowNewSelf(INode *node);
extern int flowThroughSelf;
// Walk a use through 'self' (its node at 'selfp'), refused before it is filled
void flowNewSelfThrough(FlowState *fstate, INode **selfp);
// '*self = value' in an init: the store that fills it, when it is not filled
// yet. Returns 1 when 'lval' is that store's target, which then holds no value
// to finalize.
int flowNewSelfFill(INode *lval);
// The init returns here: refused unless 'self' is filled
void flowNewSelfReturn(FlowState *fstate, INode *at);

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode);

// Start a new scope
size_t flowScopePush();

// Create de-alias list of all own/Rc reference variables, except var found in retexp
// 'lexnode' positions an injected drop call where there is no retexp to position it on
void flowScopeDealias(size_t pos, Nodes **varlist, INode *retexp, INode *lexnode);
// Back out of current scope
void flowScopePop(size_t pos);

// Reference-count node: wraps an expression that yields a counted reference and
// adds 'amt' holders to its count when evaluated. Injected by flow analysis,
// never parsed; generation lowers it to the count adjustment.
typedef struct {
    IExpNodeHdr;
    INode *exp;
    int16_t *counts;   // points to array of counts. NULL if not tuple
    int16_t amt;       // count nbr if not a tuple, # of counts if tuple
} RefCountNode;

// Hollow release: 'var' holds a sole owning reference whose referent, or an
// element of it, was moved out, and this releases it without what moved.
// 'moved' holds each move-source expression that took one out; walked inwards, each
// reaches 'var'. Injected by flow analysis, never parsed: into a scope's
// release list with 'exp' NULL, and around the value a reassignment stores into
// such a variable, where the old value is released after 'exp' is evaluated,
// just as genlStore releases a whole one.
typedef struct {
    IExpNodeHdr;
    INode *exp;
    VarDclNode *var;
    Nodes *moved;
    uint8_t test;       // 1: release only when the variable's drop flag says it is hollow
} HollowNode;

// A release that runs only when 'var''s drop flag holds 'state' (a
// DropFlagState): a variable whose state differs by path, at a scope's end.
// Injected into a release list by the path walk's drop-flag client
// (flowdrop.c), never parsed.
typedef struct {
    IExpNodeHdr;
    INode *release;
    VarDclNode *var;
    uint8_t state;
} DropFlagNode;

// A temporary: the value of an expression nothing takes -- not bound to a
// variable, stored, passed by value, handed back or moved -- whose death does
// something (itypeNeedsFinal). Injected by flow analysis round the expression,
// at the place its value is read or thrown away (flowTempRead); generation
// keeps the value in a slot of its own and finalizes it, newest first, at the
// end of the statement that made it, or of the 'if' or 'while' condition, or
// of the right operand of 'and' or 'or', that made it.
// A borrow of it may not be used once it is gone: the loan walk roots a borrow
// of it in 'walkvar', a stand-in variable that ends where it dies
// (flowpath.c), so a variable holding the borrow and used later is refused.
// 'moved' holds each move-source expression that took its referent, or an
// element of it, out through it, as a HollowNode's does: it is released
// hollow. 'kept' says it is never finalized: a pointer into it may outlive
// its statement (flowTempEscape), or a value moved out of it by value left it
// with a hole, as a local array is left when an element moves out of it.
typedef struct {
    IExpNodeHdr;
    INode *exp;
    Nodes *moved;
    VarDclNode *walkvar;
    uint8_t kept;
} TempNode;

// Wrap the expression at 'nodep', whose value is read or thrown away here, in
// a TempNode when it is a temporary whose death does something
void flowTempRead(INode **nodep);
// How many temporaries flow has made: a statement that made none needs no walk
extern uint32_t flowTempCount;
// The walk over a statement that made temporaries, once flow has walked it:
// each temporary that a pointer going out of the statement may point into is
// kept. 'out' says the value of 'node' goes out of the statement: stored, or
// handed back.
void flowTempEscape(INode *node, int out);

// What a drop flag holds at run time: which value, if any, its variable holds
enum DropFlagState {
    DropFlagEmpty = 0,  // nothing: never given a value, or moved out
    DropFlagWhole = 1,  // its whole value
    DropFlagHollow = 2, // a sole owner some of whose referent moved out
};

// The local variable holding an owning reference that 'ref' names, or NULL
VarDclNode *flowOwningLocal(INode *ref);

// The matched value a match binds this variable to, or NULL: such a variable
// is the matched value under its variant's name, and owns nothing itself
INode *flowMatchBound(INode *var);
// Does a match's binding name the matched value's own storage (a binding by value)?
int flowMatchInPlace(VarDclNode *var);

// A hollow release of a hollowed variable, for the moves that hollowed it so far
HollowNode *flowNewHollow(VarDclNode *var);

// Handle when moving or copying a value to a new destination
void flowHandleMoveOrCopy(INode **nodep);

// Deactivate the source of a value moved to a new holder (or say the move is illegal)
void flowHandleMove(INode *node);

// Refuse a move-typed result a scope hands back when its source does not own it
void flowResultMove(INode *node);

// Does this expression still hold its value after it is read?
int flowIsLvalRead(INode *node);

// Does this cast hand on what its operand holds: a recast, or a conversion
// into an owning virtual reference, which carries the operand's owner?
int flowCastCarries(INode *cast);

// If needed, inject a reference-count node for Rc references, adjusting the count by amt
void flowInjectRefCountAmt(INode **nodep, int16_t amt);

// Is this type a counted (Rc) reference, single or virtual?
int flowIsRcRef(INode *type);

// Does a copy of a value of this type -- a struct, an enum, a tuple, an array --
// add a holder to a counted reference its death releases? And does this
// variant hold one its enum's drop releases?
int flowHeldCounted(INode *type);
int flowVariantHeldCounted(INode *variant);

// Is this type an owning reference into a region, single or virtual, or a
// tuple carrying one: what a store releases before it overwrites?
int flowIsOwningType(INode *type);

#endif
