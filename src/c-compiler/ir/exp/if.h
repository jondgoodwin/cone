/** Handling for if nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef if_h
#define if_h

// If statement
typedef struct IfNode {
    ITypedNodeHdr;
    Nodes *condblk;
} IfNode;

IfNode *newIfNode();
void ifPrint(IfNode *ifnode);

// if node name resolution
void ifNameRes(PassState *pstate, IfNode *ifnode);

// Type check the if statement node
// - Every conditional expression must be a bool
// - if's vtype is specified/checked only when coerced by iexpCoerces
void ifPass(PassState *pstate, IfNode *ifnode);

void ifRemoveReturns(IfNode *ifnode);

// Perform data flow analysis on an if expression
void ifFlow(FlowState *fstate, IfNode **ifnodep);

#endif