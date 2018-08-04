/** AST handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"

// Serialize the AST for a numeric literal
void nbrTypePrint(NbrAstNode *node) {
	if (node == i8Type)
		astFprint("i8");
	else if (node == i16Type)
		astFprint("i16");
	else if (node == i32Type)
		astFprint("i32");
	else if (node == i64Type)
		astFprint("i64");
	else if (node == u8Type)
		astFprint("u8");
	else if (node == u16Type)
		astFprint("u16");
	else if (node == u32Type)
		astFprint("u32");
	else if (node == u64Type)
		astFprint("u64");
	else if (node == f32Type)
		astFprint("f32");
	else if (node == f64Type)
		astFprint("f64");
	else if (node == boolType)
		astFprint("Bool");
}

// Is a number-typed node
int isNbr(AstNode *node) {
	return (node->asttype == IntNbrType || node->asttype == UintNbrType || node->asttype == FloatNbrType);
}