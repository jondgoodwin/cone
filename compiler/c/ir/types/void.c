/** void type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new Void type node
VoidTypeNode *newVoidNode() {
    VoidTypeNode *voidnode;
    newNode(voidnode, VoidTypeNode, VoidTag);
    voidnode->flags |= ZeroSizeType;
    return voidnode;
}

// Clone void
INode *cloneVoidNode(CloneState *cstate, VoidTypeNode *node) {
    StarNode *newnode = memAllocBlk(sizeof(VoidTypeNode));
    memcpy(newnode, node, sizeof(VoidTypeNode));
    return (INode *)newnode;
}

// Create a new Absence node
AbsenceNode *newAbsenceNode() {
    AbsenceNode *node;
    newNode(node, AbsenceNode, AbsenceTag);
    node->vtype = (INode*)newVoidNode();
    return node;
}

// Create a node standing in for an expression already reported as bad.
//
// Where the type sentinel errorType is a singleton, this is a fresh node per
// site: it takes an expression's place in the tree, so it keeps that
// expression's source position for anything that still needs to point at it.
// Its errorType value type is what keeps every later check quiet about it.
INode *newErrorNode(INode *lexnode) {
    AbsenceNode *node = newAbsenceNode();
    node->vtype = errorType;
    if (lexnode)
        inodeLexCopy((INode*)node, lexnode);
    return (INode*)node;
}

// Serialize the void type node
void voidPrint(VoidTypeNode *voidnode) {
    inodeFprint("void");
}
