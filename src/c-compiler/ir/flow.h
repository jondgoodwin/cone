/** The Data Flow analysis pass which handles:
 * - Escape analysis of stack and heap-allocated values, including conditional moves
 * - Determining when to de-alias references, including drop & free logic
 * - Checking lifetimes of borrowed references
 * - Checking that various mechanics are allowed by reference permissions
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

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode);

// Start a new scope
size_t flowScopePush();

// Back out of current scope
void flowScopePop(size_t pos, INode *node);


#endif