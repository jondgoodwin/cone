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
    anode->vtype = NULL;
    anode->namesym = anonName;
    anode->llvmtype = NULL;
    nodelistInit(&anode->nodelist, 0);
    anode->subtypes = newNodes(0);
    return anode;
}

// Create a new array type of a specified size and element type
ArrayNode *newArrayNodeTyped(size_t size, INode *elemtype) {
    ArrayNode *anode = newArrayNode();
    anode->size = size;
    anode->elemtype = elemtype;
    return anode;
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
    inodeTypeCheck(pstate, &node->elemtype);
    if (!itypeHasSize(node->elemtype))
        errorMsgNode(node->elemtype, ErrorInvType, "Type must have a defined size.");
}

// Compare two struct signatures to see if they are equivalent
int arrayEqual(ArrayNode *node1, ArrayNode *node2) {
    return (node1->size == node2->size
        && itypeIsSame(node1->elemtype, node2->elemtype));
}
