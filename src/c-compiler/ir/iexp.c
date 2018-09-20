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

    // Handle coercion to a type tuple
    if (to->tag == TTupleTag) {
        Nodes *totypes = ((TTupleNode *)to)->types;
        // When it is from a value tuple, we can coerce each value
        if ((*from)->tag == VTupleTag) {
            Nodes *values = ((VTupleNode *)*from)->values;
            if (values->used < totypes->used)
                return 0;
            int16_t cnt;
            INode **nodesp;
            INode **typep = &nodesGet(totypes, 0);
            for (nodesFor(values, cnt, nodesp)) {
                if (iexpCoerces(*nodesp, typep++) == 0)
                    return 0;
            }
            return 1;
        }

        // When it is from a single node returning a type tuple, require exact type match
        INode *fromtype = ((ITypedNode*)(*from))->vtype;
        if (fromtype->tag == TTupleTag) {
            Nodes *fromtypes = ((TTupleNode *)fromtype)->types;
            if (fromtypes->used < totypes->used)
                return 0;
            int16_t cnt;
            INode **nodesp;
            INode **typep = &nodesGet(totypes, 0);
            for (nodesFor(fromtypes, cnt, nodesp)) {   
                if (itypeIsSame(*nodesp, *typep++) == 0)
                    return 0;
            }
            return 1;
        }
        return 0;
    }

    INode *fromtype = *from;
    iexpToTypeDcl(fromtype);

	// Are types equivalent, or is 'to' a subtype of fromtype?
	int match = itypeMatches(to, fromtype);
	if (match <= 1)
		return match; // return fail or non-changing matches. Fall through to perform any coercion

	// Add coercion node
	*from = (INode*) newCastNode(*from, to);
	return 1;
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

// Retrieve the permission flags for the node
uint16_t iexpGetPermFlags(INode *node) {
    switch (node->tag) {
    case VarNameUseTag:
        return iexpGetPermFlags((INode*)((NameUseNode*)node)->dclnode);
    case VarDclTag:
        return permGetFlags(((VarDclNode*)node)->perm);
    case FnDclTag:
        return permGetFlags((INode*)idPerm);
    case DerefTag:
    {
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(((DerefNode *)node)->exp);
        if (vtype->tag == RefTag)
            return permGetFlags(vtype->perm);
        else if (vtype->tag == PtrTag)
            return 0xFFFF;  // <-- In a trust block?
        assert(0 && "Should be ref or ptr");
    }
    case FnCallTag:
    {
        FnCallNode *fncall = (FnCallNode *)node;
        // Property access. Permission is the conjunction of structure and property permissions.
        if (fncall->methprop)
            return iexpGetPermFlags(fncall->objfn) & iexpGetPermFlags((INode*)fncall->methprop);
        // True function call. Has no permissions.
        else if (((ITypedNode*)fncall->objfn)->vtype->tag == FnSigTag)
            return 0;
        // Array index/slice. Permission based on array's permission
        else
            return iexpGetPermFlags(fncall->objfn);
    }
    default:
        return 0;
    }
}

// expression is valid lval expression: we can get its address and assign it a value
int iexpIsLval(INode *node) {
    switch (node->tag) {

        // Variable names and dereferences (*) are always lvals
    case VarNameUseTag:
    case DerefTag:
        return 1;

        // A FnCall node is only an lval if it is *not* a function call
        // (i.e., it is a property access or array index/slice)
    case FnCallTag:
        return ((ITypedNode*)((FnCallNode*)node)->objfn)->vtype->tag != FnSigTag;

    default: break;
    }

    return 0;
}

// Is Lval mutable
int iexpIsLvalMutable(INode *lval) {
    return MayWrite & iexpGetPermFlags(lval);
}

// Verify that all lval expressions are valid lvals and are mutable
void iexpLvalCheck(INode *lval) {
    // Check each lval separately in a value tuple of lvals
    if (lval->tag == VTupleTag) {
        VTupleNode *ltuple = (VTupleNode *)lval;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(ltuple->values, cnt, nodesp))
            iexpLvalCheck(*nodesp);
        return;
    }

    // '_' named lval need not be checked. It is a placeholder that just swallows a value
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;

    // lval must be an lval and mutable
    if (!iexpIsLval(lval))
        errorMsgNode(lval, ErrorBadLval, "Expression to left of assignment must be lval");
    else if (!iexpIsLvalMutable(lval))
        errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
}

// Ensure we can read and copy/move all rvals
void iexpRvalCheck(INode **rvalp) {
    // Check each lval separately in a value tuple of lvals
    if ((*rvalp)->tag == VTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((VTupleNode *)*rvalp)->values, cnt, nodesp))
            iexpRvalCheck(nodesp);
        return;
    }

    iexpHandleCopy(rvalp);  // Move semantics, etc.
}