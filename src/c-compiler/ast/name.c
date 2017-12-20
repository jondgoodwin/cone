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
void nameUsePrint(NameUseAstNode *name) {
	astFprint(&name->namesym->namestr);
}

// Check the name use's AST
void nameUsePass(AstPass *pstate, NameUseAstNode *name) {
	// During name resolution, point to name declaration and copy over needed fields
	switch (pstate->pass) {
	case NameResolution:
		name->dclnode = (NameDclAstNode*)name->namesym->node;
		if (name->dclnode) {
			switch (name->dclnode->asttype) {
			case VarNameDclNode: name->asttype = VarNameUseNode; return;
			case VtypeNameDclNode: name->asttype = VtypeNameUseNode; return;
			case PermNameDclNode: name->asttype = PermNameUseNode; return;
			case AllocNameDclNode: name->asttype = AllocNameUseNode; return;
			}
		}
		else
			errorMsgNode((AstNode*)name, ErrorUnkName, "The name %s does not refer to a declared name", &name->namesym->namestr);
		break;
	case TypeCheck:
		name->vtype = name->dclnode->vtype;
		break;
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
void nameDclPrint(NameDclAstNode *name) {
	int newline = 1;
	astPrintIndent();
	astPrintNode((AstNode*)name->perm);
	astFprint("%s ", &name->namesym->namestr);
	astPrintNode(name->vtype);
	if (name->value) {
		astFprint(" = ");
		astPrintNode(name->value);
		if (name->value->asttype == BlockNode)
			newline = 0;
	}
	if (newline)
		astPrintNL();
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(AstNode *rettype, BlockAstNode *blk) {
	AstNode *laststmt;
	laststmt = nodesGet(blk->stmts, blk->stmts->used - 1);
	if (rettype == voidType) {
		if (laststmt->asttype != ReturnNode)
			nodesAdd(&blk->stmts, (AstNode*) newReturnNode());
	}
	else {
		if (laststmt->asttype == StmtExpNode)
			laststmt->asttype = ReturnNode;
	}
}

// Enable resolution of fn parameter references to parameters
void nameDclFnNameResolve(AstPass *pstate, NameDclAstNode *name) {
	FnSigAstNode *fnsig = (FnSigAstNode*)name->vtype;
	SymNode *nodesp;
	uint32_t cnt;
	// Load parameters into global symbol table (with unhooking mechanism)
	for (inodesFor(fnsig->parms, cnt, nodesp)) {
		NameDclAstNode *parm = (NameDclAstNode*)nodesp->node;
		parm->prev = parm->namesym->node;
		parm->namesym->node = (AstNode*)parm;
	}
	astPass(pstate, name->value);
	// Unhood parameters from global symbol table
	for (inodesFor(fnsig->parms, cnt, nodesp)) {
		NameDclAstNode *parm = (NameDclAstNode*)nodesp->node;
		parm->namesym->node = parm->prev;
	}
}

// Provide parameter and return type context for type checking function's logic
void nameDclFnTypeCheck(AstPass *pstate, NameDclAstNode *name) {
	FnSigAstNode *oldfnsig = pstate->fnsig;
	pstate->fnsig = (FnSigAstNode*)name->vtype;
	astPass(pstate, name->value);
	pstate->fnsig = oldfnsig;
}

// Type check variable against its initial value
void nameDclVarTypeCheck(AstPass *pstate, NameDclAstNode *name) {
	astPass(pstate, name->value);
	// Infer the var's vtype from its value, if not provided
	if (name->vtype == voidType)
		name->vtype = ((TypedAstNode *)name->value)->vtype;
	// Otherwise, verify that declared type and initial value type matches
	else if (!typeCoerces(name->vtype, &name->value))
		errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
}

// Check the name declaration's AST
void nameDclPass(AstPass *pstate, NameDclAstNode *name) {
	AstNode *vtype = name->vtype;
	astPass(pstate, (AstNode*)name->perm);
	astPass(pstate, vtype);

	// Process nodes in name's initial value/code block
	if (name->value) {
		switch (pstate->pass) {
		case NameResolution:
			if (vtype->asttype == FnSig)
				nameDclFnNameResolve(pstate, name);
			break;
		case TypeCheck:
			if (vtype->asttype == FnSig) {
				// Syntactic sugar: Turn implicit returns into explicit returns
				fnImplicitReturn(((FnSigAstNode*)name->vtype)->rettype, (BlockAstNode *)name->value);
				// Do type checking of function (with fnsig as context)
				nameDclFnTypeCheck(pstate, name);
			}
			else
				nameDclVarTypeCheck(pstate, name);
			break;
		}
	}
	else if (vtype == voidType)
		errorMsgNode((AstNode*)name, ErrorNoType, "Name must specify a type");

}
