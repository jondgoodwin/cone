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

// Analyze named value node
void namedValWalk(PassState *pstate, NamedValNode *node) {
	inodeWalk(pstate, &node->val);
    if (pstate->pass == TypeCheck) {
        node->vtype = ((ITypedNode*)node->val)->vtype;
    }
}
