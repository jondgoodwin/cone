/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new deref node
DerefNode *newDerefNode() {
    DerefNode *node;
    newNode(node, DerefNode, DerefTag);
    node->vtype = unknownType;
    return node;
}

// Inject automatic deref node, if node's type is a ref or ptr. Return 1 if dereffed.
int derefInject(INode **node) {
    INode *nodetype = iexpGetTypeDcl(*node);
    if (nodetype->tag != RefTag && nodetype->tag != PtrTag)
        return 0;
    DerefNode *deref = newDerefNode();
    deref->exp = *node;
    if (nodetype->tag == PtrTag)
        deref->vtype = ((PtrNode*)((IExpNode *)*node)->vtype)->pvtype;
    else
        deref->vtype = ((RefNode*)((IExpNode *)*node)->vtype)->pvtype;
    *node = (INode*)deref;
    return 1;
}

// Clone deref
INode *cloneDerefNode(CloneState *cstate, DerefNode *node) {
    DerefNode *newnode;
    newnode = memAllocBlk(sizeof(DerefNode));
    memcpy(newnode, node, sizeof(DerefNode));
    newnode->exp = cloneNode(cstate, node->exp);
    return (INode *)newnode;
}

// Serialize deref
void derefPrint(DerefNode *node) {
    inodeFprint("*");
    inodePrintNode(node->exp);
}

// Name resolution of deref node
void derefNameRes(NameResState *pstate, DerefNode *node) {
    inodeNameRes(pstate, &node->exp);
}

// Type check deref node
void derefTypeCheck(TypeCheckState *pstate, DerefNode *node) {
    if (iexpTypeCheck(pstate, &node->exp) == 0)
        return;

    PtrNode *ptype = (PtrNode*)((IExpNode *)node->exp)->vtype;
    if (ptype->tag != RefTag && ptype->tag != PtrTag)
        errorMsgNode((INode*)node, ErrorNotPtr, "May only de-reference a simple reference or pointer.");
    node->vtype = ptype->pvtype;
}
