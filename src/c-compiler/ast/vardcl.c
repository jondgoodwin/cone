/** AST handling for variable declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/nametbl.h"
#include "../shared/error.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
NameDclAstNode *newNameDclNode(Name *namesym, uint16_t asttype, AstNode *type, PermAstNode *perm, AstNode *val) {
	NameDclAstNode *name;
	newAstNode(name, NameDclAstNode, asttype);
	name->vtype = type;
	name->perm = perm;
	name->namesym = namesym;
	name->value = val;
	name->hooklink = NULL;
	name->prevname = NULL;
	name->scope = 0;
	name->index = 0;
	name->llvmvar = NULL;
	return name;
}

// Add a compiler built-in type to the global name table as immutable, declared type name
// This gives a program's later NameUse nodes something to point to
void newNameDclNodeStr(char *namestr, uint16_t asttype, AstNode *type) {
	Name *sym;
	sym = nameFind(namestr, strlen(namestr));
	sym->node = (NamedAstNode*)newNameDclNode(sym, asttype, NULL, immPerm, type);
}

// Return true if node is one of the specialized name declaration nodes
int isNameDclNode(AstNode *node) {
	return node->asttype == VarNameDclNode || node->asttype == VtypeNameDclNode || node->asttype == PermNameDclNode || node->asttype == AllocNameDclNode;
}

// Serialize the AST for a variable/function
void nameDclPrint(NameDclAstNode *name) {
	astPrintNode((AstNode*)name->perm);
	astFprint("%s ", &name->namesym->namestr);
		astPrintNode(name->vtype);
	if (name->value) {
		astFprint(" = ");
		if (name->value->asttype == BlockNode)
			astPrintNL();
		astPrintNode(name->value);
	}
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(AstNode *rettype, BlockAstNode *blk) {
	AstNode *laststmt;
	laststmt = nodesLast(blk->stmts);
	if (rettype == voidType) {
		if (laststmt->asttype != ReturnNode)
			nodesAdd(&blk->stmts, (AstNode*) newReturnNode());
	}
	else {
		// Inject return in front of expression
		if (isExpNode(laststmt)) {
			ReturnAstNode *retnode = newReturnNode();
			retnode->exp = laststmt;
			nodesLast(blk->stmts) = (AstNode*)retnode;
		}
		else if (laststmt->asttype != ReturnNode)
			errorMsgNode(laststmt, ErrorNoRet, "A return value is expected but this statement cannot give one.");
	}
}

// Enable resolution of fn parameter references to parameters
void nameDclFnNameResolve(PassState *pstate, NameDclAstNode *name) {
	int16_t oldscope = pstate->scope;
	pstate->scope = 1;
	FnSigAstNode *fnsig = (FnSigAstNode*)name->vtype;
	inodesHook(fnsig->parms);		// Load into global name table
	astPass(pstate, name->value);
	inodesUnhook(fnsig->parms);		// Unhook from name table
	pstate->scope = oldscope;
}

// Enable name resolution of local variables
void nameDclVarNameResolve(PassState *pstate, NameDclAstNode *name) {

	// Variable declaration within a block is a local variable
	if (pstate->scope > 1) {
		name->prevname = name->namesym->node; // Latent unhooker
		name->namesym->node = (NamedAstNode*)name;
		// Capture variable's scope info and have block know about variable
		name->scope = pstate->scope;
		inodesAdd(&pstate->blk->locals, name->namesym, (AstNode*)name);
	}

	if (name->value)
		astPass(pstate, name->value);
}

// Provide parameter and return type context for type checking function's logic
void nameDclFnTypeCheck(PassState *pstate, NameDclAstNode *varnode) {
	FnSigAstNode *oldfnsig = pstate->fnsig;
	pstate->fnsig = (FnSigAstNode*)varnode->vtype;
	astPass(pstate, varnode->value);
	pstate->fnsig = oldfnsig;
}

// Type check variable against its initial value
void nameDclVarTypeCheck(PassState *pstate, NameDclAstNode *name) {
	astPass(pstate, name->value);
	// Global variables and function parameters require literal initializers
	if (name->scope <= 1 && !litIsLiteral(name->value))
		errorMsgNode(name->value, ErrorNotLit, "Variable may only be initialized with a literal.");
	// Infer the var's vtype from its value, if not provided
	if (name->vtype == voidType)
		name->vtype = ((TypedAstNode *)name->value)->vtype;
	// Otherwise, verify that declared type and initial value type matches
	else if (!typeCoerces(name->vtype, &name->value))
		errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
}

// Check the function or variable declaration's AST
void nameDclPass(PassState *pstate, NameDclAstNode *name) {
	astPass(pstate, (AstNode*)name->perm);
	astPass(pstate, name->vtype);
	AstNode *vtype = typeGetVtype(name->vtype);

	// Process nodes in name's initial value/code block
	switch (pstate->pass) {
	case NameResolution:
		// Hook into global name table if not a global owner by module
		// (because those have already been hooked by module for forward references)
		/*if (name->owner->asttype != ModuleNode)
			namespaceHook((NamedAstNode*)name, name->namesym);*/
		if (vtype->asttype == FnSig) {
			if (name->value)
				nameDclFnNameResolve(pstate, name);
		}
		else
			nameDclVarNameResolve(pstate, name);
		break;

	case TypeCheck:
		if (name->value) {
			if (vtype->asttype == FnSig) {
				// Syntactic sugar: Turn implicit returns into explicit returns
				fnImplicitReturn(((FnSigAstNode*)name->vtype)->rettype, (BlockAstNode *)name->value);
				// Do type checking of function (with fnsig as context)
				nameDclFnTypeCheck(pstate, name);
			}
			else
				nameDclVarTypeCheck(pstate, name);
		}
		else if (vtype == voidType)
			errorMsgNode((AstNode*)name, ErrorNoType, "Name must specify a type");
		break;
	}
}

// Check the value type declaration's AST
void nameVtypeDclPass(PassState *pstate, NameDclAstNode *name) {
	astPass(pstate, name->value);
}
