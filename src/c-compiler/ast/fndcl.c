/** AST handling for function/method declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"
#include "../shared/error.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
FnDclAstNode *newFnDclNode(Name *namesym, uint16_t flags, AstNode *type, AstNode *val) {
	FnDclAstNode *name;
	newAstNode(name, FnDclAstNode, FnDclTag);
    name->flags = flags;
	name->vtype = type;
	name->owner = NULL;
	name->namesym = namesym;
	name->value = val;
	name->llvmvar = NULL;
    name->nextnode = NULL;
	return name;
}

// Serialize the AST for a variable/function
void fnDclPrint(FnDclAstNode *name) {
	astFprint("fn %s ", &name->namesym->namestr);
	astPrintNode(name->vtype);
	if (name->value) {
		astFprint(" {} ");
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
		if (isValueNode(laststmt)) {
			ReturnAstNode *retnode = newReturnNode();
			retnode->exp = laststmt;
			nodesLast(blk->stmts) = (AstNode*)retnode;
		}
		else if (laststmt->asttype != ReturnNode)
			errorMsgNode(laststmt, ErrorNoRet, "A return value is expected but this statement cannot give one.");
	}
}

// Enable resolution of fn parameter references to parameters
void fnDclNameResolve(PassState *pstate, FnDclAstNode *name) {
	int16_t oldscope = pstate->scope;
	pstate->scope = 1;
	FnSigAstNode *fnsig = (FnSigAstNode*)name->vtype;
    nametblHookPush();
    AstNode **nodesp;
    uint32_t cnt;
    for (nodesFor(fnsig->parms, cnt, nodesp))
        nametblHookNode((NamedAstNode *)*nodesp);
	astPass(pstate, name->value);
	nametblHookPop();
	pstate->scope = oldscope;
}

// Provide parameter and return type context for type checking function's logic
void fnDclTypeCheck(PassState *pstate, FnDclAstNode *varnode) {
	FnSigAstNode *oldfnsig = pstate->fnsig;
	pstate->fnsig = (FnSigAstNode*)varnode->vtype;
	astPass(pstate, varnode->value);
	pstate->fnsig = oldfnsig;
}

// Check the function declaration's AST
void fnDclPass(PassState *pstate, FnDclAstNode *name) {
	astPass(pstate, name->vtype);
	AstNode *vtype = typeGetVtype(name->vtype);

	// Process nodes in name's initial value/code block
	switch (pstate->pass) {
	case NameResolution:
		// Hook into global name table if not a global owner by module
		// (because those have already been hooked by module for forward references)
		/*if (name->owner->asttype != ModuleNode)
			namespaceHook((NamedAstNode*)name, name->namesym);*/
		if (name->value)
			fnDclNameResolve(pstate, name);
		break;

	case TypeCheck:
		if (name->value) {
			// Syntactic sugar: Turn implicit returns into explicit returns
			fnImplicitReturn(((FnSigAstNode*)name->vtype)->rettype, (BlockAstNode *)name->value);
			// Do type checking of function (with fnsig as context)
			fnDclTypeCheck(pstate, name);
		}
		else if (vtype == voidType)
			errorMsgNode((AstNode*)name, ErrorNoType, "Name must specify a type");
		break;
	}
}
