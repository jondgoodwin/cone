/** Handling for allocate expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new allocate node
RefNode *newAllocateNode() {
    RefNode *node;
    newNode(node, RefNode, AllocateTag);
    return node;
}

// Clone allocate
INode *cloneAllocateNode(CloneState *cstate, RefNode *node) {
    RefNode *newnode = memAllocBlk(sizeof(RefNode));
    memcpy(newnode, node, sizeof(RefNode));
    newnode->vtexp = cloneNode(cstate, node->vtexp);
    return (INode *)newnode;
}

// Serialize allocate
void allocatePrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->vtexp);
    inodeFprint(")");
}

// Name resolution of allocate node
void allocateNameRes(NameResState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;
    inodeNameRes(pstate, &node->vtexp);
}

// Type check allocate node
void allocateTypeCheck(TypeCheckState *pstate, RefNode **nodep) {
    RefNode *node = *nodep;

    // Ensure expression is a value usable for initializing allocated memory
    if (iexpTypeCheckAny(pstate, &node->vtexp) == 0)
        return;

    // Infer reference's value type based on initial value
    RefNode *reftype = (RefNode *)node->vtype;
    refSetPermVtype(reftype, reftype->perm, ((IExpNode*)node->vtexp)->vtype);
}

// Perform data flow analysis on allocate node
void allocateFlow(FlowState *fstate, RefNode **nodep) {
    RefNode *node = *nodep;
    // For an allocated reference, we need to handle the copied value
    flowLoadValue(fstate, &node->vtexp);
}
