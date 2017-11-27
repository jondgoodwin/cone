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

// Macro for creating primitive types
#define primtype(dest, typ, nbyt) {\
	NbrTypeAstNode *ptype; \
	newAstNode(ptype, NbrTypeAstNode, typ); \
	ptype->asttype = typ; \
	ptype->nbytes = nbyt; \
	dest = (AstNode*) ptype; \
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
	voidType = (AstNode*) newVoidNode();
	permInit();

	// Add type identifiers to the symbol table
	typAddIdent("mut", mutPerm);
	typAddIdent("mmut", mmutPerm);
	typAddIdent("imm", immPerm);
	typAddIdent("const", constPerm);
	typAddIdent("constx", constxPerm);
	typAddIdent("mutx", mutxPerm);
	typAddIdent("id", idPerm);
}
