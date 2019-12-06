/** Handling for allocate expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new allocate node
AllocateNode *newAllocateNode() {
    AllocateNode *node;
    newNode(node, AllocateNode, AllocateTag);
    return node;
}

// Clone allocate
INode *cloneAllocateNode(CloneState *cstate, AllocateNode *node) {
    AllocateNode *newnode = memAllocBlk(sizeof(AllocateNode));
    memcpy(newnode, node, sizeof(AllocateNode));
    newnode->exp = cloneNode(cstate, node->exp);
    return (INode *)newnode;
}

// Serialize allocate
void allocatePrint(AllocateNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of allocate node
void allocateNameRes(NameResState *pstate, AllocateNode **nodep) {
    AllocateNode *node = *nodep;
    inodeNameRes(pstate, &node->exp);
}

// Type check allocate node
void allocateTypeCheck(TypeCheckState *pstate, AllocateNode **nodep) {
    AllocateNode *node = *nodep;

    // Ensure expression is a value usable for initializing allocated memory
    if (iexpTypeCheck(pstate, &node->exp) == 0)
        return;

    // Infer reference's value type based on initial value
    RefNode *reftype = (RefNode *)node->vtype;
    refSetPermVtype(reftype, reftype->perm, ((IExpNode*)node->exp)->vtype);
}

// Perform data flow analysis on allocate node
void allocateFlow(FlowState *fstate, AllocateNode **nodep) {
    AllocateNode *node = *nodep;
    // For an allocated reference, we need to handle the copied value
    flowLoadValue(fstate, &node->exp);
}
