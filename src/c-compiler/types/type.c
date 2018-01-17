/** Compiler Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/symbol.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include <string.h>
#include <assert.h>

#define getVtype(node) {\
	if (astgroup(node->asttype) == ExpGroup) \
		node = ((TypedAstNode *)node)->vtype; \
	if (node->asttype == VtypeNameUseNode) \
		node = ((NameUseAstNode *)node)->dclnode->value; \
	assert(astgroup(node->asttype) == VTypeGroup); \
}

// Return value type
AstNode *typeGetVtype(AstNode *node) {
	getVtype(node);
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
	case FnSig:
		return fnSigEqual((FnSigAstNode*)node1, (FnSigAstNode*)node2);
	case RefType:
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
	// If they are the same value type info, types match
	if (to == fromtype)
		return 1;

	// If types are equivalent, it is a valid subtype
	if (typeEqual(to, fromtype))
		return 1;

	// Not identical - but if both are numbers - cast between them
	if (isNbr(to) && isNbr(fromtype)) {
		*from = (AstNode*) newCastAstNode(*from, to);
		return 1;
	}

	// Types must be of the same kind
	if (to->asttype != fromtype->asttype)
		return 0;

	// Type-specific coercion logic
	switch (to->asttype) {
	case RefType:
		return ptrTypeCoerces((PtrAstNode*)to, (PtrAstNode*)fromtype);
	default:
		return 0;
	}
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

// Initialize built-in types
void typeInit() {
	// Built-in global variable types
	voidType = (AstNode*) newVoidNode();
}
