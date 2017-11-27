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
void typInit() {
	// Built-in global variable types
	voidType = (AstNode*) newVoidNode();
}
