/** Handling for borrow expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef borrow_h
#define borrow_h

typedef struct BorrowNode {
    IExpNodeHdr;
    INode *exp;
} BorrowNode;

// Create new borrow node
BorrowNode *newBorrowNode();

// Insert automatic ref, if node is a variable
void borrowAuto(INode **node, INode* reftype);

// Inject a borrow mutable node on some node (expected to be an lval)
void borrowMutRef(INode **node, INode* type);

// Clone borrow
INode *cloneBorrowNode(CloneState *cstate, BorrowNode *node);

void borrowPrint(BorrowNode *node);

// Name resolution of borrow node
void borrowNameRes(NameResState *pstate, BorrowNode **nodep);

// Type check borrow node
void borrowTypeCheck(TypeCheckState *pstate, BorrowNode **node);

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, BorrowNode **nodep);

#endif