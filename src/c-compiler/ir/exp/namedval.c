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
void namedValNameRes(NameResState *pstate, NamedValNode *node) {
    inodeNameRes(pstate, &node->val);
}

// Type check named value node
void namedValTypeCheck(TypeCheckState *pstate, NamedValNode *node) {
    inodeTypeCheck(pstate, &node->val);
    node->vtype = ((ITypedNode*)node->val)->vtype;
}
