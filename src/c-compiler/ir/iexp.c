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
	if (isExpNode(node) || (node)->tag == VarDclTag || (node)->tag == FnDclTag) \
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
        node = ((RefNode*)node)->pvtype;
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

    // Re-type a null literal node to match the expected ref/ptr type
    if ((*from)->tag == NullTag) {
        if (!refIsNullable(to) && to->tag != PtrTag)
            return 0;
        ((NullNode*)(*from))->vtype = to;
        return 1;
    }

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

	// Add coercion node
    if (match == 2) {
        *from = (INode*)newCastNode(*from, to);
        return 1;
    }

    // If not an integer literal, don't convert
    if ((*from)->tag != ULitTag)
        return 0;
    // If it is an integer literal, convert it to correct type/value in place
    ULitNode *lit = (ULitNode*)(*from);
    lit->vtype = to;
    if (to->tag == FloatNbrTag) {
        lit->tag = FLitTag;
        ((FLitNode*)lit)->floatlit = (double)lit->uintlit;
    }
    return 1;
}

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from) {
    iexpToTypeDcl(to);
    INode *fromtype = *from;
    iexpToTypeDcl(fromtype);
    return itypeMatches(to, fromtype) == 1;
}

// Retrieve the permission flags for the node
uint16_t iexpGetPermFlags(INode *node) {
    switch (node->tag) {
    case VarNameUseTag:
        return iexpGetPermFlags((INode*)((NameUseNode*)node)->dclnode);
    case VarDclTag:
        return permGetFlags(((VarDclNode*)node)->perm);
    case FnDclTag:
        return permGetFlags((INode*)opaqPerm);
    case DerefTag:
    {
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(((DerefNode *)node)->exp);
        if (vtype->tag == RefTag)
            return permGetFlags(vtype->perm);
        else if (vtype->tag == PtrTag)
            return 0xFFFF;  // <-- In a trust block?
        assert(0 && "Should be ref or ptr");
    }
    case ArrIndexTag:
    {
        FnCallNode *fncall = (FnCallNode *)node;
        return iexpGetPermFlags(fncall->objfn);
    }
    case StrFieldTag:
    {
        FnCallNode *fncall = (FnCallNode *)node;
        // Property access. Permission is the conjunction of structure and property permissions.
        return iexpGetPermFlags(fncall->objfn) & iexpGetPermFlags((INode*)fncall->methprop);
    }
    default:
        return 0;
    }
}
