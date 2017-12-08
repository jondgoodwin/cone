/** AST handling for primitive numbers
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
NbrTypeAstNode *newNbrTypeNode(uint16_t typ, char nbytes, Symbol *namesym) {
	NbrTypeAstNode *node;
	newAstNode(node, NbrTypeAstNode, typ);
	node->namesym = namesym;
	node->nbytes = nbytes;
	return node;
}

// Serialize the AST for a Unsigned literal
void nbrTypePrint(int indent, NbrTypeAstNode *node, char* prefix) {
	astPrintLn(indent, "%s %s", prefix, node->namesym->namestr);
}
