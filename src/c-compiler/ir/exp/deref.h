/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef deref_h
#define deref_h

typedef struct DerefNode {
    IExpNodeHdr;
    INode *exp;
} DerefNode;

DerefNode *newDerefNode();

// Clone deref
INode *cloneDerefNode(CloneState *cstate, DerefNode *node);

void derefPrint(DerefNode *node);
// Name resolution of deref node
void derefNameRes(NameResState *pstate, DerefNode *node);

// Type check deref node
void derefTypeCheck(TypeCheckState *pstate, DerefNode *node);

// Inject automatic deref node, if node's type is a ref or ptr. Return 1 if dereffed.
int derefAuto(INode **node);


#endif