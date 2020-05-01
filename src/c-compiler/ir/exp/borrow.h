/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef borrow_h
#define borrow_h

// Uses RefNode defined in reference.h

// Inject a borrow mutable node on some node (expected to be an lval)
void borrowMutRef(INode **node, INode* type, INode *perm);

// Clone borrow
INode *cloneBorrowNode(CloneState *cstate, RefNode *node);

void borrowPrint(RefNode *node);

// Type check borrow node
void borrowTypeCheck(TypeCheckState *pstate, RefNode **node);

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, RefNode **nodep);

#endif