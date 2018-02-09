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

// Is totype equivalent or a non-changing subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2+ - requires increasingly lossy conversion/coercion
int typeMatches(AstNode *totype, AstNode *fromtype) {
	// Convert, if needed, from names to the type declaration
	if (totype->asttype == VtypeNameUseNode)
		totype = ((NameUseAstNode *)totype)->dclnode->value;
	assert(astgroup(totype->asttype) == VTypeGroup);
	if (fromtype->asttype == VtypeNameUseNode)
		fromtype = ((NameUseAstNode *)fromtype)->dclnode->value;
	assert(astgroup(fromtype->asttype) == VTypeGroup);

	// If they are the same value type info, types match
	if (totype == fromtype)
		return 1;

	// Types must be of the same kind
	if (totype->asttype != fromtype->asttype) {
		if (isNbr(totype) && isNbr(fromtype))
			return 4; // Coerceable to a different number type, with potential loss
		else
			return 0;
	}

	// Type-specific matching logic
	switch (totype->asttype) {
	case RefType:
		return ptrTypeMatches((PtrAstNode*)totype, (PtrAstNode*)fromtype);
	case ArrayType:
		return arrayEqual((ArrayAstNode*)totype, (ArrayAstNode*)fromtype);
	case UintNbrType:
	case IntNbrType:
	case FloatNbrType:
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

// Add type mangle info to buffer
char *typeMangle(char *bufp, AstNode *vtype) {
	switch (vtype->asttype) {
	case VtypeNameUseNode: case PermNameUseNode:
	{
		strcpy(bufp, &((NameUseAstNode *)vtype)->dclnode->namesym->namestr);
		break;
	}
	case RefType:
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

// Initialize built-in types
void typeInit() {
	// Built-in global variable types
	voidType = (AstNode*) newVoidNode();
}
