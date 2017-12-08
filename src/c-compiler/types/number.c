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
NbrTypeAstNode *newNbrTypeNode(uint16_t typ, char nbytes) {
	NbrTypeAstNode *node;
	newAstNode(node, NbrTypeAstNode, typ);
	node->nbytes = nbytes;
	return node;
}

// Serialize the AST for a numeric literal
void nbrTypePrint(int indent, NbrTypeAstNode *node, char* prefix) {
	if (node==i8Type)
		astPrintLn(indent, "%s %s", prefix, "i8");
	else if (node == i16Type)
		astPrintLn(indent, "%s %s", prefix, "i16");
	else if (node == i32Type)
		astPrintLn(indent, "%s %s", prefix, "i32");
	else if (node == i64Type)
		astPrintLn(indent, "%s %s", prefix, "i64");
	else if (node == u8Type)
		astPrintLn(indent, "%s %s", prefix, "u8");
	else if (node == u16Type)
		astPrintLn(indent, "%s %s", prefix, "u16");
	else if (node == u32Type)
		astPrintLn(indent, "%s %s", prefix, "u32");
	else if (node == u64Type)
		astPrintLn(indent, "%s %s", prefix, "u64");
	else if (node == f32Type)
		astPrintLn(indent, "%s %s", prefix, "f32");
	else if (node == f64Type)
		astPrintLn(indent, "%s %s", prefix, "f64");
}
