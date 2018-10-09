/** Handling for variable declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
VarDclNode *newVarDclNode(Name *namesym, uint16_t tag, INode *type, INode *perm, INode *val) {
	VarDclNode *name;
	newNode(name, VarDclNode, tag);
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

// Serialize a variable node
void varDclPrint(VarDclNode *name) {
	inodePrintNode((INode*)name->perm);
	inodeFprint(" %s ", &name->namesym->namestr);
	inodePrintNode(name->vtype);
	if (name->value) {
		inodeFprint(" = ");
		if (name->value->tag == BlockTag)
			inodePrintNL();
		inodePrintNode(name->value);
	}
}

// Enable name resolution of local variables
void varDclNameResolve(PassState *pstate, VarDclNode *name) {

	// Variable declaration within a block is a local variable
	if (pstate->scope > 1) {
		if (name->namesym->node && pstate->scope == ((VarDclNode*)name->namesym->node)->scope) {
			errorMsgNode((INode *)name, ErrorDupName, "Name is already defined. Only one allowed.");
			errorMsgNode((INode*)name->namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
		}
		else {
			name->scope = pstate->scope;
			// Add name to global name table (containing block will unhook it later)
			nametblHookNode((INamedNode*)name);
		}
	}

	if (name->value)
		inodeWalk(pstate, &name->value);
}

// Type check variable against its initial value
void varDclTypeCheck(PassState *pstate, VarDclNode *name) {
	inodeWalk(pstate, &name->value);
	// Global variables and function parameters require literal initializers
	if (name->scope <= 1 && !litIsLiteral(name->value))
		errorMsgNode(name->value, ErrorNotLit, "Variable may only be initialized with a literal.");
	// Infer the var's vtype from its value, if not provided
	if (name->vtype == voidType)
		name->vtype = ((ITypedNode *)name->value)->vtype;
	// Otherwise, verify that declared type and initial value type matches
	else if (!iexpCoerces(name->vtype, &name->value))
		errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
}

// Check the variable declaration node
void varDclPass(PassState *pstate, VarDclNode *name) {
	inodeWalk(pstate, (INode**)&name->perm);
	inodeWalk(pstate, &name->vtype);
	INode *vtype = iexpGetTypeDcl(name->vtype);

	// Process nodes in name's initial value/code block
	switch (pstate->pass) {
	case NameResolution:
		// Hook into global name table if not a global owner by module
		// (because those have already been hooked by module for forward references)
		/*if (name->owner->tag != ModuleTag)
			namespaceHook((INamedNode*)name, name->namesym);*/
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

// Perform data flow analysis
void varDclFlow(FlowState *fstate, VarDclNode **vardclnode) {
    flowAddVar(*vardclnode);
    if ((*vardclnode)->value)
        flowLoadValue(fstate, &((*vardclnode)->value), 1);
}