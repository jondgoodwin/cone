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
#include <string.h>
#include <assert.h>

#define getVtype(node) {\
	if (astgroup(node->asttype) == ExpGroup) \
		node = ((TypedAstNode *)node)->vtype; \
	if (node->asttype == VtypeNameUseNode) \
		node = ((NameUseAstNode *)node)->dclnode->value; \
	assert(astgroup(node->asttype) == VTypeGroup); \
}

int typeEqual(AstNode *node1, AstNode *node2) {
	// Otherwise use type-specific equality checks
	switch (node1->asttype) {
	case FnSig:
		return fnSigEqual((FnSigAstNode*)node1, (FnSigAstNode*)node2);
	default:
		return 0;
	}
}
// Return true if the types for both nodes are equivalent
int typeIsSame(AstNode *node1, AstNode *node2) {

	// Convert nodes to their value types
	getVtype(node1);
	getVtype(node2);

	// If they are the same type name, types match
	if (node1 == node2)
		return 1;

	return typeEqual(node1, node2);
}

// can node be subtyped to subtype?
int typeIsSubtype(AstNode *subtype, AstNode *node) {
	int ret;

	// Convert nodes to their value types
	getVtype(subtype);
	getVtype(node);

	// If they are the same value type info, types match
	if (subtype == node)
		return 1;

	// If types are equivalent, it is a valid subtype
	if (ret = typeEqual(subtype, node))
		return ret;

	// Need some subtyping logic here!!
	return 0;
}

// Create a new Void type node
VoidTypeAstNode *newVoidNode() {
	VoidTypeAstNode *voidnode;
	newAstNode(voidnode, VoidTypeAstNode, VoidType);
	return voidnode;
}

// Serialize the void type node
void voidPrint(int indent, VoidTypeAstNode *voidnode, char *prefix) {
	astPrintLn(indent, "%s (void)", prefix);
}

// Initialize built-in types
void typeInit() {
	// Built-in global variable types
	voidType = (AstNode*) newVoidNode();
}
