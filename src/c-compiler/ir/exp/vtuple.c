/** Handling for value tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new value tuple node
VTupleNode *newVTupleNode() {
    VTupleNode *tuple;
    newNode(tuple, VTupleNode, VTupleTag);
    tuple->vtype = NULL;
    tuple->values = newNodes(4);
    return tuple;
}

// Clone value tuple
INode *cloneVTupleNode(CloneState *cstate, VTupleNode *node) {
    VTupleNode *newnode;
    newnode = memAllocBlk(sizeof(VTupleNode));
    memcpy(newnode, node, sizeof(VTupleNode));
    newnode->values = cloneNodes(cstate, node->values);
    return (INode *)newnode;
}

// Serialize a value tuple node
void vtuplePrint(VTupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;

    for (nodesFor(tuple->values, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
}

// Name resolution for vtuple
void vtupleNameRes(NameResState *pstate, VTupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->values, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Type check the value tuple node
// - Infer type tuple from types of vtuple's values
void vtupleTypeCheck(TypeCheckState *pstate, VTupleNode *tuple) {
    // Build ad hoc type tuple that accumulates types of vtuple's values
    TTupleNode *ttuple = newTTupleNode(tuple->values->used);
    tuple->vtype = (INode *)ttuple;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->values, cnt, nodesp)) {
        inodeTypeCheck(pstate, nodesp);
        nodesAdd(&ttuple->types, ((IExpNode *)*nodesp)->vtype);
    }
}
