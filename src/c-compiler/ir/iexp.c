/** Expression nodes that return a typed value
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>
#include <assert.h>

// Macro that changes iexp pointer to type dcl pointer
#define iexpToTypeDcl(node) {\
	if (isExpNode(node)) \
		node = ((ITypedNode *)node)->vtype; \
	if (node->tag == TypeNameUseTag) \
		node = (INode*)((NameUseNode *)node)->dclnode; \
}

// Return value type
INode *iexpGetTypeDcl(INode *node) {
    iexpToTypeDcl(node);
	return node;
}

// Return type (or de-referenced type if ptr/ref)
INode *iexpGetDerefTypeDcl(INode *node) {
    iexpToTypeDcl(node);
    if (node->tag == RefTag) {
        node = ((PtrNode*)node)->pvtype;
        if (node->tag == TypeNameUseTag)
            node = (INode*)((NameUseNode *)node)->dclnode;
    }
    else if (node->tag == PtrTag) {
        node = ((PtrNode*)node)->pvtype;
        if (node->tag == TypeNameUseTag)
            node = (INode*)((NameUseNode *)node)->dclnode;
    }
    return node;
}

// can from's value be coerced to to's value type?
// This might inject a 'cast' node in front of the 'from' node with non-matching numbers
int iexpCoerces(INode *to, INode **from) {

	// Convert to pointer from iexp to type dcl
	iexpToTypeDcl(to);

	// When coercing a block, do so on its last expression
	while ((*from)->tag == BlockTag) {
		((ITypedNode*)*from)->vtype = to;
		from = &nodesLast(((BlockNode*)*from)->stmts);
	}

	// Coercing an 'if' requires we do so on all its paths
	if ((*from)->tag == IfTag) {
		int16_t cnt;
		INode **nodesp;
		IfNode *ifnode = (IfNode*)*from;
		ifnode->vtype = to;
		if (nodesGet(ifnode->condblk, ifnode->condblk->used - 2) != voidType)
			errorMsgNode((INode*)ifnode, ErrorNoElse, "Missing else branch which needs to provide a value");
		for (nodesFor(ifnode->condblk, cnt, nodesp)) {
			cnt--; nodesp++;
			INode **lastnode = &nodesLast(((BlockNode*)*nodesp)->stmts);
			if (!iexpCoerces(to, lastnode)) {
				errorMsgNode(*lastnode, ErrorInvType, "expression type does not match expected type");
			}
		}
		return 1;
	}

    INode *fromtype = *from;
    iexpToTypeDcl(fromtype);

	// Are types equivalent, or is 'to' a subtype of fromtype?
	int match = itypeMatches(to, fromtype);
	if (match <= 1)
		return match; // return fail or non-changing matches. Fall through to perform any coercion

	// Add coercion operation. When both are numbers - cast between them
	if (isNbr(to)) {
		*from = (INode*) newCastNode(*from, to);
		return 1;
	}

	return 0;
}

// Ensure implicit copies (e.g., assignment, function arguments) are done safely
// using a move or the copy method as needed.
void iexpHandleCopy(INode **nodep) {
    int copytrait = itypeCopyTrait(((ITypedNode *)*nodep)->vtype);
    if (copytrait != CopyBitwise)
        errorMsgNode(*nodep, WarnCopy, "No current support for move. Be sure this is safe!");
    // if CopyMethod - inject use of that method to create a safe clone that can be "moved"
    // if CopyMove - turn off access to the source (via static (local var) or dynamic mechanism)
}
