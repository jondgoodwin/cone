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

// Coerce a value tuple, typed from its own values, to the wanted tuple type by
// coercing each value to its element's type, as an array literal's elements
// are coerced (arrayLitCoerce) and as a return's value tuple is. So an
// unsuffixed literal takes its element's number type: '(5, 6)' is an
// '(i64, i64)' where one is wanted. The counts must agree.
int vtupleCoerce(TupleNode *tuple, INode *totypedcl) {
    if (totypedcl->tag != TTupleTag)
        return 0;
    Nodes *totypes = ((TupleNode *)totypedcl)->elems;
    if (totypes->used != tuple->elems->used)
        return 0;
    INode **totypesp = &nodesGet(totypes, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp)) {
        if (!iexpCoerce(nodesp, *totypesp++))
            return 0;
    }
    tuple->vtype = totypedcl;
    return 1;
}
