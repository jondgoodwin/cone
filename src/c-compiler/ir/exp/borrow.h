/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef borrow_h
#define borrow_h

// Uses RefNode defined in reference.h

// Create new borrow node
RefNode *newBorrowNode();

// Insert automatic ref, if node is a variable
void borrowAuto(INode **node, INode* reftype);

// Inject a borrow mutable node on some node (expected to be an lval)
void borrowMutRef(INode **node, INode* type);

// Clone borrow
INode *cloneBorrowNode(CloneState *cstate, RefNode *node);

void borrowPrint(RefNode *node);

// Name resolution of borrow node
void borrowNameRes(NameResState *pstate, RefNode **nodep);

// Type check borrow node
void borrowTypeCheck(TypeCheckState *pstate, RefNode **node);

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, RefNode **nodep);

#endif