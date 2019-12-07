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
    anode->namesym = anonName;
    anode->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)anode, 0);
    return anode;
}

// Create a new array type of a specified size and element type
ArrayNode *newArrayNodeTyped(size_t size, INode *elemtype) {
    ArrayNode *anode = newArrayNode();
    anode->size = size;
    anode->elemtype = elemtype;
    return anode;
}

// Clone array
INode *cloneArrayNode(CloneState *cstate, ArrayNode *node) {
    ArrayNode *newnode = memAllocBlk(sizeof(ArrayNode));
    memcpy(newnode, node, sizeof(ArrayNode));
    newnode->elemtype = cloneNode(cstate, node->elemtype);
    return (INode *)newnode;
}

// Serialize an array type
void arrayPrint(ArrayNode *node) {
    inodeFprint("[%d]", (int)node->size);
    inodePrintNode(node->elemtype);
}

// Name resolution of an array type
void arrayNameRes(NameResState *pstate, ArrayNode *node) {
    inodeNameRes(pstate, &node->elemtype);
}

// Type check an array type
void arrayTypeCheck(TypeCheckState *pstate, ArrayNode *node) {
    if (itypeTypeCheck(pstate, &node->elemtype) == 0)
        return;
    if (!itypeIsConcrete(node->elemtype)) {
        errorMsgNode((INode*)node, ErrorInvType, "Element's type must be concrete and instantiable.");
    }
    // If the element's type if ThreadBound or Move, so is the array's type
    ITypeNode *elemtype = (ITypeNode*)itypeGetTypeDcl(node->elemtype);
    node->flags |= elemtype->flags & (ThreadBound | MoveType);
}

// Compare two struct signatures to see if they are equivalent
int arrayEqual(ArrayNode *node1, ArrayNode *node2) {
    return (node1->size == node2->size
        && itypeIsSame(node1->elemtype, node2->elemtype));
}
