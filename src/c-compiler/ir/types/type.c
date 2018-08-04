/** Compiler Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../nametbl.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../../shared/error.h"
#include <string.h>
#include <assert.h>

#define getVtype(node) {\
	if (isValueNode(node)) \
		node = ((TypedAstNode *)node)->vtype; \
	if (node->asttype == TypeNameUseTag) \
		node = (AstNode*)((NameUseAstNode *)node)->dclnode; \
}

// Return value type
AstNode *typeGetVtype(AstNode *node) {
    getVtype(node);
	return node;
}

// Return type (or de-referenced type if ptr/ref)
AstNode *typeGetDerefType(AstNode *node) {
    getVtype(node);
    if (node->asttype == RefType) {
        node = ((PtrAstNode*)node)->pvtype;
        if (node->asttype == TypeNameUseTag)
            node = (AstNode*)((NameUseAstNode *)node)->dclnode;
    }
    else if (node->asttype == PtrType) {
        node = ((PtrAstNode*)node)->pvtype;
        if (node->asttype == TypeNameUseTag)
            node = (AstNode*)((NameUseAstNode *)node)->dclnode;
    }
    return node;
}

// Internal routine only - we know that node1 and node2 are both types
int typeEqual(AstNode *node1, AstNode *node2) {
	// If they are the same type name, types match
	if (node1 == node2)
		return 1;
	if (node1->asttype != node2->asttype)
		return 0;

	// Otherwise use type-specific equality checks
	switch (node1->asttype) {
	case FnSigType:
		return fnSigEqual((FnSigAstNode*)node1, (FnSigAstNode*)node2);
	case RefType: case PtrType:
		return ptrTypeEqual((PtrAstNode*)node1, (PtrAstNode*)node2);
	default:
		return 0;
	}
}

// Return true if the types for both nodes are equivalent
int typeIsSame(AstNode *node1, AstNode *node2) {

	// Convert nodes to their value types
	getVtype(node1);
	getVtype(node2);

	return typeEqual(node1, node2);
}

// Is totype equivalent or a non-changing subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2+ - requires increasingly lossy conversion/coercion
int typeMatches(AstNode *totype, AstNode *fromtype) {
	// Convert, if needed, from names to the type declaration
	if (totype->asttype == TypeNameUseTag)
		totype = (AstNode*)((NameUseAstNode *)totype)->dclnode;
	if (fromtype->asttype == TypeNameUseTag)
		fromtype = (AstNode*)((NameUseAstNode *)fromtype)->dclnode;

	// If they are the same value type info, types match
	if (totype == fromtype)
		return 1;

	// Type-specific matching logic
	switch (totype->asttype) {
	case RefType: case PtrType:
		if (fromtype->asttype != RefType && fromtype->asttype != PtrType)
			return 0;
		return ptrTypeMatches((PtrAstNode*)totype, (PtrAstNode*)fromtype);

	case ArrayType:
		if (totype->asttype != fromtype->asttype)
			return 0;
		return arrayEqual((ArrayAstNode*)totype, (ArrayAstNode*)fromtype);

	//case FnSigType:
	//	return fnSigMatches((FnSigAstNode*)totype, (FnSigAstNode*)fromtype);

	case UintNbrType:
	case IntNbrType:
	case FloatNbrType:
		if (totype->asttype != fromtype->asttype)
			return isNbr(totype) && isNbr(fromtype) ? 4 : 0;
		return ((NbrAstNode *)totype)->bits > ((NbrAstNode *)fromtype)->bits ? 3 : 2;

	default:
		return typeEqual(totype, fromtype);
	}
}

// can from's value be coerced to to's value type?
// This might inject a 'cast' node in front of the 'from' node with non-matching numbers
int typeCoerces(AstNode *to, AstNode **from) {
	AstNode *fromtype = *from;

	// Convert nodes to their value types
	getVtype(to);

	// When coercing a block, do so on its last expression
	while ((*from)->asttype == BlockNode) {
		((TypedAstNode*)*from)->vtype = to;
		from = &nodesLast(((BlockAstNode*)*from)->stmts);
	}

	// Coercing an 'if' requires we do so on all its paths
	if ((*from)->asttype == IfNode) {
		int16_t cnt;
		AstNode **nodesp;
		IfAstNode *ifnode = (IfAstNode*)*from;
		ifnode->vtype = to;
		if (nodesGet(ifnode->condblk, ifnode->condblk->used - 2) != voidType)
			errorMsgNode((AstNode*)ifnode, ErrorNoElse, "Missing else branch which needs to provide a value");
		for (nodesFor(ifnode->condblk, cnt, nodesp)) {
			cnt--; nodesp++;
			AstNode **lastnode = &nodesLast(((BlockAstNode*)*nodesp)->stmts);
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
		*from = (AstNode*) newCastAstNode(*from, to);
		return 1;
	}

	return 0;
}

// Return a CopyTrait indicating how to handle when a value is assigned to a variable or passed to a function.
int typeCopyTrait(AstNode *typenode) {
    getVtype(typenode);

    // For an aggregate type, existence of a destructor or a non-CopyBitwise property is infectious
    // If it has a .copy method, it is CopyMethod, or else it is CopyMove.
    if (isMethodType(typenode)) {
        int copytrait = CopyBitwise;
        MethNodes *nodes = &((MethodTypeAstNode *)typenode)->methprops;
        uint32_t cnt;
        AstNode **nodesp;
        for (methnodesFor(nodes, cnt, nodesp)) {
            if (((*nodesp)->asttype == VarDclTag && CopyBitwise != typeCopyTrait(*nodesp))
                /* || *nodesp points to a destructor */)
                copytrait == CopyBitwise ? CopyMove : copytrait;
            // else if (nodesp points to the .copy method)
            //    return CopyMethod;
        }
        return copytrait;
    }
    // For references, a 'uni' reference is CopyMove and all others are CopyBitwise
    else if (typenode->asttype == RefType) {
        return (((PtrAstNode *)typenode)->perm->flags & MayAlias) ? CopyBitwise : CopyMove;
    }
    // All pointers are CopyMove (potentially unsafe to copy)
    else if (typenode->asttype == PtrType)
        return CopyMove;
    // The default (e.g., numbers) is CopyBitwise
    return CopyBitwise;
}

// Ensure implicit copies (e.g., assignment, function arguments) are done safely
// using a move or the copy method as needed.
void typeHandleCopy(AstNode **nodep) {
    int copytrait = typeCopyTrait(*nodep);
    if (copytrait != CopyBitwise)
        errorMsgNode(*nodep, WarnCopy, "No current support for move. Be sure this is safe!");
    // if CopyMethod - inject use of that method to create a safe clone that can be "moved"
    // if CopyMove - turn off access to the source (via static (local var) or dynamic mechanism)
}

// Add type mangle info to buffer
char *typeMangle(char *bufp, AstNode *vtype) {
	switch (vtype->asttype) {
	case TypeNameUseTag:
	{
		strcpy(bufp, &((NameUseAstNode *)vtype)->dclnode->namesym->namestr);
		break;
	}
	case RefType: case PtrType:
	{
		PtrAstNode *pvtype = (PtrAstNode *)vtype;
		*bufp++ = '&';
		if (pvtype->perm != constPerm) {
			bufp = typeMangle(bufp, (AstNode*)pvtype->perm);
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
VoidTypeAstNode *newVoidNode() {
	VoidTypeAstNode *voidnode;
	newAstNode(voidnode, VoidTypeAstNode, VoidType);
	return voidnode;
}

// Serialize the void type node
void voidPrint(VoidTypeAstNode *voidnode) {
	astFprint("void");
}

