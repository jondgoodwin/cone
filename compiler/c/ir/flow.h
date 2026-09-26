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
typedef struct FnCallNode FnCallNode;

// Why a function would need a walk that follows borrows along each path: the
// gate, set as the walk meets each trigger. Nothing reads it yet; -V 2 counts it.
enum FlowGate {
    FlowGateHolder = 0x1,   // a local declared, assigned or swapped whose type carries a borrow
    FlowGateResult = 0x2,   // a value a scope hands out carrying a borrow, not as a bare borrowed reference
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
    VarDclNode *inflight[FlowInflightMax];  // The variable each waiting operand's borrow is of
} FlowState;

// Start the flow state for a function with this signature
void flowStateInit(FlowState *fstate, FnSigNode *fnsig);

// Gate trigger: a local declared, assigned or swapped with this type
void flowGateHolder(FlowState *fstate, INode *type);

// Gate trigger: a value a return, break or block end hands out
void flowGateResult(FlowState *fstate, INode *exp);

// Gate trigger: a call storing through a '&mut X' argument beside another borrow
void flowGateCall(FlowState *fstate, FnCallNode *node);

// An operand of a call or a literal was just walked: while the rest are, a
// borrow it makes waits, and the variable it borrows is remembered
void flowGateOperand(FlowState *fstate, INode *operand);
#define flowGateOperandsEnd(fstate, mark) ((fstate)->inflightcnt = (mark))

// A variable is named while an operand's borrow waits: gate trigger when it is
// the one borrowed. Called only when 'inflightcnt' is not zero.
void flowGateUse(FlowState *fstate, VarDclNode *var);

// Tally a function's gate once its walk is done, and print the tallies (-V 2)
void flowGateCount(FlowState *fstate);
void flowGatePrint();

// Set for -V 2, to ask every trigger and count each; otherwise the first one
// found settles the gate
extern int flowGateCountAll;

// Perform data flow analysis on a node whose value we intend to load
// At minimum, we check that it is a valid, readable value
// copyflag indicates whether value is to be copied or moved
// If copied, we may need to alias it. If moved, we may have to deactivate its source.
void flowLoadValue(FlowState *fstate, INode **nodep);

// Load a reference that a value is about to be read through, and refuse the
// read when the reference's permission grants none
void flowLoadThroughRef(FlowState *fstate, INode **refp);

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode);

// Start a new scope
size_t flowScopePush();

// Create de-alias list of all own/rc reference variables, except var found in retexp
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
} HollowNode;

// The local variable holding an owning reference that 'ref' names, or NULL
VarDclNode *flowOwningLocal(INode *ref);

// A hollow release of a hollowed variable, for the moves that hollowed it so far
HollowNode *flowNewHollow(VarDclNode *var);

// Handle when moving or copying a value to a new destination
void flowHandleMoveOrCopy(INode **nodep);

// Refuse a move-typed result a scope hands back when its source does not own it
void flowResultMove(INode *node);

// Does this expression still hold its value after it is read?
int flowIsLvalRead(INode *node);

// If needed, inject a reference-count node for rc references, adjusting the count by amt
void flowInjectRefCountAmt(INode **nodep, int16_t amt);

// Is this type a counted (rc) reference, single or slice?
int flowIsRcRef(INode *type);

// Does a variable of this type hold something its scope must release:
// an rc or so reference, single or slice, or a tuple carrying one?
int flowIsOwningType(INode *type);

#endif
