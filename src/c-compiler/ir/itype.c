/** Compiler Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>
#include <assert.h>

#define getVtype(node) {\
	if (isExpNode(node)) \
		node = ((ITypedNode *)node)->vtype; \
	if (node->tag == TypeNameUseTag) \
		node = (INode*)((NameUseNode *)node)->dclnode; \
}

// Return value type
INode *typeGetVtype(INode *node) {
    getVtype(node);
	return node;
}

// Return type (or de-referenced type if ptr/ref)
INode *typeGetDerefType(INode *node) {
    getVtype(node);
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

// Internal routine only - we know that node1 and node2 are both types
int typeEqual(INode *node1, INode *node2) {
	// If they are the same type name, types match
	if (node1 == node2)
		return 1;
	if (node1->tag != node2->tag)
		return 0;

	// Otherwise use type-specific equality checks
	switch (node1->tag) {
	case FnSigTag:
		return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
	case RefTag: case PtrTag:
		return ptrTypeEqual((PtrNode*)node1, (PtrNode*)node2);
	default:
		return 0;
	}
}

// Return true if the types for both nodes are equivalent
int typeIsSame(INode *node1, INode *node2) {

	// Convert nodes to their value types
	getVtype(node1);
	getVtype(node2);

	return typeEqual(node1, node2);
}

// Is totype equivalent or a non-changing subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2+ - requires increasingly lossy conversion/coercion
int typeMatches(INode *totype, INode *fromtype) {
	// Convert, if needed, from names to the type declaration
	if (totype->tag == TypeNameUseTag)
		totype = (INode*)((NameUseNode *)totype)->dclnode;
	if (fromtype->tag == TypeNameUseTag)
		fromtype = (INode*)((NameUseNode *)fromtype)->dclnode;

	// If they are the same value type info, types match
	if (totype == fromtype)
		return 1;

	// Type-specific matching logic
	switch (totype->tag) {
	case RefTag: case PtrTag:
		if (fromtype->tag != RefTag && fromtype->tag != PtrTag)
			return 0;
		return ptrTypeMatches((PtrNode*)totype, (PtrNode*)fromtype);

	case ArrayTag:
		if (totype->tag != fromtype->tag)
			return 0;
		return arrayEqual((ArrayNode*)totype, (ArrayNode*)fromtype);

	//case FnSigTag:
	//	return fnSigMatches((FnSigNode*)totype, (FnSigNode*)fromtype);

	case UintNbrTag:
	case IntNbrTag:
	case FloatNbrTag:
		if (totype->tag != fromtype->tag)
			return isNbr(totype) && isNbr(fromtype) ? 4 : 0;
		return ((NbrNode *)totype)->bits > ((NbrNode *)fromtype)->bits ? 3 : 2;

	default:
		return typeEqual(totype, fromtype);
	}
}

// can from's value be coerced to to's value type?
// This might inject a 'cast' node in front of the 'from' node with non-matching numbers
int typeCoerces(INode *to, INode **from) {
	INode *fromtype = *from;

	// Convert nodes to their value types
	getVtype(to);

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
			if (!typeCoerces(to, lastnode)) {
				errorMsgNode(*lastnode, ErrorInvType, "expression type does not match expected type");
			}
		}
		return 1;
	}

	getVtype(fromtype);

	// Are types equivalent, or is 'to' a subtype of fromtype?
	int match = typeMatches(to, fromtype);
	if (match <= 1)
		return match; // return fail or non-changing matches. Fall through to perform any coercion

	// Add coercion operation. When both are numbers - cast between them
	if (isNbr(to)) {
		*from = (INode*) newCastNode(*from, to);
		return 1;
	}

	return 0;
}

// Return a CopyTrait indicating how to handle when a value is assigned to a variable or passed to a function.
int typeCopyTrait(INode *typenode) {
    getVtype(typenode);

    // For an aggregate type, existence of a destructor or a non-CopyBitwise property is infectious
    // If it has a .copy method, it is CopyMethod, or else it is CopyMove.
    if (isMethodType(typenode)) {
        int copytrait = CopyBitwise;
        IMethNodes *nodes = &((IMethodNode *)typenode)->methprops;
        uint32_t cnt;
        INode **nodesp;
        for (imethnodesFor(nodes, cnt, nodesp)) {
            if (((*nodesp)->tag == VarDclTag && CopyBitwise != typeCopyTrait(*nodesp))
                /* || *nodesp points to a destructor */)
                copytrait == CopyBitwise ? CopyMove : copytrait;
            // else if (nodesp points to the .copy method)
            //    return CopyMethod;
        }
        return copytrait;
    }
    // For references, a 'uni' reference is CopyMove and all others are CopyBitwise
    else if (typenode->tag == RefTag) {
        return (((PtrNode *)typenode)->perm->flags & MayAlias) ? CopyBitwise : CopyMove;
    }
    // All pointers are CopyMove (potentially unsafe to copy)
    else if (typenode->tag == PtrTag)
        return CopyMove;
    // The default (e.g., numbers) is CopyBitwise
    return CopyBitwise;
}

// Ensure implicit copies (e.g., assignment, function arguments) are done safely
// using a move or the copy method as needed.
void typeHandleCopy(INode **nodep) {
    int copytrait = typeCopyTrait(*nodep);
    if (copytrait != CopyBitwise)
        errorMsgNode(*nodep, WarnCopy, "No current support for move. Be sure this is safe!");
    // if CopyMethod - inject use of that method to create a safe clone that can be "moved"
    // if CopyMove - turn off access to the source (via static (local var) or dynamic mechanism)
}

// Add type mangle info to buffer
char *typeMangle(char *bufp, INode *vtype) {
	switch (vtype->tag) {
	case TypeNameUseTag:
	{
		strcpy(bufp, &((NameUseNode *)vtype)->dclnode->namesym->namestr);
		break;
	}
	case RefTag: case PtrTag:
	{
		PtrNode *pvtype = (PtrNode *)vtype;
		*bufp++ = '&';
		if (pvtype->perm != constPerm) {
			bufp = typeMangle(bufp, (INode*)pvtype->perm);
			*bufp++ = ' ';
		}
		bufp = typeMangle(bufp, pvtype->pvtype);
		break;
	}
	default:
		assert(0 && "unknown type for parameter type mangling");
	}
	return bufp + strlen(bufp);
}

// Create a new Void type node
VoidTypeNode *newVoidNode() {
	VoidTypeNode *voidnode;
	newNode(voidnode, VoidTypeNode, VoidTag);
	return voidnode;
}

// Serialize the void type node
void voidPrint(VoidTypeNode *voidnode) {
	inodeFprint("void");
}

