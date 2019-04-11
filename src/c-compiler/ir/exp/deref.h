/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef deref_h
#define deref_h

typedef struct DerefNode {
    ITypedNodeHdr;
    INode *exp;
} DerefNode;

DerefNode *newDerefNode();
void derefPrint(DerefNode *node);
// Name resolution of deref node
void derefNameRes(PassState *pstate, DerefNode *node);

// Type check deref node
void derefPass(PassState *pstate, DerefNode *node);
void derefAuto(INode **node);

#endif