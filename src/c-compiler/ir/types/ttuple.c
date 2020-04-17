/** Handling for type tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new type tuple node
TTupleNode *newTTupleNode(int cnt) {
    TTupleNode *tuple;
    newNode(tuple, TTupleNode, TTupleTag);
    tuple->types = newNodes(cnt);
    return tuple;
}

// Serialize a type tuple node
void ttuplePrint(TTupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;

    for (nodesFor(tuple->types, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
}

// Name resolution of the type tuple node
void ttupleNameRes(NameResState *pstate, TTupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->types, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Type check the type tuple node
void ttupleTypeCheck(TypeCheckState *pstate, TTupleNode *tuple) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(tuple->types, cnt, nodesp))
        itypeTypeCheck(pstate, nodesp);
}

// Compare that two tuples are equivalent
int ttupleEqual(TTupleNode *totype, TTupleNode *fromtype) {
    if (fromtype->tag != TTupleTag)
        return 0;
    if (totype->types->used != fromtype->types->used)
        return 0;

    INode **fromnodesp = &nodesGet(fromtype->types, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(totype->types, cnt, nodesp))
        if (!itypeIsSame(*nodesp, *fromnodesp++))
            return 0;
    return 1;
}