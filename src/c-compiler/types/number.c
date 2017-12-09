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

// Declare built-in number types and their names
void nbrDclNames() {
	newNameDclNodeStr("i8", VtypeNameDclNode, (AstNode*)(i8Type = newNbrTypeNode(IntNbrType, 1)));
	newNameDclNodeStr("i16", VtypeNameDclNode, (AstNode*)(i16Type = newNbrTypeNode(IntNbrType, 2)));
	newNameDclNodeStr("i32", VtypeNameDclNode, (AstNode*)(i32Type = newNbrTypeNode(IntNbrType, 4)));
	newNameDclNodeStr("i64", VtypeNameDclNode, (AstNode*)(i64Type = newNbrTypeNode(IntNbrType, 8)));
	newNameDclNodeStr("u8", VtypeNameDclNode, (AstNode*)(u8Type = newNbrTypeNode(UintNbrType, 1)));
	newNameDclNodeStr("u16", VtypeNameDclNode, (AstNode*)(u16Type = newNbrTypeNode(UintNbrType, 2)));
	newNameDclNodeStr("u32", VtypeNameDclNode, (AstNode*)(u32Type = newNbrTypeNode(UintNbrType, 4)));
	newNameDclNodeStr("u64", VtypeNameDclNode, (AstNode*)(u64Type = newNbrTypeNode(UintNbrType, 8)));
	newNameDclNodeStr("f32", VtypeNameDclNode, (AstNode*)(f32Type = newNbrTypeNode(FloatNbrType, 4)));
	newNameDclNodeStr("f64", VtypeNameDclNode, (AstNode*)(f64Type = newNbrTypeNode(FloatNbrType, 8)));
}

// Create a new primitive number type node
NbrAstNode *newNbrTypeNode(uint16_t typ, char nbytes) {
	NbrAstNode *node;
	newAstNode(node, NbrAstNode, typ);
	node->nbytes = nbytes;
	return node;
}

// Serialize the AST for a numeric literal
void nbrTypePrint(int indent, NbrAstNode *node, char* prefix) {
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
