/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new deref node
StarNode *newDerefNode() {
    StarNode *node;
    newNode(node, StarNode, DerefTag);
    node->vtype = unknownType;
    return node;
}

// Inject automatic deref node, if node's type is a ref or ptr. Return 1 if dereffed.
int derefInject(INode **node) {
    INode *nodetype = iexpGetTypeDcl(*node);
    if (nodetype->tag != RefTag && nodetype->tag != PtrTag)
        return 0;
    StarNode *deref = newDerefNode();
    deref->vtexp = *node;
    if (nodetype->tag == PtrTag)
        deref->vtype = ((StarNode*)((IExpNode *)*node)->vtype)->vtexp;
    else
        deref->vtype = ((RefNode*)((IExpNode *)*node)->vtype)->vtexp;
    *node = (INode*)deref;
    return 1;
}

// Clone deref
INode *cloneDerefNode(CloneState *cstate, StarNode *node) {
    StarNode *newnode;
    newnode = memAllocBlk(sizeof(StarNode));
    memcpy(newnode, node, sizeof(StarNode));
    newnode->vtexp = cloneNode(cstate, node->vtexp);
    return (INode *)newnode;
}

// Serialize deref
void derefPrint(StarNode *node) {
    inodeFprint("*");
    inodePrintNode(node->vtexp);
}

// Name resolution of deref node
void derefNameRes(NameResState *pstate, StarNode *node) {
    inodeNameRes(pstate, &node->vtexp);
}

// Type check deref node
void derefTypeCheck(TypeCheckState *pstate, StarNode *node) {
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    StarNode *ptype = (StarNode*)((IExpNode *)node->vtexp)->vtype;
    if (ptype->tag != RefTag && ptype->tag != PtrTag)
        errorMsgNode((INode*)node, ErrorNotPtr, "May only de-reference a simple reference or pointer.");
    node->vtype = ptype->vtexp;
}
