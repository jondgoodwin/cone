/** Handling for address-of expression nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new addr node
AddrNode *newAddrNode() {
	AddrNode *node;
	newNode(node, AddrNode, AddrTag);
	return node;
}

// Serialize addr
void addrPrint(AddrNode *node) {
	inodeFprint("&(");
	inodePrintNode(node->vtype);
	inodeFprint("->");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrNode *node, RefNode *reftype) {
	INode *exp = node->exp;
	if (exp->tag != VarNameUseTag
        || (((NameUseNode*)exp)->dclnode->tag != VarDclTag
            && ((NameUseNode*)exp)->dclnode->tag != FnDclTag)) {
		errorMsgNode((INode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
    INamedNode *dclnode = ((NameUseNode*)exp)->dclnode;
    INode *perm = (dclnode->tag == VarDclTag) ? ((VarDclNode*)dclnode)->perm : (INode*)immPerm;
	if (!permMatches(reftype->perm, perm))
		errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
}

// Analyze addr node
void addrPass(PassState *pstate, AddrNode *node) {
	inodeWalk(pstate, &node->exp);
	if (pstate->pass == TypeCheck) {
		if (!isExpNode(node->exp)) {
			errorMsgNode(node->exp, ErrorBadTerm, "Needs to be an expression");
			return;
		}
		RefNode *reftype = (RefNode *)node->vtype;
		if (reftype->pvtype == NULL)
			reftype->pvtype = ((ITypedNode *)node->exp)->vtype; // inferred
		if (reftype->alloc == voidType)
			addrTypeCheckBorrow(node, reftype);
		else
			allocAllocate(node, reftype);
	}
}