/** AST handling for names
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"
#include "../shared/error.h"

#include <string.h>

// Create a new name use node
NameUseAstNode *newNameUseNode(Symbol *namesym) {
	NameUseAstNode *name;
	newAstNode(name, NameUseAstNode, NameUseNode);
	name->namesym = namesym;
	return name;
}

// Serialize the AST for a name use
void nameUsePrint(int indent, NameUseAstNode *name, char *prefix) {
	astPrintLn(indent, "%s `%s`", prefix, name->namesym->namestr);
}

// Check the name use's AST
void nameUsePass(AstPass *pstate, NameUseAstNode *name) {
	// During name resolution, point to name declaration and copy over needed fields
	if (pstate->pass == NameResolution) {
		name->dclnode = (NameDclAstNode*)name->namesym->node;
		name->vtype = name->dclnode->vtype;
		switch (name->dclnode->asttype) {
		case VarNameDclNode: name->asttype = VarNameUseNode; return;
		case VtypeNameDclNode: name->asttype = VtypeNameUseNode; return;
		case PermNameDclNode: name->asttype = PermNameUseNode; return;
		case AllocNameDclNode: name->asttype = AllocNameUseNode; return;
		}
	}
}


// Create a new name declaraction node
NameDclAstNode *newNameDclNode(Symbol *namesym, uint16_t asttype, AstNode *type, PermAstNode *perm, AstNode *val) {
	NameDclAstNode *name;
	newAstNode(name, NameDclAstNode, asttype);
	name->vtype = type;
	name->perm = perm;
	name->namesym = namesym;
	name->value = val;
	name->prev = NULL;
	name->scope = 0;
	name->index = 0;
	return name;
}

// Add a compiler built-in type to the global symbol table as immutable, declared type name
// This gives a program's later NameUse nodes something to point to
void newNameDclNodeStr(char *namestr, uint16_t asttype, AstNode *type) {
	Symbol *sym;
	sym = symFind(namestr, strlen(namestr));
	sym->node = (AstNode*)newNameDclNode(sym, asttype, NULL, immPerm, type);
}

// Return true if node is one of the specialized name declaration nodes
int isNameDclNode(AstNode *node) {
	return node->asttype == VarNameDclNode || node->asttype == VtypeNameDclNode || node->asttype == PermNameDclNode || node->asttype == AllocNameDclNode;
}

// Serialize the AST for a variable/function
void nameDclPrint(int indent, NameDclAstNode *name, char *prefix) {
	astPrintLn(indent, name->vtype->asttype == FnSig ? "%s fn %s()" : "%s var %s", prefix, name->namesym->namestr);
	astPrintNode(indent + 1, name->vtype, "");
	if (name->value)
		astPrintNode(indent + 1, name->value, "");
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(AstNode *rettype, BlockAstNode *blk) {
	AstNode *laststmt;
	laststmt = nodesGet(blk->nodes, blk->nodes->used - 1);
	if (rettype == voidType) {
		if (laststmt->asttype != ReturnNode)
			nodesAdd(&blk->nodes, (AstNode*) newReturnNode());
	}
	else {
		if (laststmt->asttype == StmtExpNode)
			laststmt->asttype = ReturnNode;
	}
}

// Check the name declaration's AST
void nameDclPass(AstPass *pstate, NameDclAstNode *name) {
	FnSigAstNode *oldfnsig = pstate->fnsig;

	switch (pstate->pass) {
	case NameResolution:
		// Scoping stuff
		break;

	case TypeCheck:
		// Special handling for a function...
		if (name->vtype->asttype == FnSig && name->value && name->value->asttype == BlockNode) {
			// Syntactic sugar: Turn implicit returns into explicit returns
			fnImplicitReturn(((FnSigAstNode*)name->vtype)->rettype, (BlockAstNode *)name->value);
			// Provide parameter and return type context for type checking function's logic
			pstate->fnsig = (FnSigAstNode*)name->vtype;
		}
		// Type check non-function variable declaration:
		else if (name->value) {
			// Infer the var's vtype from its value, if not provided
			if (name->vtype == voidType)
				name->vtype = ((TypedAstNode *)name->value)->vtype;
			// Otherwise, verify that declared type and initial value type matches
			else if (!typeIsSubtype(name->vtype, name->value))
				errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
		}
		break;
	}

	astPass(pstate, name->vtype);
	astPass(pstate, (AstNode*)name->perm);
	if (name->value)
		astPass(pstate, name->value);

	pstate->fnsig = oldfnsig;
}
