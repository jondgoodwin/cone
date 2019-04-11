/** Handling for size-of nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new sizeof node
SizeofNode *newSizeofNode() {
    SizeofNode *node;
    newNode(node, SizeofNode, SizeofTag);
    node->vtype = (INode*)usizeType;
    return node;
}

// Serialize sizeof
void sizeofPrint(SizeofNode *node) {
    inodeFprint("(sizeof, ");
    inodePrintNode(node->type);
    inodeFprint(")");
}

// Name resolution of sizeof node
void sizeofNameRes(NameResState *pstate, SizeofNode *node) {
    inodeNameRes(pstate, &node->type);
}

// Type check sizeof node
void sizeofTypeCheck(TypeCheckState *pstate, SizeofNode *node) {
    inodeTypeCheck(pstate, &node->type);
}
