/** Handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new struct type whose info will be filled in afterwards
StructNode *newStructNode(Name *namesym) {
	StructNode *snode;
	newNode(snode, StructNode, StructTag);
    snode->vtype = NULL;
    snode->owner = NULL;
    snode->namesym = namesym;
    snode->llvmtype = NULL;
    snode->subtypes = newNodes(0);
    methnodesInit(&snode->methprops, 8);
	return snode;
}

// Serialize a struct type
void structPrint(StructNode *node) {
	inodeFprint(node->tag == StructTag? "struct %s {}" : "alloc %s {}", &node->namesym->namestr);
}

// Semantically analyze a struct type
void structPass(PassState *pstate, StructNode *node) {
    nametblHookPush();
    INode **nodesp;
    uint32_t cnt;
    for (methnodesFor(&node->methprops, cnt, nodesp)) {
        if (isNamedNode(*nodesp))
            nametblHookNode((INamedNode*)*nodesp);
    }
    for (methnodesFor(&node->methprops, cnt, nodesp)) {
        inodeWalk(pstate, (INode**)nodesp);
    }
    nametblHookPop();
}

// Compare two struct signatures to see if they are equivalent
int structEqual(StructNode *node1, StructNode *node2) {
	// inodes must match exactly in order
	return 1;
}

// Will from struct coerce to a to struct (we know they are not the same)
int structCoerces(StructNode *to, StructNode *from) {
	return 1;
}