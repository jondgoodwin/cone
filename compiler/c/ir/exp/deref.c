/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Inject automatic deref node, if node's type is a ref or ptr. Return 1 if dereffed.
int derefInject(INode **node) {
    INode *nodetype = iexpGetTypeDcl(*node);
    if (nodetype->tag != RefTag && nodetype->tag != PtrTag)
        return 0;
    StarNode *deref = newStarNode(DerefTag);
    inodeLexCopy((INode*)deref, *node);
    deref->vtexp = *node;
    if (nodetype->tag == PtrTag)
        deref->vtype = ((StarNode*)nodetype)->vtexp;
    else
        deref->vtype = ((RefNode*)nodetype)->vtexp;
    *node = (INode*)deref;
    return 1;
}

// Serialize deref
void derefPrint(StarNode *node) {
    inodeFprint("*");
    inodePrintNode(node->vtexp);
}

// Type check deref node
void derefTypeCheck(TypeCheckState *pstate, StarNode *node) {
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    INode *ptype = iexpGetTypeDcl(node->vtexp);
    if (ptype->tag == RefTag)
        node->vtype = ((RefNode*)ptype)->vtexp;
    else if (ptype->tag == PtrTag)
        node->vtype = ((StarNode*)ptype)->vtexp;
    else
        errorMsgNode((INode*)node, ErrorNotPtr, "May only de-reference a simple reference or pointer.");
}

// Perform data flow analysis on deref node
void derefFlow(FlowState *fstate, StarNode **node) {
    flowLoadThroughRef(fstate, &(*node)->vtexp);
}
