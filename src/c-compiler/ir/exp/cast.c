/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new cast node
CastNode *newCastNode(INode *exp, INode *type) {
	CastNode *node;
	newNode(node, CastNode, CastTag);
	node->vtype = type;
	node->exp = exp;
	return node;
}

// Serialize cast
void castPrint(CastNode *node) {
	inodeFprint("(cast, ");
	inodePrintNode(node->vtype);
	inodeFprint(", ");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Analyze cast node
void castPass(PassState *pstate, CastNode *node) {
	inodeWalk(pstate, &node->exp);
	inodeWalk(pstate, &node->vtype);
    if (pstate->pass == TypeCheck && 0 == itypeMatches(node->vtype, ((ITypedNode *)node->exp)->vtype)) {
        // Ignore failure to match for now. In future, only allow inside trust block?
        // errorMsgNode(node->vtype, ErrorInvType, "expression may not be type cast to this type");
    }
}
