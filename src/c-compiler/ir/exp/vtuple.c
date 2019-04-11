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
void vtupleNameRes(PassState *pstate, VTupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->values, cnt, nodesp))
        inodeWalk(pstate, nodesp);
}

// Type check the value tuple node
// - Infer type tuple from types of vtuple's values
void vtupleWalk(PassState *pstate, VTupleNode *tuple) {
    if (pstate->pass == NameResolution) {
        vtupleNameRes(pstate, tuple);
        return;
    }

    // Build ad hoc type tuple that accumulates types of vtuple's values
    TTupleNode *ttuple = newTTupleNode(tuple->values->used);
    tuple->vtype = (INode *)ttuple;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->values, cnt, nodesp)) {
        inodeWalk(pstate, nodesp);
        nodesAdd(&ttuple->types, ((ITypedNode *)*nodesp)->vtype);
    }
}
