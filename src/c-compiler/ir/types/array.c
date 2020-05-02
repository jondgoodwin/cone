/** Handling for array types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new array type whose info will be filled in afterwards
ArrayNode *newArrayNode() {
    ArrayNode *anode;
    newNode(anode, ArrayNode, ArrayTag);
    anode->llvmtype = NULL;
    anode->size = 0;
    anode->elems = newNodes(1);
    return anode;
}

// Create a new array type of a specified size and element type
ArrayNode *newArrayNodeTyped(INode *lexnode, size_t size, INode *elemtype) {
    ArrayNode *anode = newArrayNode();
    inodeLexCopy((INode*)anode, lexnode);
    anode->size = size;
    nodesAdd(&anode->elems, elemtype);
    return anode;
}

// Return the element type of the array type
INode *arrayElemType(INode *array) {
    return nodesGet(((ArrayNode *)array)->elems, 0);
}

// Clone array
INode *cloneArrayNode(CloneState *cstate, ArrayNode *node) {
    ArrayNode *newnode = memAllocBlk(sizeof(ArrayNode));
    memcpy(newnode, node, sizeof(ArrayNode));
    newnode->elems = cloneNodes(cstate, node->elems);
    return (INode *)newnode;
}

// Serialize an array type
void arrayPrint(ArrayNode *node) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("[");
    inodeFprint("%d", (int)node->size);
    for (nodesFor(node->elems, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint("]");
}

// Name resolution of an array type
void arrayNameRes(NameResState *pstate, ArrayNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->elems, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Type check an array type
void arrayTypeCheck(TypeCheckState *pstate, ArrayNode *node) {
    INode **elemtypep = &nodesGet(node->elems, 0);
    if (itypeTypeCheck(pstate, elemtypep) == 0)
        return;
    if (!itypeIsConcrete(*elemtypep)) {
        errorMsgNode((INode*)node, ErrorInvType, "Element's type must be concrete and instantiable.");
    }
    // If the element's type if ThreadBound or Move, so is the array's type
    ITypeNode *elemtype = (ITypeNode*)itypeGetTypeDcl(*elemtypep);
    node->flags |= elemtype->flags & (ThreadBound | MoveType);
}

// Compare two array types to see if they are equivalent
int arrayEqual(ArrayNode *node1, ArrayNode *node2) {
    return (node1->size == node2->size
        && itypeIsSame(arrayElemType((INode*)node1), arrayElemType((INode*)node2)));
}

// Is from-type a subtype of to-struct (we know they are not the same)
TypeCompare arrayMatches(ArrayNode *to, ArrayNode *from, SubtypeConstraint constraint) {
    if (to->size != from->size)
        return NoMatch;
    
    TypeCompare result = itypeMatches(arrayElemType((INode*)to), arrayElemType((INode*)from), constraint);
    switch (result) {
    case ConvSubtype:
        return (constraint == Monomorph) ? result : NoMatch;
    default:
        return result;
    }
}
