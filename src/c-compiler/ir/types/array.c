/** Handling for structs
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
    anode->owner = NULL;
    anode->namesym = anonName;
    anode->llvmtype = NULL;
    imethnodesInit(&anode->methprops, 0);
    anode->subtypes = newNodes(0);
    return anode;
}

// Serialize an array type
void arrayPrint(ArrayNode *node) {
	inodeFprint("[%d]", (int)node->size);
	inodePrintNode(node->elemtype);
}

// Semantically analyze an array type
void arrayPass(PassState *pstate, ArrayNode *node) {
	inodeWalk(pstate, &node->elemtype);
}

// Compare two struct signatures to see if they are equivalent
int arrayEqual(ArrayNode *node1, ArrayNode *node2) {
	return (node1->size == node2->size
		/* && itypeIsSame(node1->elemtype, node2->elemtype*/);
}
