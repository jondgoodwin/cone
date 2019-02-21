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
    node->vtype = voidType;
    return node;
}

// Serialize deref
void derefPrint(DerefNode *node) {
    inodeFprint("*");
    inodePrintNode(node->exp);
}

// Analyze deref node
void derefPass(PassState *pstate, DerefNode *node) {
    inodeWalk(pstate, &node->exp);
    if (pstate->pass == TypeCheck) {
        PtrNode *ptype = (PtrNode*)((ITypedNode *)node->exp)->vtype;
        if (ptype->tag == RefTag || ptype->tag == PtrTag)
            node->vtype = ptype->pvtype;
        else
            errorMsgNode((INode*)node, ErrorNotPtr, "Cannot de-reference a non-pointer value.");
    }
}

// Insert automatic deref, if node is a ref
void derefAuto(INode **node) {
    if (iexpGetTypeDcl(*node)->tag != RefTag)
        return;
    DerefNode *deref = newDerefNode();
    deref->exp = *node;
    deref->vtype = ((RefNode*)((ITypedNode *)*node)->vtype)->pvtype;
    *node = (INode*)deref;
}
