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

// Back out of current scope
void flowScopePop(size_t pos, Nodes **varlist);


typedef struct {
    ITypedNodeHdr;
    INode *exp;
    int16_t aliasamt;
} AliasNode;

// Initialize a function's alias stack
void flowAliasInit();

// Start a new frame on alias stack
size_t flowAliasPushNew(int16_t init);

// Restore previous stack
void flowAliasPop(size_t oldpos);

// Reset current frame (to one value initialized to init value)
void flowAliasReset();

// Ensure frame has enough initialized alias counts for 'size' values
void flowAliasSize(int16_t size);

// Increment aliasing count at frame's position
void flowAliasIncr(int16_t pos);

// Get aliasing count at frame's position
int16_t flowAliasGet(size_t pos);

#endif