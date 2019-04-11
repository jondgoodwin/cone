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

// Serialize allocate
void allocatePrint(AllocateNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->vtype);
    inodeFprint("->");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of allocate node
void allocateNameRes(PassState *pstate, AllocateNode **nodep) {
    AllocateNode *node = *nodep;
    inodeWalk(pstate, &node->exp);
}

// Type check allocate node
void allocatePass(PassState *pstate, AllocateNode **nodep) {
    AllocateNode *node = *nodep;
    if (pstate->pass == NameResolution) {
        inodeWalk(pstate, &node->exp);
        return;
    }

    // Type check a borrowed or allocated reference
    RefNode *reftype = (RefNode *)node->vtype;
    inodeWalk(pstate, &node->exp);

    // expression must be a value usable for initializing allocated reference
    INode *initval = node->exp;
    if (!isExpNode(initval)) {
        errorMsgNode(initval, ErrorBadTerm, "Needs to be a value");
        return;
    }

    // Infer reference's value type based on initial value
    reftype->pvtype = ((ITypedNode*)initval)->vtype;
}

// Perform data flow analysis on allocate node
void allocateFlow(FlowState *fstate, AllocateNode **nodep) {
    AllocateNode *node = *nodep;
    // For an allocated reference, we need to handle the copied value
    flowLoadValue(fstate, &node->exp);
}
