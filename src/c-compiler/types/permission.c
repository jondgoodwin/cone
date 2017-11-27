/** Permission Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

// Create a new primitive number type node
PermTypeAstNode *newPermTypeNode(char ptyp, uint16_t flags, AstNode *locker, Symbol *name) {
	PermTypeAstNode *node;
	newAstNode(node, PermTypeAstNode, PermType);
	node->name = name;
	node->flags = flags;
	node->ptype = ptyp;
	node->locker = locker;
	return node;
}

// Serialize the AST for a Unsigned literal
void permTypePrint(int indent, PermTypeAstNode *node, char* prefix) {
	astPrintLn(indent, "%s %s", prefix, node->name->name);
}
