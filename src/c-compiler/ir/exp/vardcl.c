/** AST handling for variable declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"
#include "../../shared/error.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
VarDclAstNode *newVarDclNode(Name *namesym, uint16_t asttype, INode *type, PermAstNode *perm, INode *val) {
	VarDclAstNode *name;
	newNode(name, VarDclAstNode, asttype);
	name->vtype = type;
	name->owner = NULL;
	name->namesym = namesym;
	name->perm = perm;
	name->value = val;
	name->scope = 0;
	name->index = 0;
	name->llvmvar = NULL;
	return name;
}

// Serialize the AST for a variable/function
void varDclPrint(VarDclAstNode *name) {
	inodePrintNode((INode*)name->perm);
	inodeFprint("%s ", &name->namesym->namestr);
	inodePrintNode(name->vtype);
	if (name->value) {
		inodeFprint(" = ");
		if (name->value->asttype == BlockNode)
			inodePrintNL();
		inodePrintNode(name->value);
	}
}

// Enable name resolution of local variables
void varDclNameResolve(PassState *pstate, VarDclAstNode *name) {

	// Variable declaration within a block is a local variable
	if (pstate->scope > 1) {
		if (name->namesym->node && pstate->scope == ((VarDclAstNode*)name->namesym->node)->scope) {
			errorMsgNode((INode *)name, ErrorDupName, "Name is already defined. Only one allowed.");
			errorMsgNode((INode*)name->namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
		}
		else {
			name->scope = pstate->scope;
			// Add name to global name table (containing block will unhook it later)
			nametblHookNode((NamedAstNode*)name);
		}
	}

	if (name->value)
		inodeWalk(pstate, &name->value);
}

// Type check variable against its initial value
void varDclTypeCheck(PassState *pstate, VarDclAstNode *name) {
	inodeWalk(pstate, &name->value);
	// Global variables and function parameters require literal initializers
	if (name->scope <= 1 && !litIsLiteral(name->value))
		errorMsgNode(name->value, ErrorNotLit, "Variable may only be initialized with a literal.");
	// Infer the var's vtype from its value, if not provided
	if (name->vtype == voidType)
		name->vtype = ((TypedAstNode *)name->value)->vtype;
	// Otherwise, verify that declared type and initial value type matches
	else if (!typeCoerces(name->vtype, &name->value))
		errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
    typeHandleCopy(&name->value);
}

// Check the function or variable declaration's AST
void varDclPass(PassState *pstate, VarDclAstNode *name) {
	inodeWalk(pstate, (INode**)&name->perm);
	inodeWalk(pstate, &name->vtype);
	INode *vtype = typeGetVtype(name->vtype);

	// Process nodes in name's initial value/code block
	switch (pstate->pass) {
	case NameResolution:
		// Hook into global name table if not a global owner by module
		// (because those have already been hooked by module for forward references)
		/*if (name->owner->asttype != ModuleNode)
			namespaceHook((NamedAstNode*)name, name->namesym);*/
    	varDclNameResolve(pstate, name);
		break;

	case TypeCheck:
		if (name->value) {
			varDclTypeCheck(pstate, name);
		}
		else if (vtype == voidType)
			errorMsgNode((INode*)name, ErrorNoType, "Name must specify a type");
		break;
	}
}
