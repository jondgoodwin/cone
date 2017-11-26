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

// Macro for creating primitive types
#define primtype(dest, typ, nbyt) {\
	PrimTypeAstNode *ptype; \
	newAstNode(ptype, PrimTypeAstNode, typ); \
	ptype->asttype = typ; \
	ptype->nbytes = nbyt; \
	dest = (AstNode*) ptype; \
}

// Initialize Integer and Float primitive types
void primInit() {
	primtype(i8Type, IntType, 1);
	primtype(i16Type, IntType, 2);
	primtype(i32Type, IntType, 4);
	primtype(i64Type, IntType, 8);
	primtype(u8Type, UintType, 1);
	primtype(u16Type, UintType, 2);
	primtype(u32Type, UintType, 4);
	primtype(u64Type, UintType, 8);
	primtype(f32Type, FloatType, 4);
	primtype(f64Type, FloatType, 8);
}

// Add a type identifier to the symbol table
void typAddIdent(char *name, AstNode *type) {
	Symbol *sym;
	TypeAstNode *node;
	sym = symFind(name, strlen(name));
	sym->node = (AstNode*) (node = (TypeAstNode*)memAllocBlk(sizeof(TypeAstNode)));
	node->asttype = TypeNode;
	node->flags = 0;
	node->type = type;
	node->name = sym->name;
}

// Initialize built-in types
void typInit() {
	// Built-in global variable types
	primInit();
	permInit();

	// Add type identifiers to the symbol table
	typAddIdent("i8", i8Type);
	typAddIdent("i16", i16Type);
	typAddIdent("i32", i32Type);
	typAddIdent("i64", i64Type);
	typAddIdent("u8", u8Type);
	typAddIdent("u16", u16Type);
	typAddIdent("u32", u32Type);
	typAddIdent("u64", u64Type);
	typAddIdent("f32", f32Type);
	typAddIdent("f64", f64Type);

	typAddIdent("mut", mutPerm);
	typAddIdent("mmut", mmutPerm);
	typAddIdent("imm", immPerm);
	typAddIdent("const", constPerm);
	typAddIdent("constx", constxPerm);
	typAddIdent("mutx", mutxPerm);
	typAddIdent("id", idPerm);
}
