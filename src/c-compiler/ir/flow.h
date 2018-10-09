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
    FnSigNode *fnsig;	// The type signature of the function we are within
} FlowState;

// Dispatch a node walk for the data flow pass
void flowWalk(FlowState *fstate, INode **node);

// Perform data flow analysis on a node whose value we want to copy or move
// Does it have a valid value? Is it loadable (e.g., readable from a reference)?
// Is it copyable?  If not, can we deactivate its source?
void flowCopyValue(FlowState *fstate, INode **nodep);

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode);

// Start a new scope
size_t flowScopePush();

// Back out of current scope
void flowScopePop(size_t pos, Nodes **varlist);


#endif