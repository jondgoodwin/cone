/** Handling for deref nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Inject automatic deref node, if node's type is a ref or ptr. Return 1 if dereffed.
// A key reaches nothing on its own: only its arena's '[]' reaches what it
// names, 'arena[key]', with the arena of the same invariant lifetime
static void derefRefuseKey(INode *at, INode *type) {
    if (lifeIsKey(type))
        lifeKeyAccessError(at, type);
}

int derefInject(INode **node) {
    INode *nodetype = iexpGetTypeDcl(*node);
    if (nodetype->tag != RefTag && nodetype->tag != PtrTag)
        return 0;
    derefRefuseKey(*node, nodetype);
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

int derefWarnIndexStar = 0;
StarNode *derefSliceBorrowed = NULL;

// Type check deref node
void derefTypeCheck(TypeCheckState *pstate, StarNode *node) {
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    // '*x[i]' over an index place, the spelling from when 'x[i]' was the borrow
    // '[]' lends: during the transition it keeps that meaning, the written '*'
    // taking the place of the injected one, so it is the element as before
    if (derefIsIndexPlace(node->vtexp)) {
        if (derefWarnIndexStar)
            errorMsgNode((INode*)node, WarnIndexStar,
                "`x[i]` is now the element itself, a place, as an array's is: write `x[i]`, not `*x[i]`.");
        node->vtexp = ((StarNode*)node->vtexp)->vtexp;
    }

    INode *ptype = iexpGetTypeDcl(node->vtexp);
    derefRefuseKey((INode*)node, ptype);
    if (ptype->tag == RefTag)
        node->vtype = ((RefNode*)ptype)->vtexp;
    else if (ptype->tag == PtrTag)
        node->vtype = ((StarNode*)ptype)->vtexp;
    // A slice reference's pointee, the array it borrows: a place only to be borrowed again
    // ('&*s', '&mut *s', '&uni *s'), which borrowTypeCheck allows by naming the node
    else if (ptype->tag == ArrayRefTag && node == derefSliceBorrowed)
        node->vtype = (INode*)newArrayDerefNodeFrom((RefNode*)ptype);
    else
        errorMsgNode((INode*)node, ErrorNotPtr, "May only de-reference a simple reference or pointer.");
}

// Perform data flow analysis on deref node
void derefFlow(FlowState *fstate, StarNode **node) {
    // '*self' and 'self.x' in an init reach through self, which is allowed
    // once it is filled (flowNewSelf)
    if (flowNewSelf((*node)->vtexp)) {
        int svthrough = flowThroughSelf;
        flowThroughSelf = 1;
        flowLoadThroughRef(fstate, &(*node)->vtexp);
        flowThroughSelf = svthrough;
        return;
    }
    flowLoadThroughRef(fstate, &(*node)->vtexp);
}
