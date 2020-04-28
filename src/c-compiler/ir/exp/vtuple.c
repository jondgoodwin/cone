/** Handling for value tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new value tuple node
TupleNode *newVTupleNode() {
    TupleNode *tuple;
    newNode(tuple, TupleNode, VTupleTag);
    tuple->vtype = unknownType;
    tuple->elems = newNodes(4);
    return tuple;
}

// Clone value tuple
INode *cloneVTupleNode(CloneState *cstate, TupleNode *node) {
    TupleNode *newnode;
    newnode = memAllocBlk(sizeof(TupleNode));
    memcpy(newnode, node, sizeof(TupleNode));
    newnode->elems = cloneNodes(cstate, node->elems);
    return (INode *)newnode;
}

// Serialize a value tuple node
void vtuplePrint(TupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;

    for (nodesFor(tuple->elems, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
}

// Name resolution for vtuple
void vtupleNameRes(NameResState *pstate, TupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Type check the value tuple node
// - Infer type tuple from types of vtuple's values
void vtupleTypeCheck(TypeCheckState *pstate, TupleNode *tuple) {
    // Build ad hoc type tuple that accumulates types of vtuple's values
    TupleNode *ttuple = newTTupleNode(tuple->elems->used);
    tuple->vtype = (INode *)ttuple;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp)) {
        if (iexpTypeCheckAny(pstate, nodesp) == 0)
            continue;
        nodesAdd(&ttuple->elems, ((IExpNode *)*nodesp)->vtype);
    }
}
