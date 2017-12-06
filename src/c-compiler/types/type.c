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

// Return true if the types for both nodes are the same
int typeEqual(AstNode *node1, AstNode *node2) {

	// Convert expression nodes to their value type
	if (astgroup(node1->asttype) == ExpGroup)
		node1 = ((TypedAstNode *)node1)->vtype;
	if (astgroup(node2->asttype) == ExpGroup)
		node2 = ((TypedAstNode *)node2)->vtype;

	// If they are the same type name, types match
	if (node1 == node2)
		return 1;

	// Otherwise use type-specific equality checks
	switch (node1->asttype) {
	case FnSig:
		return fnSigEqual((FnSigAstNode*)node1, (FnSigAstNode*)node2);
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
void voidPrint(int indent, VoidTypeAstNode *voidnode, char *prefix) {
	astPrintLn(indent, "%s (void)", prefix);
}

// Initialize built-in types
void typeInit() {
	// Built-in global variable types
	voidType = (AstNode*) newVoidNode();
}
