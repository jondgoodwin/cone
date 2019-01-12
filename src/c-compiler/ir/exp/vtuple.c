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

// Check the value tuple node
void vtupleWalk(PassState *pstate, VTupleNode *tuple) {
    if (pstate->pass == NameResolution) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(tuple->values, cnt, nodesp))
            inodeWalk(pstate, nodesp);
    }
    else if (pstate->pass == TypeCheck) {
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
}
