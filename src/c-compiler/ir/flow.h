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

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode);

// Start a new scope
size_t flowScopePush();

// Create de-alias list of all own/rc reference variables, except var found in retexp 
// As a simple optimization: returns 1 if retexp name was not de-aliased
int flowScopeDealias(size_t pos, Nodes **varlist, INode *retexp);
// Back out of current scope
void flowScopePop(size_t pos);

// Alias Node structure
typedef struct {
    IExpNodeHdr;
    INode *exp;
    int16_t *counts;   // points to array of counts. NULL if not tuple
    int16_t aliasamt;  // count nbr if not a tuple, # of counts if tuple
} AliasNode;

// Handle when moving or copying a value to a new destination
void flowHandleMoveOrCopy(INode **nodep);

#endif
