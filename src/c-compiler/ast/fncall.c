/** AST handling for function/method calls
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

#include <assert.h>

// Create a function call node
FnCallAstNode *newFnCallAstNode(AstNode *fn, int nnodes) {
	FnCallAstNode *node;
	newAstNode(node, FnCallAstNode, FnCallNode);
	node->objfn = fn;
	node->args = newNodes(nnodes);
	return node;
}

// Serialize function call node
void fnCallPrint(FnCallAstNode *node) {
	AstNode **nodesp;
	uint32_t cnt;
	astPrintNode(node->objfn);
	astFprint("(");
	for (nodesFor(node->args, cnt, nodesp)) {
		astPrintNode(*nodesp);
		if (cnt > 1)
			astFprint(", ");
	}
	astFprint(")");
}

// Analyze function call node
void fnCallPass(PassState *pstate, FnCallAstNode *node) {
	AstNode **argsp;
	uint32_t cnt;
	for (nodesFor(node->args, cnt, argsp))
        astPass(pstate, *argsp);
    astPass(pstate, node->objfn);

    switch (pstate->pass) {
    case TypeCheck:
    {
	    // If this is an object call, resolve method name within first argument's type
	    if (node->objfn->asttype == MbrNameUseTag) {
		    NameUseAstNode *methname = (NameUseAstNode*)node->objfn;
		    Name *methsym = methname->namesym;
		    FnDclAstNode *method;
            if (methsym->namestr == '_') {
                errorMsgNode((AstNode*)node, ErrorNotPublic, "May not access the private method `%s`.", &methsym->namestr);
                return;
            }
		    else if (method = methtypeFindMethod(typeGetVtype(*nodesNodes(node->args)), methsym, node->args, (AstNode*)node)) {
			    methname->asttype = VarNameUseTag;
			    methname->dclnode = (NamedAstNode*)method;
			    methname->vtype = methname->dclnode->vtype;
		    }
		    else {
			    errorMsgNode((AstNode*)node, ErrorNoMeth, "The method `%s` is not defined by the object's type.", &methsym->namestr);
			    return;
		    }
	    }

	    // Automatically deref a reference to the function
	    else
		    derefAuto(&node->objfn);

	    // Capture return vtype and ensure we are calling a function
	    AstNode *fnsig = typeGetVtype(node->objfn);
	    if (fnsig->asttype == FnSigType)
		    node->vtype = ((FnSigAstNode*)fnsig)->rettype;
	    else {
		    errorMsgNode(node->objfn, ErrorNotFn, "Cannot call a value that is not a function");
		    return;
	    }

	    // Error out if we have too many arguments
	    int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->args->used;
	    if (argsunder < 0) {
		    errorMsgNode((AstNode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
		    return;
	    }

	    // Type check that passed arguments match declared parameters
	    AstNode **argsp;
	    uint32_t cnt;
	    AstNode **parmp = &nodesGet(((FnSigAstNode*)fnsig)->parms, 0);
	    for (nodesFor(node->args, cnt, argsp)) {
		    if (!typeCoerces(*parmp, argsp))
			    errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
		    else
			    handleCopy(pstate, *argsp);
		    parmp++;
	    }

	    // If we have too few arguments, use default values, if provided
	    if (argsunder > 0) {
		    if (((VarDclAstNode*)*parmp)->value == NULL)
			    errorMsgNode((AstNode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
		    else {
			    while (argsunder--) {
				    nodesAdd(&node->args, ((VarDclAstNode*)*parmp)->value);
				    parmp++;
			    }
		    }
	    }
	    break;
    }
    }
}