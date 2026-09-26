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

// Context used across the data flow pass for a specific function/method
typedef struct FlowState {
    FnSigNode *fnsig;    // The type signature of the function we are within
    int16_t scope;      // Current block scope (2 = main block)
} FlowState;

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

// The matched value a match binds this variable to, or NULL: such a variable
// is the matched value under its variant's name, and owns nothing itself
INode *flowMatchBound(INode *var);

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

// Does a copy of a value of this type -- a struct, an enum, a tuple, an array --
// add a holder to a counted reference its death releases? And does this
// variant hold one its enum's drop releases?
int flowHeldCounted(INode *type);
int flowVariantHeldCounted(INode *variant);

// Is this type an rc or so reference, single or slice, or a tuple carrying
// one: what a store releases before it overwrites?
int flowIsOwningType(INode *type);

#endif
