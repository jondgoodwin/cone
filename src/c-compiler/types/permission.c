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
PermTypeAstNode *newPermTypeNode(char ptyp, uint16_t flags, AstNode *locker) {
	PermTypeAstNode *node;
	newAstNode(node, PermTypeAstNode, PermType);
	node->flags = flags;
	node->ptype = ptyp;
	node->locker = locker;
	return node;
}

// Serialize the AST for a Unsigned literal
void permTypePrint(int indent, PermTypeAstNode *node, char* prefix) {
	switch (node->ptype) {
	case MutPerm: astPrintLn(indent, "%s %s", prefix, "mut"); break;
	case MmutPerm: astPrintLn(indent, "%s %s", prefix, "mmut"); break;
	case ImmPerm: astPrintLn(indent, "%s %s", prefix, "imm"); break;
	case ConstPerm: astPrintLn(indent, "%s %s", prefix, "const"); break;
	case MutxPerm: astPrintLn(indent, "%s %s", prefix, "mutx"); break;
	case IdPerm: astPrintLn(indent, "%s %s", prefix, "id"); break;
	default: astPrintLn(indent, "%s %s", prefix, "lock"); break;
	}
}
