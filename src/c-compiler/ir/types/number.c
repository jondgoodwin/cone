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
void nbrTypePrint(NbrNode *node) {
	if (node == i8Type)
		inodeFprint("i8");
	else if (node == i16Type)
		inodeFprint("i16");
	else if (node == i32Type)
		inodeFprint("i32");
	else if (node == i64Type)
		inodeFprint("i64");
	else if (node == u8Type)
		inodeFprint("u8");
	else if (node == u16Type)
		inodeFprint("u16");
	else if (node == u32Type)
		inodeFprint("u32");
	else if (node == u64Type)
		inodeFprint("u64");
	else if (node == f32Type)
		inodeFprint("f32");
	else if (node == f64Type)
		inodeFprint("f64");
	else if (node == boolType)
		inodeFprint("Bool");
}

// Is a number-typed node
int isNbr(INode *node) {
	return (node->asttype == IntNbrTag || node->asttype == UintNbrTag || node->asttype == FloatNbrTag);
}