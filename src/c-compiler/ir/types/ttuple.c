/** Handling for type tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new type tuple node
TupleNode *newTTupleNode(int cnt) {
    TupleNode *tuple;
    newNode(tuple, TupleNode, TTupleTag);
    tuple->elems = newNodes(cnt);
    return tuple;
}

// Serialize a type tuple node
void ttuplePrint(TupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;

    for (nodesFor(tuple->elems, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
}

// Name resolution of the type tuple node
void ttupleNameRes(NameResState *pstate, TupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Type check the type tuple node
void ttupleTypeCheck(TypeCheckState *pstate, TupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->elems, cnt, nodesp))
        itypeTypeCheck(pstate, nodesp);
}

// Compare that two tuples are equivalent
int ttupleEqual(TupleNode *totype, TupleNode *fromtype) {
    if (fromtype->tag != TTupleTag)
        return 0;
    if (totype->elems->used != fromtype->elems->used)
        return 0;

    INode **fromnodesp = &nodesGet(fromtype->elems, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(totype->elems, cnt, nodesp))
        if (!itypeIsSame(*nodesp, *fromnodesp++))
            return 0;
    return 1;
}