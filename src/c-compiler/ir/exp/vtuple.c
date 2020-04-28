/** Handling for value tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

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
    int tag = -1;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
        int newtag = isTypeNode(*nodesp) ? TTupleTag : VTupleTag;
        if (tag == -1)
            tag = newtag;
        else if (tag == -2)
            ;
        else if (tag != newtag)
            tag = -2;
    }
    if (tag >= 0)
        tuple->tag = tag;
    else
        errorMsgNode((INode*)tuple, ErrorBadElems, "Elements of tuple must be all types or all values");
}

// Type check the value tuple node
// - Infer type tuple from types of vtuple's values
void vtupleTypeCheck(TypeCheckState *pstate, TupleNode *tuple) {
    // Build ad hoc type tuple that accumulates types of vtuple's values
    TupleNode *ttuple = newTupleNode(tuple->elems->used);
    ttuple->tag = TTupleTag;
    tuple->vtype = (INode *)ttuple;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp)) {
        if (iexpTypeCheckAny(pstate, nodesp) == 0)
            continue;
        nodesAdd(&ttuple->elems, ((IExpNode *)*nodesp)->vtype);
    }
}
