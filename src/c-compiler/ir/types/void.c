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
    voidnode->flags |= OpaqueType;
    return voidnode;
}

// Serialize the void type node
void voidPrint(VoidTypeNode *voidnode) {
    inodeFprint("void");
}
