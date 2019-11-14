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

BorrowNode *newBorrowNode();
void borrowPrint(BorrowNode *node);

// Name resolution of borrow node
void borrowNameRes(NameResState *pstate, BorrowNode **nodep);

// Type check borrow node
void borrowTypeCheck(TypeCheckState *pstate, BorrowNode **node);

// Perform data flow analysis on addr node
void borrowFlow(FlowState *fstate, BorrowNode **nodep);

// Insert automatic ref, if node is a variable
void borrowAuto(INode **node, INode* reftype);

#endif