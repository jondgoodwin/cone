/** Handling for named value nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new named value node
NamedValNode *newNamedValNode(INode *name) {
    NamedValNode *node;
    newNode(node, NamedValNode, NamedValTag);
    node->vtype = NULL;
    node->name = name;
    return node;
}

// Serialize named value node
void namedValPrint(NamedValNode *node) {
    inodePrintNode(node->name);
    inodeFprint(": ");
    inodePrintNode(node->val);
}

// Name resolution of named value node
void namedValNameRes(PassState *pstate, NamedValNode *node) {
    inodeWalk(pstate, &node->val);
}

// Type check named value node
void namedValWalk(PassState *pstate, NamedValNode *node) {
    if (pstate->pass == NameResolution) {
        namedValNameRes(pstate, node);
        return;
    }

    inodeWalk(pstate, &node->val);
    node->vtype = ((ITypedNode*)node->val)->vtype;
}
