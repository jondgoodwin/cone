/** Handling for allocate expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Serialize allocate
void allocatePrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->vtexp);
    inodeFprint(")");
}

// Type check allocate node
void allocateTypeCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;

    // The default permission type is 'uni'
    if (node->perm == unknownType)
        node->perm = newPermUseNode(uniPerm);

    // Ensure expression is a value usable for initializing allocated memory
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    INode *vtype = ((IExpNode*)node->vtexp)->vtype;
    if (!itypeIsConcrete(vtype) || itypeIsZeroSize(vtype)) {
        errorMsgNode(node->vtexp, ErrorInvType, "May not allocate a value of abstract or zero-size type");
    }

    // Infer reference's value type based on initial value
    RefNode *reftype = newRefNodeFull(RefTag, (INode*)node, node->region, node->perm, vtype);
    refTypeCheck(pstate, reftype);
    node->vtype = (INode *)reftype;

    // Type check that ref region's allocation functions are declared correctly
    regionAllocTypeCheck(itypeGetTypeDcl(node->region));
    INode *region = itypeGetTypeDcl(node->region);
}

// Perform data flow analysis on allocate node
void allocateFlow(FlowState *fstate, RefNode **nodep) {
    RefNode *node = *nodep;
    // For an allocated reference, we need to handle the copied value
    flowLoadValue(fstate, &node->vtexp);
    flowHandleMoveOrCopy(&node->vtexp);
}
